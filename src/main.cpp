// Точка сборки программатора Jaffa Flasher: USB CDC и DTR, NVS, лог платы на UART1, функции цели для core/gdb и
// хозяина для core/commands (design §1, §10, §12). Единственное место, где всё встречается.
#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>

#include "chips/chips.h"
#include "core/commands.h"
#include "core/console.h"
#include "core/gdb.h"
#include "core/image.h"
#include "esp32-hal-tinyusb.h"
#include "pins.h"
#include "port/session.h"

namespace {

Preferences prefs;
const chip::Desc *selected = nullptr;
commands::LogFormat log_format = {115200, 8, 'N', 1};
bool log_on = true;
image::Image img;  // около 32 КБ — не на стеке

// ---------------------------------------------------------------- USB

bool dtr() { return tud_cdc_n_get_line_state(0) & 0x01; }  // DTR без RTS (Context, USB CDC)

void usb_write(void *, const uint8_t *data, size_t len) {
  while (len && dtr()) {  // write() может принять часть
    size_t n = Serial.write(data, len);
    data += n;
    len -= n;
    if (!n) delay(1);
  }
}

// ---------------------------------------------------------------- выбор чипа и лог

void set_chip(void *, const chip::Desc *d) {
  selected = d;
  prefs.putString("chip", d->name);
}
const chip::Desc *get_chip(void *) { return selected; }

// SERIAL_* ядра по формату: [биты 5–8][чётность N, E, O][стоп 1, 2].
uint32_t serial_config(const commands::LogFormat &f) {
  static const uint32_t CFG[4][3][2] = {
      {{SERIAL_5N1, SERIAL_5N2}, {SERIAL_5E1, SERIAL_5E2}, {SERIAL_5O1, SERIAL_5O2}},
      {{SERIAL_6N1, SERIAL_6N2}, {SERIAL_6E1, SERIAL_6E2}, {SERIAL_6O1, SERIAL_6O2}},
      {{SERIAL_7N1, SERIAL_7N2}, {SERIAL_7E1, SERIAL_7E2}, {SERIAL_7O1, SERIAL_7O2}},
      {{SERIAL_8N1, SERIAL_8N2}, {SERIAL_8E1, SERIAL_8E2}, {SERIAL_8O1, SERIAL_8O2}},
  };
  int p = f.parity == 'E' ? 1 : f.parity == 'O' ? 2 : 0;
  return CFG[f.data_bits - 5][p][f.stop_bits - 1];
}

void log_uart_begin() {
  Serial1.begin(log_format.baud, serial_config(log_format), PIN_LOG_RX, -1);
  gpio_pullup_en((gpio_num_t)PIN_LOG_RX);
}

commands::LogFormat get_log(void *) { return log_format; }
// Формат кадра updateBaudRate не меняет — UART заново (jaffa-flasher-page design §7).
void set_log(void *, const commands::LogFormat &f) {
  log_format = f;
  Serial1.end();
  log_uart_begin();
}

void log_pause() { log_on = false; }
void log_clear() {
  while (Serial1.available()) Serial1.read();
}
void log_resume() { log_on = true; }

void print(const char *line);  // строка программатора в вывод — ниже, после консоли

// ---------------------------------------------------------------- цель для gdb

bool t_active(void *) { return session::active(); }
// Строки сеанса сессия всегда выводит сама; отдельно в emit — только если команда из gdb.
bool t_start(void *, gdb::LineFn emit, void *ectx) {
  if (emit == console::Console::emit) emit = nullptr;
  return selected && session::start(*selected, emit, ectx);
}
void t_end(void *) { session::end(); }
bool t_read(void *, uint32_t a, uint8_t *b, uint32_t n) { return session::read_mem(a, b, n); }
bool t_rreg(void *, uint8_t i, uint32_t *v) { return session::read_reg(i, v); }
bool t_wreg(void *, uint8_t i, uint32_t v) { return session::write_reg(i, v); }
bool t_flash(void *, const image::Image &im) { return session::flash(im); }
void t_info(void *, gdb::LineFn emit, void *ectx) { session::info(emit, ectx); }
void t_reset(void *) { session::reset_board(); }
bool t_erase(void *, gdb::LineFn emit, void *ectx) {
  if (emit == console::Console::emit) emit = nullptr;  // в вывод программатора сессия пишет сама
  return session::erase_main(emit, ectx);
}

const commands::Host HOST = {nullptr, get_chip, set_chip, t_active, t_start,  t_end,
                             t_info,  t_erase,  t_reset,  get_log, set_log};

gdb::Outcome t_command(void *, const char *line, gdb::LineFn emit, void *ectx) {
  return commands::run(HOST, line, emit, ectx, commands::Source::Gdb);
}
void t_print(void *, const char *line) { print(line); }

const gdb::Target TARGET = {nullptr, get_chip, t_active, t_start,   t_end,  t_read,
                            t_rreg,  t_wreg,   t_flash,  t_command, t_print};

gdb::Server server(TARGET, img, usb_write, nullptr);

void console_command(void *, const char *line, gdb::LineFn emit, void *ectx) {
  commands::run(HOST, line, emit, ectx, commands::Source::Text);
}
const console::Io IO = {nullptr, usb_write, console_command};
console::Console con(server, IO);

void print(const char *line) { con.line(line); }

void print_chip() {
  commands::run(HOST, "chip", console::Console::emit, &con, commands::Source::Text);
}

}  // namespace

void setup() {
  Serial.setRxBufferSize(8192);  // больше PacketSize с кадром (design §1)
  Serial.begin(115200);

  Serial1.setRxBufferSize(4096);
  log_uart_begin();

  prefs.begin("flasher", false);
  selected = chips::find(prefs.getString("chip", "").c_str());

  session::begin({print, log_pause, log_clear, log_resume});
  session::recover(selected);

  commands::help(console::Console::emit, &con);
  print_chip();
}

void loop() {
  uint32_t now = millis();
  con.dtr(dtr(), now);

  static uint8_t buf[512];
  int n = Serial.available();
  if (n > 0) {
    n = Serial.read(buf, n > (int)sizeof buf ? sizeof buf : n);
    con.usb(buf, n, millis());
  }
  con.tick(millis());

  n = Serial1.available();
  if (n > 0) {
    n = Serial1.read(buf, n > (int)sizeof buf ? sizeof buf : n);
    if (log_on) con.log(buf, n);  // во время сеанса SWD TX платы плавает — отбрасываем
  }
  if (n <= 0) delay(1);
}
