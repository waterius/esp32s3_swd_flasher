// Образ: что ПК прислал для записи за один раз — стирания диапазонов и байты (design §7).
// Единица, которую задели, пишется целиком: байты вне образа — 0xFF, как у gdb.
#pragma once
#include <stdint.h>

#include "core/chip.h"

namespace image {

// Сумма областей записи и число единиц у самого большого поддержанного чипа.
constexpr uint32_t MAX_BYTES = 32 * 1024;
constexpr uint16_t MAX_UNITS = 256;
constexpr uint8_t MAX_REGIONS = 4;

struct Unit {
  const chip::Region *region;
  uint32_t addr;
  uint32_t len;
  const uint8_t *data;  // итоговые байты единицы
  bool touched;         // в единицу попал байт или стирание — её стирают и пишут
};

class Image {
 public:
  // Пустой образ под области записи чипа.
  void reset(const chip::Desc &desc);

  // false — диапазон вне областей записи: образ отброшен, адрес в fail_addr(). После этого
  // образ ничего не принимает до reset().
  bool erase(uint32_t addr, uint32_t len);
  bool write(uint32_t addr, const uint8_t *data, uint32_t len);

  bool failed() const { return failed_; }
  uint32_t fail_addr() const { return fail_addr_; }
  bool empty() const;

  uint16_t unit_count() const { return unit_count_; }
  Unit unit(uint16_t index) const;

 private:
  struct Slot {
    const chip::Region *region;
    uint32_t offset;      // начало области в data_
    uint16_t first_unit;  // номер первой единицы области
  };
  // Единица и смещение в data_ для адреса внутри области записи; false — вне областей.
  bool locate(uint32_t addr, uint32_t len, const Slot **slot) const;
  bool fail(uint32_t addr);
  void touch(const Slot &slot, uint32_t addr, uint32_t len);

  Slot slots_[MAX_REGIONS];
  uint8_t slot_count_ = 0;
  uint16_t unit_count_ = 0;
  bool failed_ = false;
  uint32_t fail_addr_ = 0;
  uint8_t touched_[MAX_UNITS / 8];
  uint8_t data_[MAX_BYTES];
};

}  // namespace image
