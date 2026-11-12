#include "core/plan.h"

#include <string.h>

namespace plan {

namespace {

// Самая большая единица среди поддержанных чипов.
constexpr uint32_t MAX_UNIT = 4096;
uint8_t current[MAX_UNIT];

void refuse(Plan &out, const char *reason, uint32_t addr) {
  out.ok = false;
  out.reason = reason;
  out.addr = addr;
  out.write_count = out.skip_count = 0;
}

}  // namespace

void make(Plan &out, const image::Image &img, const chip::Desc &desc, ReadFn read, void *ctx) {
  out.ok = true;
  out.reason = nullptr;
  out.addr = 0;
  out.write_count = out.skip_count = 0;
  memset(out.action, 0, sizeof out.action);

  if (img.failed()) return refuse(out, "адрес вне памяти чипа", img.fail_addr());
  if (img.empty()) return refuse(out, "образа нет", 0);

  for (uint16_t i = 0; i < img.unit_count(); i++) {
    image::Unit u = img.unit(i);
    if (!u.touched) continue;
    const uint8_t *chip_unit = nullptr;
    if (u.region->read_before_write) {
      if (u.len > MAX_UNIT || !read(ctx, u.addr, current, u.len))
        return refuse(out, "чтение не удалось", u.addr);
      chip_unit = current;
    }
    chip::Verdict v = desc.decide(*u.region, u.addr, u.data, chip_unit, u.len);
    switch (v.decision) {
      case chip::Decision::Refuse:
        return refuse(out, v.reason, u.addr);
      case chip::Decision::Skip:
        out.action[i] = Action::Skip;
        out.skip_count++;
        break;
      case chip::Decision::Write:
        out.action[i] = Action::Write;
        out.write_count++;
        break;
    }
  }
}

bool id_matches(const chip::Desc &desc, uint32_t value) { return value == desc.id_value; }

}  // namespace plan
