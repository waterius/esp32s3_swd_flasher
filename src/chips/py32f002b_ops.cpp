// Действия с PY32F002B через SWD: подготовка, запись регистрами по FlashPrg.c пака Puya,
// возврат регистров (design §4, §6, §8). Источники — datasheets/README.md.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "chips/ops.h"
#include "core/plan.h"
#include "port/dap.h"

namespace {

// DBGMCU (RM §22.9) и RCC (RM §8.6)
constexpr uint32_t DBG_IDCODE = 0x40015800;
constexpr uint32_t DBG_APB_FZ1 = 0x40015808;
constexpr uint32_t DBG_IWDG_STOP = 1u << 12;
constexpr uint32_t RCC_ICSCR = 0x40021004;
constexpr uint32_t RCC_APBENR1 = 0x4002103C;
constexpr uint32_t RCC_DBGEN = 1u << 27;
// Заводская область: тайминги flash для HSI 24 МГц (RM §4.5.2–4.5.6), UID (RM §4.3)
constexpr uint32_t INFO_TIMING_24MHZ = 0x1FFF011C;
constexpr uint32_t UID = 0x1FFF0000;
// FLASH (RM §4.8, FlashPrg.c)
constexpr uint32_t FLASH_KEYR = 0x40022008;
constexpr uint32_t FLASH_SR = 0x40022010;
constexpr uint32_t FLASH_CR = 0x40022014;
constexpr uint32_t FLASH_TS0 = 0x40022100;  // TS0 … PRETPE — девять слов подряд
constexpr uint32_t KEY1 = 0x45670123, KEY2 = 0xCDEF89AB;
constexpr uint32_t SR_EOP = 1u << 0, SR_WRPERR = 1u << 4, SR_BSY = 1u << 16;
constexpr uint32_t CR_PG = 1u << 0, CR_PER = 1u << 1, CR_PGSTRT = 1u << 19, CR_EOPIE = 1u << 24,
                   CR_LOCK = 1u << 31;
// IWDG (RM §18)
constexpr uint32_t IWDG_KR = 0x40003000;
// USER OTP и замок (RM §3.4)
constexpr uint32_t OTP = 0x1FFF0280;
constexpr uint32_t BSY_LIMIT_US = 50000;

struct State {
  uint32_t id;
  uint32_t fz1_saved;
  uint32_t icscr;
  chip::Timing timing;
  uint32_t ts_before[9];  // регистры таймингов до загрузки — вернуть при «запереть»
  bool ts_factory;        // регистры после сброса совпали с заводскими значениями
  uint8_t uid[16];
};
State st;
char reason[96];

bool wait_not_busy(uint32_t *busy_us, const char **err) {
  uint32_t t0 = micros(), sr = SR_BSY;
  while (micros() - t0 < BSY_LIMIT_US) {
    if (!dap::read32(FLASH_SR, &sr)) return *err = dap::last_error(), false;
    if (!(sr & SR_BSY)) {
      if (busy_us) *busy_us = micros() - t0;
      return true;
    }
  }
  *err = "BSY дольше 50 мс";
  return false;
}

// После операции: WRPERR, EOP; флаги сбросить, режим снять.
bool finish(uint32_t mode_bits, const char **err) {
  uint32_t sr = 0, cr = 0;
  bool ok = dap::read32(FLASH_SR, &sr);
  const char *why = !ok ? dap::last_error() : (sr & SR_WRPERR) ? "WRPERR" : !(sr & SR_EOP) ? "нет EOP" : nullptr;
  dap::write32(FLASH_SR, SR_EOP | SR_WRPERR);  // сброс записью 1
  if (dap::read32(FLASH_CR, &cr)) dap::write32(FLASH_CR, cr & ~(mode_bits | CR_EOPIE | CR_PGSTRT));
  if (why) *err = why;
  return why == nullptr;
}

bool start_mode(uint32_t mode_bits, const char **err) {
  uint32_t cr;
  if (!wait_not_busy(nullptr, err)) return false;
  if (!dap::write32(FLASH_SR, SR_EOP | SR_WRPERR) || !dap::read32(FLASH_CR, &cr) ||
      !dap::write32(FLASH_CR, cr | mode_bits | CR_EOPIE))
    return *err = dap::last_error(), false;
  return true;
}

const char *prepare(const chip::Desc &desc) {
  memset(&st, 0, sizeof st);
  // 1. Идентификатор; модуль DBG без тактирования читается нулём (RM §8.6.14)
  if (!dap::read32(DBG_IDCODE, &st.id)) return "чтение DBG_IDCODE не удалось";
  uint32_t apbenr;
  if (!dap::read32(RCC_APBENR1, &apbenr) || !dap::write32(RCC_APBENR1, apbenr | RCC_DBGEN))
    return "не включается DBGEN";
  if (st.id == 0 && !dap::read32(DBG_IDCODE, &st.id)) return "чтение DBG_IDCODE не удалось";
  // 2. Тот ли чип
  if (!plan::id_matches(desc, st.id)) {
    snprintf(reason, sizeof reason, "на плате не %s: 0x%08lX", desc.name, (unsigned long)st.id);
    return reason;
  }
  // 3–4. Сторож стоит, пока стоит ядро
  uint32_t fz1;
  if (!dap::read32(DBG_APB_FZ1, &st.fz1_saved) || !dap::write32(DBG_APB_FZ1, st.fz1_saved | DBG_IWDG_STOP) ||
      !dap::read32(DBG_APB_FZ1, &fz1) || !(fz1 & DBG_IWDG_STOP))
    return "DBG_IWDG_STOP не встаёт: сторож сбросит чип посреди записи";
  // 5. Частота HSI и заводские тайминги
  uint32_t info[5], ts[9];
  if (!dap::read32(RCC_ICSCR, &st.icscr) || !dap::read_block(INFO_TIMING_24MHZ, info, 5) ||
      !dap::read_block(FLASH_TS0, ts, 9))
    return "чтение таймингов не удалось";
  st.timing = desc.timing(st.icscr, info);
  if (!st.timing.ok) return st.timing.reason;
  st.ts_factory = memcmp(ts, st.timing.regs, sizeof ts) == 0;
  // 6. UID
  if (!dap::read_bytes(UID, st.uid, sizeof st.uid)) return "чтение UID не удалось";
  return nullptr;
}

void restore() {
  uint32_t cr;
  if (dap::read32(FLASH_CR, &cr)) dap::write32(FLASH_CR, cr | CR_LOCK);
  dap::write32(DBG_APB_FZ1, st.fz1_saved);  // пока DBGEN включён; RCC_APBENR1 сбросит NRST
}

bool unlock(const char **err) {
  uint32_t cr;
  if (!dap::read32(FLASH_CR, &cr)) return *err = dap::last_error(), false;
  if (cr & CR_LOCK) {  // повторные ключи в отпертый KEYR — неверная последовательность (RM §4.2.3.1)
    if (!dap::write32(FLASH_KEYR, KEY1) || !dap::write32(FLASH_KEYR, KEY2) || !dap::read32(FLASH_CR, &cr))
      return *err = dap::last_error(), false;
    if (cr & CR_LOCK) return *err = "ключи FLASH_KEYR не приняты", false;
  }
  // тайминги — после ключей, как у Black Magic Debug и RM §4.2.3.6
  if (!dap::read_block(FLASH_TS0, st.ts_before, 9) || !dap::write_block(FLASH_TS0, st.timing.regs, 9))
    return *err = "тайминги не записались", false;
  return true;
}

void lock() {
  dap::write_block(FLASH_TS0, st.ts_before, 9);
  uint32_t cr;
  if (dap::read32(FLASH_CR, &cr)) dap::write32(FLASH_CR, cr | CR_LOCK);
}

bool erase_unit(uint32_t addr, uint32_t *busy_us, const char **err) {
  if (!start_mode(CR_PER, err)) return false;
  if (!dap::write32(addr, 0xFFFFFFFF)) {  // любое слово страницы запускает стирание
    *err = dap::last_error();
    finish(CR_PER, err);
    return false;
  }
  if (!wait_not_busy(busy_us, err)) {
    const char *why = *err;
    finish(CR_PER, err);
    return *err = why, false;
  }
  return finish(CR_PER, err);
}

bool write_unit(uint32_t addr, const uint8_t *data, uint32_t len, uint32_t *busy_us, const char **err) {
  if (len != 128) return *err = "единица не 128 Б", false;
  // второй рубеж от замка USER OTP — что бы ни решило правило плана (design §6)
  if (addr == OTP && data[0] == 0x55 && data[1] == 0xAA) return *err = "замок OTP не ставится", false;
  uint32_t words[32];
  memcpy(words, data, sizeof words);
  if (!start_mode(CR_PG, err)) return false;
  uint32_t cr;
  bool ok = dap::write_block(addr, words, 31) && dap::read32(FLASH_CR, &cr) &&
            dap::write32(FLASH_CR, cr | CR_PGSTRT) &&  // перед последним словом (FlashPrg.c)
            dap::write32(addr + 124, words[31]);
  if (!ok) {
    *err = dap::last_error();
    finish(CR_PG, err);
    return false;
  }
  if (!wait_not_busy(busy_us, err)) {
    const char *why = *err;
    finish(CR_PG, err);
    return *err = why, false;
  }
  return finish(CR_PG, err);
}

void reload_watchdog() { dap::write32(IWDG_KR, 0xAAAA); }

void info(gdb::LineFn emit, void *ectx) {
  char line[96];
  snprintf(line, sizeof line, "# DBG_IDCODE: 0x%08lX", (unsigned long)st.id);
  emit(ectx, line);
  int n = snprintf(line, sizeof line, "# UID: 0x");
  for (int i = 15; i >= 0 && n > 0 && (size_t)n < sizeof line; i--) n += snprintf(line + n, sizeof line - n, "%02X", st.uid[i]);
  emit(ectx, line);
  emit(ectx, st.ts_factory ? "# HSI 24 МГц, тайминги заводские" : "# HSI 24 МГц, тайминги в регистрах не заводские");
}

}  // namespace

namespace chips {

const Ops py32f002b_ops = {prepare, restore, unlock, lock, erase_unit, write_unit, reload_watchdog, info};

}  // namespace chips
