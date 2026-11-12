// Сеанс SWD: подключение через NRST двумя способами, подготовка чипа, запись образа,
// завершение при любом исходе (design §4, §5, §8). Функции цели для core/gdb и хозяина для
// core/commands собирает main.
#pragma once
#include <stdint.h>

#include "core/chip.h"
#include "core/gdb.h"
#include "core/image.h"

namespace session {

struct Hooks {
  void (*print)(const char *line);  // строка программатора «# …» в вывод
  void (*log_pause)();              // пересылку лога остановить, принятое отбрасывать
  void (*log_clear)();              // очистить приёмник лога — до отпускания NRST в конце
  void (*log_resume)();             // пересылку лога включить
};

void begin(const Hooks &hooks);

// Если программатор перезапустился посреди сеанса (отметка в RTC-памяти) — подключиться и
// завершить его, чтобы плата не осталась остановленной.
void recover(const chip::Desc *desc);

bool active();
bool start(const chip::Desc &desc, gdb::LineFn emit, void *emit_ctx);
void end();
void info(gdb::LineFn emit, void *emit_ctx);

bool read_mem(uint32_t addr, uint8_t *buf, uint32_t len);
bool read_reg(uint8_t index, uint32_t *value);
bool write_reg(uint8_t index, uint32_t value);
bool flash(const image::Image &img);
// Полное стирание основной flash в идущем сеансе (команда erase): строки шагов и итог —
// в вывод программатора и, если emit не nullptr, ещё и туда.
bool erase_main(gdb::LineFn emit, void *emit_ctx);

// Импульс NRST 10 мс без SWD (команда reset).
void reset_board();

}  // namespace session
