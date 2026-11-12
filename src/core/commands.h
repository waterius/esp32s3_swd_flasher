// Команды оснастки: help, chip, info, reset, baud и, через monitor, swdp_scan (design §11).
// Один обработчик для строки в текстовом режиме и для monitor в gdb — одинаковые ответы.
#pragma once
#include <stdint.h>

#include "core/chip.h"
#include "core/gdb.h"

namespace commands {

constexpr uint32_t BAUD_MIN = 1200;
constexpr uint32_t BAUD_MAX = 2000000;

// Что команды делают на плате: выбор чипа (с NVS), сеанс SWD, сброс, скорость лога.
struct Host {
  void *ctx;
  const chip::Desc *(*chip)(void *ctx);
  void (*set_chip)(void *ctx, const chip::Desc *desc);
  bool (*session_active)(void *ctx);
  bool (*start_session)(void *ctx, gdb::LineFn emit, void *emit_ctx);
  void (*end_session)(void *ctx);
  // Сведения о чипе в идущем сеансе: способ подключения, IDCODE, идентификатор, UID, HSI.
  void (*info)(void *ctx, gdb::LineFn emit, void *emit_ctx);
  void (*reset_board)(void *ctx);
  uint32_t (*baud)(void *ctx);
  void (*set_baud)(void *ctx, uint32_t baud);
};

// Откуда пришла команда: строкой в мониторе порта или monitor в gdb. swdp_scan — только из gdb:
// сеанс, начатый строкой, ничто бы не закрыло.
enum class Source : uint8_t { Text, Gdb };

void run(const Host &host, const char *line, gdb::LineFn emit, void *emit_ctx, Source source);

// Подсказка при старте и на неизвестную команду.
void help(gdb::LineFn emit, void *emit_ctx);

}  // namespace commands
