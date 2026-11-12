// Команды программатора: help, chip, info, erase, reset, baud и, через monitor, swdp_scan (design §11).
// chip перечисляет области чипа, baud задаёт и формат кадра лога (jaffa-flasher-page design §4, §7).
// Один обработчик для строки в текстовом режиме и для monitor в gdb — одинаковые ответы.
#pragma once
#include <stdint.h>

#include "core/chip.h"
#include "core/gdb.h"

namespace commands {

constexpr uint32_t BAUD_MIN = 1200;
constexpr uint32_t BAUD_MAX = 2000000;

// UART лога платы: скорость и формат кадра (jaffa-flasher-page design §7).
struct LogFormat {
  uint32_t baud;
  uint8_t data_bits;  // 5–8
  char parity;        // 'N', 'E', 'O'
  uint8_t stop_bits;  // 1, 2
};

// «8E1» → биты, чётность, стоп; false — формат неверный, out не тронут.
bool parse_format(const char *text, LogFormat *out);

// Что команды делают на плате: выбор чипа (с NVS), сеанс SWD, сброс, UART лога.
struct Host {
  void *ctx;
  const chip::Desc *(*chip)(void *ctx);
  void (*set_chip)(void *ctx, const chip::Desc *desc);
  bool (*session_active)(void *ctx);
  bool (*start_session)(void *ctx, gdb::LineFn emit, void *emit_ctx);
  void (*end_session)(void *ctx);
  // Сведения о чипе в идущем сеансе: способ подключения, IDCODE, идентификатор, UID, HSI.
  void (*info)(void *ctx, gdb::LineFn emit, void *emit_ctx);
  // Полное стирание основной flash в идущем сеансе; строки и итог выводит само.
  bool (*erase)(void *ctx, gdb::LineFn emit, void *emit_ctx);
  void (*reset_board)(void *ctx);
  LogFormat (*log)(void *ctx);
  void (*set_log)(void *ctx, const LogFormat &format);
};

// Откуда пришла команда: строкой в мониторе порта или monitor в gdb. swdp_scan — только из gdb:
// сеанс, начатый строкой, ничто бы не закрыло.
enum class Source : uint8_t { Text, Gdb };

// Возвращает, что команда сделала с чипом (erase — WriteOk или WriteFailed).
gdb::Outcome run(const Host &host, const char *line, gdb::LineFn emit, void *emit_ctx, Source source);

// Подсказка при старте и на неизвестную команду.
void help(gdb::LineFn emit, void *emit_ctx);

}  // namespace commands
