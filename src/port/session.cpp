#include "port/session.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <stdio.h>

#include "chips/ops.h"
#include "core/plan.h"
#include "core/write.h"
#include "port/dap.h"
#include "port/swd.h"

namespace session {

namespace {

enum class Method : uint8_t { None, UnderReset, AfterReset };

constexpr uint32_t MARKER = 0x5E551011;  // «идёт сеанс» в RTC-памяти: переживает сбой оснастки
RTC_NOINIT_ATTR uint32_t rtc_marker;

Hooks hooks;
const chip::Desc *desc = nullptr;
const chips::Ops *ops = nullptr;
Method method = Method::None;
uint32_t idcode = 0;
bool is_active = false;
plan::Plan pl;
writer::Report rep;

void print(const char *line) {
  if (hooks.print) hooks.print(line);
}

bool wait_halt(uint32_t limit_us) {
  uint32_t t0 = micros(), v;
  while (micros() - t0 < limit_us)
    if (dap::read32(dap::DHCSR, &v) && (v & dap::DHCSR_S_HALT)) return true;
  return false;
}

// Способ 1: под сбросом — ядро встаёт на первой инструкции, прошивка платы не делает ни такта.
bool connect_under_reset() {
  dap::nrst(true);
  delay(2);
  if (!dap::connect(&idcode)) return false;
  if (!dap::write32(dap::DEMCR, 1) || !dap::write32(dap::DHCSR, 0xA05F0001)) return false;  // VC_CORERESET, C_DEBUGEN
  dap::nrst(false);
  return wait_halt(100000);
}

// Способ 2: сразу после сброса — ловить чип, пока прошивка платы не уснула.
bool connect_after_reset() {
  dap::nrst(true);
  delay(2);
  dap::nrst(false);
  uint32_t t0 = micros();
  while (micros() - t0 < 50000) {
    if (!dap::connect(&idcode)) continue;
    if (!dap::write32(dap::DHCSR, 0xA05F0003) || !wait_halt(10000)) continue;  // C_DEBUGEN | C_HALT
    // перезапуск в остановку на первой инструкции: периферия — как после сброса
    if (!dap::write32(dap::DEMCR, 1) || !dap::write32(dap::AIRCR, 0x05FA0004)) return false;
    delay(1);
    return wait_halt(100000);
  }
  return false;
}

bool connect() {
#ifndef SWD_CONNECT_AFTER_RESET_ONLY
  if (connect_under_reset()) return method = Method::UnderReset, true;
#endif
  if (connect_after_reset()) return method = Method::AfterReset, true;
  return false;
}

// Завершение — design §5, при любом исходе.
void finish() {
  if (method != Method::None) {
    if (ops) ops->restore();
    dap::write32(dap::DEMCR, 0);
    if (hooks.log_pause) hooks.log_pause();  // принятое до запуска платы — мусор
    dap::nrst(true);
    if (method == Method::UnderReset) {
      dap::write32(dap::DHCSR, 0xA05F0000);
      dap::power_down();
      dap::nrst(false);
    } else {
      dap::nrst(false);
      // ядро стоит на первой инструкции: C_HALT переживает сброс; порт мог сброситься
      uint32_t id;
      if (!dap::write32(dap::DHCSR, 0xA05F0000)) {
        dap::connect(&id);
        dap::write32(dap::DHCSR, 0xA05F0000);
      }
      dap::power_down();
    }
  }
  dap::release();
  method = Method::None;
  is_active = false;
  rtc_marker = 0;
  if (hooks.log_resume) hooks.log_resume();
}

// ---------------------------------------------------------------- функции чипа для core/write

bool w_unlock(void *, const char **err) { return ops->unlock(err); }
void w_lock(void *) { ops->lock(); }
bool w_erase(void *, uint32_t addr, uint32_t *busy, const char **err) { return ops->erase_unit(addr, busy, err); }
bool w_write(void *, uint32_t addr, const uint8_t *d, uint32_t len, uint32_t *busy, const char **err) {
  return ops->write_unit(addr, d, len, busy, err);
}
void w_reload(void *) { ops->reload_watchdog(); }
bool w_read(void *, uint32_t addr, uint8_t *buf, uint32_t len) { return dap::read_bytes(addr, buf, len); }
uint32_t w_now(void *) { return micros(); }
const writer::Ops WRITE_OPS = {nullptr, w_unlock, w_lock, w_erase, w_write, w_reload, w_read, w_now};

bool plan_read(void *, uint32_t addr, uint8_t *buf, uint32_t len) { return dap::read_bytes(addr, buf, len); }

void print_line(void *, const char *line) { print(line); }

}  // namespace

void begin(const Hooks &h) {
  hooks = h;
  dap::release();
}

void recover(const chip::Desc *d) {
  if (rtc_marker != MARKER) return;
  rtc_marker = 0;
  if (!d) return;
  desc = d;
  ops = chips::ops_for(*d);
  if (hooks.log_pause) hooks.log_pause();
  if (connect()) print("# сеанс прерван перезапуском оснастки: плата запущена");
  finish();
}

bool active() { return is_active; }

bool start(const chip::Desc &d, gdb::LineFn emit, void *ectx) {
  if (is_active) return true;
  desc = &d;
  ops = chips::ops_for(d);
  if (!ops) return false;
  rtc_marker = MARKER;
  if (hooks.log_pause) hooks.log_pause();
  dap::clear_reset_seen();
  uint32_t t0 = millis();
  char line[96];
  if (!connect()) {
    method = Method::None;
    finish();
    snprintf(line, sizeof line, "# чип не отвечает");
    emit ? emit(ectx, line) : print(line);
    return false;
  }
  snprintf(line, sizeof line, "# SWD: %s, IDCODE 0x%08lx, %lu мс",
           method == Method::UnderReset ? "под сбросом" : "после сброса", (unsigned long)idcode,
           (unsigned long)(millis() - t0));
  emit ? emit(ectx, line) : print(line);
  const char *why = ops->prepare(d);
  if (why) {
    snprintf(line, sizeof line, "# %s", why);
    emit ? emit(ectx, line) : print(line);
    finish();
    return false;
  }
  is_active = true;
  return true;
}

void end() {
  if (is_active || method != Method::None) finish();
}

void info(gdb::LineFn emit, void *ectx) {
  char line[96];
  snprintf(line, sizeof line, "# чип: %s, SWD %s, IDCODE 0x%08lx", desc ? desc->name : "?",
           method == Method::UnderReset ? "под сбросом" : "после сброса", (unsigned long)idcode);
  emit(ectx, line);
  if (ops) ops->info(emit, ectx);
}

bool read_mem(uint32_t addr, uint8_t *buf, uint32_t len) { return is_active && dap::read_bytes(addr, buf, len); }
bool read_reg(uint8_t index, uint32_t *value) { return is_active && dap::read_core_reg(index, value); }
bool write_reg(uint8_t index, uint32_t value) { return is_active && dap::write_core_reg(index, value); }

bool flash(const image::Image &img) {
  if (!is_active || !desc) return false;
  uint32_t t0 = millis();
  char line[160];
  plan::make(pl, img, *desc, plan_read, nullptr);
  if (!pl.ok) {
    if (pl.addr) snprintf(line, sizeof line, "# %s: 0x%08lX", pl.reason, (unsigned long)pl.addr);
    else snprintf(line, sizeof line, "# %s", pl.reason);
    print(line);
    snprintf(line, sizeof line, "# итог: ошибка, %s", pl.reason);
    print(line);
    return false;
  }
  dap::clear_reset_seen();
  writer::run(rep, img, pl, *desc, WRITE_OPS);
  writer::describe(rep, print_line, nullptr);
  bool ok = rep.result == writer::Result::Ok;
  uint32_t dhcsr;
  dap::read32(dap::DHCSR, &dhcsr);  // накопить S_RESET_ST
  if (dap::reset_seen()) {
    print("# сброс во время прошивки");
    ok = false;
  }
  if (ok) {
    snprintf(line, sizeof line, "# итог: ок, %lu мс", (unsigned long)(millis() - t0));
  } else {
    char reason[120];
    if (dap::reset_seen()) snprintf(reason, sizeof reason, "сброс во время прошивки");
    else writer::failure_text(rep, reason, sizeof reason);
    snprintf(line, sizeof line, "# итог: ошибка, %s", reason);
  }
  print(line);
  return ok;
}

void reset_board() {
  if (is_active) return;
  dap::nrst(true);
  delay(10);
  dap::nrst(false);
}

}  // namespace session
