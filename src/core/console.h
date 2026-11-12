// Один USB-порт, два режима: текст (лог платы и команды человека) и gdb (design §9, §10).
// Решает, кто открыл порт, держит вывод в буфере 4 КБ, пока порт не свободен, и следит, чтобы
// строки оснастки начинались с новой строки. Пакеты отдаёт core/gdb, строки — командам.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core/gdb.h"
#include "core/rsp.h"

namespace console {

constexpr size_t OUT_BUF = 4096;
constexpr uint32_t MONITOR_DELAY_MS = 250;  // тишина после открытия порта — на том конце монитор
constexpr uint32_t GDB_IDLE_MS = 60000;     // столько без пакетов — сеанс брошен
constexpr size_t LINE_LEN = 128;

enum class Mode : uint8_t {
  Closed,   // порт на ПК закрыт (DTR опущен)
  Unknown,  // порт открыли, кто — ещё не ясно
  Text,     // человек или монитор: вывод идёт
  Gdb,      // пакеты gdb: вывод копится
};

struct Io {
  void *ctx;
  void (*write)(void *ctx, const uint8_t *data, size_t len);  // в USB
  void (*command)(void *ctx, const char *line, gdb::LineFn emit, void *emit_ctx);
};

class Console {
 public:
  Console(gdb::Server &server, const Io &io);

  void dtr(bool high, uint32_t now_ms);
  void usb(const uint8_t *data, size_t len, uint32_t now_ms);
  void tick(uint32_t now_ms);

  // Строка оснастки «# …» — с новой строки, с CR LF в конце.
  void line(const char *text);
  // Байты лога платы — как есть.
  void log(const uint8_t *data, size_t len);
  // gdb::LineFn для команд: строка в вывод.
  static void emit(void *self, const char *text);

  Mode mode() const { return mode_; }

 private:
  void byte_text(uint8_t b, uint32_t now);
  void byte_gdb(uint8_t b, uint32_t now);
  void enter_gdb(uint32_t now);
  void to_text();
  void exec_line();
  void append(uint8_t b);
  void put(const uint8_t *data, size_t len);
  void flush();

  gdb::Server &server_;
  Io io_;
  rsp::Parser parser_;
  Mode mode_ = Mode::Closed;
  uint32_t opened_ms_ = 0;
  uint32_t last_packet_ms_ = 0;
  bool idle_fired_ = false;
  bool candidate_ = false;  // в текстовом режиме пришёл «$» — может быть пакет gdb
  char line_[LINE_LEN];
  size_t line_len_ = 0;
  bool line_overflow_ = false;
  uint8_t out_[OUT_BUF];
  size_t out_len_ = 0;
  uint32_t dropped_ = 0;
  bool at_line_start_ = true;
};

}  // namespace console
