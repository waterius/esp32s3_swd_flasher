#include "port/dap.h"

#include <Arduino.h>

#include "pins.h"
#include "port/swd.h"

namespace dap {

namespace {

// Регистры порта отладки (DP) и MEM-AP, банк 0.
constexpr uint8_t DP_IDCODE = 0x0, DP_ABORT = 0x0, DP_CTRL_STAT = 0x4, DP_SELECT = 0x8, DP_RDBUFF = 0xC;
constexpr uint8_t AP_CSW = 0x0, AP_TAR = 0x4, AP_DRW = 0xC;
constexpr uint32_t CSW_WORD_AUTOINC = 0x23000052;  // 32 бита, автоинкремент, design §3
constexpr uint32_t CTRL_POWERUP = 0x50000000;      // CSYSPWRUPREQ | CDBGPWRUPREQ
constexpr uint32_t CTRL_POWERUP_ACK = 0xA0000000;  // CSYSPWRUPACK | CDBGPWRUPACK
constexpr uint32_t CTRL_STICKYERR = 1u << 5;
constexpr uint32_t ABORT_CLEAR = 0x1E;     // STKERRCLR | WDERRCLR | ORUNERRCLR | STKCMPCLR
constexpr uint32_t ABORT_DAPABORT = 0x1F;  // плюс DAPABORT
constexpr uint32_t WAIT_LIMIT_US = 10000;

const char *error = "";
bool reset_flag = false;

bool xfer(bool ap, bool read, uint8_t a, uint32_t *data) {
  uint32_t t0 = micros();
  for (;;) {
    swd::Ack ack = swd::transfer(ap, read, a, data);
    if (ack == swd::Ack::Ok) return true;
    if (ack == swd::Ack::Wait && micros() - t0 < WAIT_LIMIT_US) continue;
    uint32_t abort = ack == swd::Ack::Wait ? ABORT_DAPABORT : ABORT_CLEAR;
    if (ack != swd::Ack::Error) swd::transfer(false, false, DP_ABORT, &abort);
    error = ack == swd::Ack::Wait ? "WAIT дольше 10 мс" : ack == swd::Ack::Fault ? "FAULT" : "нет ответа SWD";
    return false;
  }
}

bool dp_write(uint8_t a, uint32_t v) { return xfer(false, false, a, &v); }
bool dp_read(uint8_t a, uint32_t *v) { return xfer(false, true, a, v); }
bool ap_write(uint8_t a, uint32_t v) { return xfer(true, false, a, &v); }
bool ap_read(uint8_t a, uint32_t *v) { return xfer(true, true, a, v); }

// После пакета обращений: STICKYERR — прочитанному верить нельзя.
bool sticky_ok() {
  uint32_t ctrl;
  if (!dp_read(DP_CTRL_STAT, &ctrl)) return false;
  if (ctrl & CTRL_STICKYERR) {
    uint32_t abort = ABORT_CLEAR;
    swd::transfer(false, false, DP_ABORT, &abort);
    error = "STICKYERR";
    return false;
  }
  return true;
}

void note_dhcsr(uint32_t addr, uint32_t v) {
  if (addr == DHCSR && (v & DHCSR_S_RESET_ST)) reset_flag = true;
}

// Слов до границы 1 КБ: автоинкремент TAR гарантирован только внутри неё.
uint32_t words_to_boundary(uint32_t addr) { return (0x400 - (addr & 0x3FF)) / 4; }

bool wait_regrdy() {
  uint32_t t0 = micros();
  uint32_t v;
  do {
    if (!read32(DHCSR, &v)) return false;
    if (v & DHCSR_S_REGRDY) return true;
  } while (micros() - t0 < 10000);
  error = "регистр ядра не готов";
  return false;
}

}  // namespace

const char *last_error() { return error; }

bool connect(uint32_t *idcode) {
  swd::begin();
  swd::line_reset();
  swd::jtag_to_swd();
  swd::line_reset();
  swd::idle(2);
  if (!dp_read(DP_IDCODE, idcode)) return false;
  if (!dp_write(DP_ABORT, ABORT_CLEAR) || !dp_write(DP_CTRL_STAT, CTRL_POWERUP)) return false;
  uint32_t t0 = micros(), ctrl = 0;
  do {
    if (!dp_read(DP_CTRL_STAT, &ctrl)) return false;
  } while ((ctrl & CTRL_POWERUP_ACK) != CTRL_POWERUP_ACK && micros() - t0 < 100000);
  if ((ctrl & CTRL_POWERUP_ACK) != CTRL_POWERUP_ACK) {
    error = "отладка не включилась";
    return false;
  }
  return dp_write(DP_SELECT, 0) && ap_write(AP_CSW, CSW_WORD_AUTOINC);
}

bool power_down() { return dp_write(DP_CTRL_STAT, 0); }

bool read32(uint32_t addr, uint32_t *value) {
  uint32_t dummy;
  if (!ap_write(AP_TAR, addr) || !ap_read(AP_DRW, &dummy) || !dp_read(DP_RDBUFF, value)) return false;
  note_dhcsr(addr, *value);
  return true;
}

bool write32(uint32_t addr, uint32_t value) {
  uint32_t done;
  return ap_write(AP_TAR, addr) && ap_write(AP_DRW, value) && dp_read(DP_RDBUFF, &done);
}

bool read_block(uint32_t addr, uint32_t *words, uint32_t count) {
  while (count) {
    uint32_t n = words_to_boundary(addr);
    if (n > count) n = count;
    uint32_t dummy;
    if (!ap_write(AP_TAR, addr) || !ap_read(AP_DRW, &dummy)) return false;  // первое чтение — отложенное
    for (uint32_t i = 1; i < n; i++)
      if (!ap_read(AP_DRW, &words[i - 1])) return false;
    if (!dp_read(DP_RDBUFF, &words[n - 1])) return false;
    addr += 4 * n;
    words += n;
    count -= n;
  }
  return sticky_ok();
}

bool write_block(uint32_t addr, const uint32_t *words, uint32_t count) {
  while (count) {
    uint32_t n = words_to_boundary(addr);
    if (n > count) n = count;
    if (!ap_write(AP_TAR, addr)) return false;
    for (uint32_t i = 0; i < n; i++)
      if (!ap_write(AP_DRW, words[i])) return false;
    addr += 4 * n;
    words += n;
    count -= n;
  }
  uint32_t done;
  return dp_read(DP_RDBUFF, &done) && sticky_ok();
}

bool read_bytes(uint32_t addr, uint8_t *buf, uint32_t len) {
  static uint32_t words[0x800 / 4 + 2];
  uint32_t first = addr & ~3u;
  uint32_t count = (addr + len - first + 3) / 4;
  if (count > sizeof words / sizeof words[0]) return false;
  if (!read_block(first, words, count)) return false;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(words);  // ESP32-S3 — little-endian, как и Cortex-M
  for (uint32_t i = 0; i < len; i++) buf[i] = bytes[addr - first + i];
  return true;
}

bool read_core_reg(uint8_t index, uint32_t *value) {
  return write32(DCRSR, index) && wait_regrdy() && read32(DCRDR, value);
}

bool write_core_reg(uint8_t index, uint32_t value) {
  return write32(DCRDR, value) && write32(DCRSR, (uint32_t)index | 1u << 16) && wait_regrdy();
}

bool reset_seen() { return reset_flag; }
void clear_reset_seen() { reset_flag = false; }

void nrst(bool low) {
  if (low) {
    digitalWrite(PIN_NRST, LOW);
    pinMode(PIN_NRST, OUTPUT_OPEN_DRAIN);
    digitalWrite(PIN_NRST, LOW);
  } else {
    pinMode(PIN_NRST, INPUT);
  }
}

void release() {
  swd::release();
  nrst(false);
}

}  // namespace dap
