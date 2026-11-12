#include "core/image.h"

#include <string.h>

namespace image {

void Image::reset(const chip::Desc &desc) {
  slot_count_ = 0;
  unit_count_ = 0;
  failed_ = false;
  fail_addr_ = 0;
  uint32_t offset = 0;
  for (uint8_t i = 0; i < desc.region_count && slot_count_ < MAX_REGIONS; i++) {
    const chip::Region &r = desc.regions[i];
    if (r.kind != chip::Kind::Flash || r.unit == 0) continue;
    if (offset + r.size > MAX_BYTES || unit_count_ + r.size / r.unit > MAX_UNITS) break;
    slots_[slot_count_++] = {&r, offset, unit_count_};
    offset += r.size;
    unit_count_ += r.size / r.unit;
  }
  memset(touched_, 0, sizeof touched_);
  memset(data_, 0xFF, offset);
}

bool Image::locate(uint32_t addr, uint32_t len, const Slot **slot) const {
  for (uint8_t i = 0; i < slot_count_; i++) {
    const chip::Region &r = *slots_[i].region;
    if (addr >= r.start && addr - r.start < r.size && len <= r.size - (addr - r.start)) {
      *slot = &slots_[i];
      return true;
    }
  }
  return false;
}

bool Image::fail(uint32_t addr) {
  if (!failed_) {
    failed_ = true;
    fail_addr_ = addr;
  }
  return false;
}

void Image::touch(const Slot &slot, uint32_t addr, uint32_t len) {
  const chip::Region &r = *slot.region;
  uint32_t first = (addr - r.start) / r.unit;
  uint32_t last = (addr - r.start + (len ? len - 1 : 0)) / r.unit;
  for (uint32_t u = first; u <= last; u++) {
    uint16_t n = slot.first_unit + u;
    touched_[n / 8] |= 1u << (n % 8);
  }
}

bool Image::erase(uint32_t addr, uint32_t len) {
  if (failed_) return false;
  const Slot *slot;
  if (!locate(addr, len, &slot)) return fail(addr);
  if (len) touch(*slot, addr, len);
  return true;
}

bool Image::write(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (failed_) return false;
  const Slot *slot;
  if (!locate(addr, len, &slot)) return fail(addr);
  if (!len) return true;
  memcpy(data_ + slot->offset + (addr - slot->region->start), data, len);
  touch(*slot, addr, len);
  return true;
}

bool Image::empty() const {
  for (uint16_t i = 0; i < (unit_count_ + 7) / 8; i++)
    if (touched_[i]) return false;
  return true;
}

Unit Image::unit(uint16_t index) const {
  for (uint8_t i = slot_count_; i-- > 0;) {
    const Slot &s = slots_[i];
    if (index < s.first_unit) continue;
    const chip::Region &r = *s.region;
    uint32_t u = index - s.first_unit;
    return {&r, r.start + u * r.unit, r.unit, data_ + s.offset + u * r.unit,
            (touched_[index / 8] & (1u << (index % 8))) != 0};
  }
  return {nullptr, 0, 0, nullptr, false};
}

}  // namespace image
