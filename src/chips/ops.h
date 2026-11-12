// Действия с чипом через SWD (design §6, «Действия с чипом»). Описание чипа — core/chip.h.
#pragma once
#include <stdint.h>

#include "core/chip.h"
#include "core/gdb.h"

namespace chips {

struct Ops {
  // После остановки ядра: идентификатор, отладка сторожа, частота и тайминги, UID.
  // nullptr — готово, иначе причина отказа для человека.
  const char *(*prepare)(const chip::Desc &desc);
  // Перед завершением сеанса: вернуть регистры, которые меняли для отладки и записи.
  void (*restore)();
  bool (*unlock)(const char **err);
  void (*lock)();
  bool (*erase_unit)(uint32_t addr, uint32_t *busy_us, const char **err);
  bool (*write_unit)(uint32_t addr, const uint8_t *data, uint32_t len, uint32_t *busy_us, const char **err);
  void (*reload_watchdog)();
  // Строки для `info`: идентификатор, UID, частота, тайминги.
  void (*info)(gdb::LineFn emit, void *emit_ctx);
};

extern const Ops py32f002b_ops;

const Ops *ops_for(const chip::Desc &desc);

}  // namespace chips
