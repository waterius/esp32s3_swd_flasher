// План записи: что делать с каждой единицей образа — до первого стирания (design §7).
#pragma once
#include <stdint.h>

#include "core/chip.h"
#include "core/image.h"

namespace plan {

enum class Action : uint8_t { None, Write, Skip };

struct Plan {
  bool ok;
  const char *reason;  // при отказе
  uint32_t addr;       // адрес отказа
  Action action[image::MAX_UNITS];
  uint16_t write_count;
  uint16_t skip_count;
};

// Прочитать len байт чипа с addr; false — чтению верить нельзя.
using ReadFn = bool (*)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);

// Решить про каждую задетую единицу. Читает чип только для областей с read_before_write.
// Отказ любой единицы — отказ всего плана.
void make(Plan &out, const image::Image &img, const chip::Desc &desc, ReadFn read, void *ctx);

// Прочитанный идентификатор — выбранный чип.
bool id_matches(const chip::Desc &desc, uint32_t value);

}  // namespace plan
