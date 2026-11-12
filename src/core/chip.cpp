#include "core/chip.h"

namespace chip {

const Region *find_region(const Desc &desc, uint32_t addr, uint32_t len) {
  for (uint8_t i = 0; i < desc.region_count; i++) {
    const Region &r = desc.regions[i];
    if (r.kind != Kind::Flash || r.unit == 0) continue;
    // без переполнения: addr внутри области и длина не больше остатка
    if (addr >= r.start && addr - r.start < r.size && len <= r.size - (addr - r.start)) return &r;
  }
  return nullptr;
}

}  // namespace chip
