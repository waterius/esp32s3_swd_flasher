// Порядок записи по плану через функции чипа: разблокировать, стереть, проверить стирание,
// записать, перезарядить сторож, запереть, сверить (design §7, §8). Регистров чипа не знает.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core/chip.h"
#include "core/image.h"
#include "core/plan.h"

namespace writer {

// Действия с чипом. busy_us — сколько чип был занят операцией (BSY), без передачи по SWD.
// err — короткая причина от чипа: «WRPERR», «нет EOP», «BSY дольше 50 мс».
struct Ops {
  void *ctx;
  bool (*unlock)(void *ctx, const char **err);
  void (*lock)(void *ctx);
  bool (*erase_unit)(void *ctx, uint32_t addr, uint32_t *busy_us, const char **err);
  bool (*write_unit)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len,
                     uint32_t *busy_us, const char **err);
  void (*reload_watchdog)(void *ctx);
  bool (*read)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
  uint32_t (*now_us)(void *ctx);
};

enum class Result : uint8_t { Ok, UnlockFailed, EraseFailed, NotErased, WriteFailed, ReadFailed, Mismatch };

struct RegionStats {
  const chip::Region *region;
  uint16_t written;
  uint16_t skipped;
  uint32_t erase_min_us, erase_max_us;
  uint32_t write_min_us, write_max_us;
  uint32_t time_us;
};

struct Report {
  Result result;
  const char *detail;          // причина от чипа
  uint32_t addr;               // где остановились
  const chip::Region *region;  // область, где остановились
  RegionStats regions[image::MAX_REGIONS];
  uint8_t region_count;
  uint32_t verify_us;
  bool out_of_datasheet;  // время стирания или записи больше наибольшего по даташиту
};

void run(Report &out, const image::Image &img, const plan::Plan &pl, const chip::Desc &desc,
         const Ops &ops);

// Строки «# …» для человека: шаги и время, предупреждение, сверка или причина.
using LineFn = void (*)(void *ctx, const char *line);
void describe(const Report &r, LineFn emit, void *ctx);

// Причина неудачи без «# »: «ошибка записи: 0x08001000, WRPERR». Для Ok — пустая строка.
void failure_text(const Report &r, char *buf, size_t size);

}  // namespace writer
