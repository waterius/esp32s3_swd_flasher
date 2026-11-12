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

constexpr uint32_t NRST_PULSE_MS = 2;     // с RC на NRST нога должна успеть дойти до нуля
constexpr uint32_t RESTART_LIMIT_MS = 100;  // сколько ловить чип, чтобы снять отладку

Hooks hooks;
const chip::Desc *desc = nullptr;
const chips::Ops *ops = nullptr;
Method method = Method::None;
uint32_t idcode = 0;
bool is_active = false;
bool attempted = false;  // ноги и NRST трогали — в конце запустить плату без отладки
bool prepared = false;   // ops->prepare менял регистры чипа — вернуть
bool flashed = false;    // в сеансе была запись — вывести общее время
uint32_t started_ms = 0;
plan::Plan pl;
writer::Report rep;

void print(const char *line) {
  if (hooks.print) hooks.print(line);
}

// Строка о сеансе — в вывод оснастки и, если команда пришла из gdb, ещё и в gdb.
void say(const char *line, gdb::LineFn emit, void *ectx) {
  print(line);
  if (emit) emit(ectx, line);
}

// Ждать остановки ядра. Сброс мог сбросить и порт отладки — тогда войти в SWD заново.
bool wait_halt(uint32_t limit_us) {
  uint32_t t0 = micros(), v, id;
  while (micros() - t0 < limit_us) {
    if (dap::read32(dap::DHCSR, &v)) {
      if (v & dap::DHCSR_S_HALT) return true;
    } else {
      dap::connect(&id);
    }
  }
  return false;
}

// Способ 1: под сбросом — ядро встаёт на первой инструкции, прошивка платы не делает ни такта.
bool connect_under_reset() {
  dap::nrst(true);
  delay(NRST_PULSE_MS);
  if (!dap::connect(&idcode)) return false;
  if (!dap::write32(dap::DEMCR, 1) || !dap::write32(dap::DHCSR, 0xA05F0001)) return false;  // VC_CORERESET, C_DEBUGEN
  dap::nrst(false);
  return wait_halt(100000);
}

// Способ 2: сразу после сброса — ловить чип, пока прошивка платы не уснула.
bool connect_after_reset() {
  dap::nrst(true);
  delay(NRST_PULSE_MS);
  dap::nrst(false);
  uint32_t t0 = micros();
  while (micros() - t0 < 50000) {
    if (!dap::connect(&idcode)) continue;
    if (!dap::write32(dap::DHCSR, 0xA05F0003) || !wait_halt(10000)) continue;  // C_DEBUGEN | C_HALT
    // перезапуск в остановку на первой инструкции: периферия — как после сброса
    if (!dap::write32(dap::DEMCR, 1)) continue;
    dap::write32(dap::AIRCR, 0x05FA0004);  // ответ может пропасть вместе со сбросом — не ошибка
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

// Снять отладку: перехват сброса, остановку и C_DEBUGEN, затем питание отладки.
bool clear_debug() {
  uint32_t v;
  if (!dap::write32(dap::DEMCR, 0) || !dap::write32(dap::DHCSR, 0xA05F0000)) return false;
  if (!dap::read32(dap::DHCSR, &v) || (v & 1)) return false;  // C_DEBUGEN должен быть 0
  dap::power_down();
  return true;
}

// Запустить плату с начала без отладки — design §5, при любом исходе, в том числе после
// неудачного подключения: DEMCR и DHCSR сбрасывает только POR. Сначала под сбросом; если порт
// отладки под сбросом молчит — сразу после: ядро ждёт (C_HALT или VC_CORERESET переживают NRST).
void restart_without_debug() {
  uint32_t id;
  dap::nrst(true);
  delay(NRST_PULSE_MS);
  bool cleared = dap::connect(&id) && clear_debug();
  if (hooks.log_clear) hooks.log_clear();  // принятое до запуска платы — мусор
  dap::nrst(false);
  for (uint32_t t0 = millis(); !cleared && millis() - t0 < RESTART_LIMIT_MS;) cleared = dap::connect(&id) && clear_debug();
}

void finish() {
  bool was_active = is_active;
  if (prepared && ops) ops->restore();
  if (attempted) restart_without_debug();
  dap::release();
  if (was_active && flashed) {
    char line[48];
    snprintf(line, sizeof line, "# сеанс: %lu мс", (unsigned long)(millis() - started_ms));
    print(line);
  }
  method = Method::None;
  is_active = attempted = prepared = flashed = false;
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
bool w_erase_main(void *, uint32_t addr, uint32_t *busy, const char **err) { return ops->erase_main(addr, busy, err); }
const writer::Ops WRITE_OPS = {nullptr, w_unlock, w_lock, w_erase, w_write, w_reload, w_read, w_now, w_erase_main};

bool plan_read(void *, uint32_t addr, uint8_t *buf, uint32_t len) { return dap::read_bytes(addr, buf, len); }

// Куда идут строки шага: вывод оснастки и, если emit не nullptr, ещё и gdb.
struct Say {
  gdb::LineFn emit;
  void *ectx;
};
void say_line(void *ctx, const char *line) {
  const Say *s = static_cast<const Say *>(ctx);
  say(line, s->emit, s->ectx);
}

// Шаг с чипом (запись образа или полное стирание) — общие начало и конец: строка частоты,
// отметка сброса; после — проверка сброса и строка «# итог:», которую разбирает страница.
uint32_t step_begin(Say &to) {
  flashed = true;
  char line[160];
  snprintf(line, sizeof line, "# %s", ops->timing_text());
  say_line(&to, line);
  dap::clear_reset_seen();
  return millis();
}

// what — «прошивки» или «стирания»: «# сброс во время прошивки».
bool step_end(Say &to, uint32_t t0, const char *what) {
  bool ok = rep.result == writer::Result::Ok;
  uint32_t dhcsr;
  dap::read32(dap::DHCSR, &dhcsr);  // накопить S_RESET_ST
  char reason[120], line[160];
  if (dap::reset_seen()) {
    snprintf(reason, sizeof reason, "сброс во время %s", what);
    snprintf(line, sizeof line, "# %s", reason);
    say_line(&to, line);
    ok = false;
  } else if (!ok) {
    writer::failure_text(rep, reason, sizeof reason);
  }
  if (ok) snprintf(line, sizeof line, "# итог: ок, %lu мс", (unsigned long)(millis() - t0));
  else snprintf(line, sizeof line, "# итог: ошибка, %s", reason);
  say_line(&to, line);
  return ok;
}

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
  ops = nullptr;  // регистры чипа после prepare неизвестны — возвращаем только отладку ядра
  if (hooks.log_pause) hooks.log_pause();
  attempted = true;
  finish();
  print("# сеанс прерван перезапуском оснастки: плата запущена заново");
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
  started_ms = millis();
  attempted = true;
  char line[160];
  if (!connect()) {
    finish();
    say("# чип не отвечает", emit, ectx);
    return false;
  }
  snprintf(line, sizeof line, "# SWD: %s, IDCODE 0x%08lx, %lu мс",
           method == Method::UnderReset ? "под сбросом" : "после сброса", (unsigned long)idcode,
           (unsigned long)(millis() - started_ms));
  say(line, emit, ectx);
  prepared = true;
  const char *why = ops->prepare(d);
  if (why) {
    snprintf(line, sizeof line, "# %s", why);
    say(line, emit, ectx);
    finish();
    return false;
  }
  is_active = true;
  return true;
}

void end() {
  if (is_active || attempted) finish();
}

void info(gdb::LineFn emit, void *ectx) {
  char line[96];
  snprintf(line, sizeof line, "# чип: %s, SWD %s", desc ? desc->name : "?",
           method == Method::UnderReset ? "под сбросом" : "после сброса");
  emit(ectx, line);
  if (ops) ops->info(emit, ectx);
}

bool read_mem(uint32_t addr, uint8_t *buf, uint32_t len) { return is_active && dap::read_bytes(addr, buf, len); }
bool read_reg(uint8_t index, uint32_t *value) { return is_active && dap::read_core_reg(index, value); }
bool write_reg(uint8_t index, uint32_t value) { return is_active && dap::write_core_reg(index, value); }

bool flash(const image::Image &img) {
  if (!is_active || !desc) return false;
  flashed = true;
  plan::make(pl, img, *desc, plan_read, nullptr);
  if (!pl.ok) {
    char line[160];
    if (pl.addr) snprintf(line, sizeof line, "# %s: 0x%08lX", pl.reason, (unsigned long)pl.addr);
    else snprintf(line, sizeof line, "# %s", pl.reason);
    print(line);
    snprintf(line, sizeof line, "# итог: ошибка, %s", pl.reason);
    print(line);
    return false;
  }
  Say to{nullptr, nullptr};  // строки записи gdb не нужны: придут после сеанса
  uint32_t t0 = step_begin(to);
  writer::run(rep, img, pl, *desc, WRITE_OPS);
  writer::describe(rep, say_line, &to);
  return step_end(to, t0, "прошивки");
}

bool erase_main(gdb::LineFn emit, void *ectx) {
  if (!is_active || !desc) return false;
  Say to{emit, ectx};
  uint32_t t0 = step_begin(to);
  writer::erase_main(rep, *desc, WRITE_OPS);
  writer::describe_erase(rep, say_line, &to);
  return step_end(to, t0, "стирания");
}

void reset_board() {
  if (is_active) return;
  dap::nrst(true);
  delay(10);
  dap::nrst(false);
}

}  // namespace session
