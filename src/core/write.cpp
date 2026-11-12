#include "core/write.h"

#include <stdio.h>
#include <string.h>

namespace writer {

namespace {

constexpr uint32_t MAX_UNIT = 4096;
uint8_t readback[MAX_UNIT];

RegionStats &stats_for(Report &r, const chip::Region *region) {
  for (uint8_t i = 0; i < r.region_count; i++)
    if (r.regions[i].region == region) return r.regions[i];
  RegionStats &s = r.regions[r.region_count++];
  s = {region, 0, 0, UINT32_MAX, 0, UINT32_MAX, 0, 0};
  return s;
}

void minmax(uint32_t v, uint32_t &lo, uint32_t &hi) {
  if (v < lo) lo = v;
  if (v > hi) hi = v;
}

bool stop(Report &r, Result result, uint32_t addr, const chip::Region *region, const char *detail) {
  r.result = result;
  r.addr = addr;
  r.region = region;
  r.detail = detail;
  return false;
}

// Номер первого байта не 0xFF; len — все стёрты.
uint32_t first_not_erased(const uint8_t *p, uint32_t len) {
  uint32_t i = 0;
  while (i < len && p[i] == 0xFF) i++;
  return i;
}

// Пишет единицы плана; false — остановились, причина в r.
bool write_units(Report &r, const image::Image &img, const plan::Plan &pl, const Ops &ops) {
  for (uint16_t i = 0; i < img.unit_count(); i++) {
    if (pl.action[i] == plan::Action::None) continue;
    image::Unit u = img.unit(i);
    RegionStats &s = stats_for(r, u.region);
    if (pl.action[i] == plan::Action::Skip) {
      s.skipped++;
      continue;
    }
    uint32_t t0 = ops.now_us(ops.ctx);
    uint32_t busy = 0;
    const char *err = nullptr;
    if (!ops.erase_unit(ops.ctx, u.addr, &busy, &err)) return stop(r, Result::EraseFailed, u.addr, u.region, err);
    minmax(busy, s.erase_min_us, s.erase_max_us);
    if (u.region->check_erase) {
      if (u.len > MAX_UNIT || !ops.read(ops.ctx, u.addr, readback, u.len))
        return stop(r, Result::ReadFailed, u.addr, u.region, nullptr);
      // недостёртая единица плюс новые байты могут дать замок — не пишем (спека swd-flash)
      if (first_not_erased(readback, u.len) < u.len) return stop(r, Result::NotErased, u.addr, u.region, nullptr);
    }
    if (!ops.write_unit(ops.ctx, u.addr, u.data, u.len, &busy, &err))
      return stop(r, Result::WriteFailed, u.addr, u.region, err);
    minmax(busy, s.write_min_us, s.write_max_us);
    ops.reload_watchdog(ops.ctx);
    s.written++;
    s.time_us += ops.now_us(ops.ctx) - t0;
  }
  return true;
}

// Сверяет все единицы плана, и записанные, и пропущенные.
bool verify(Report &r, const image::Image &img, const plan::Plan &pl, const Ops &ops) {
  for (uint16_t i = 0; i < img.unit_count(); i++) {
    if (pl.action[i] == plan::Action::None) continue;
    image::Unit u = img.unit(i);
    if (u.len > MAX_UNIT || !ops.read(ops.ctx, u.addr, readback, u.len))
      return stop(r, Result::ReadFailed, u.addr, u.region, nullptr);
    for (uint32_t k = 0; k < u.len; k++)
      if (readback[k] != u.data[k]) return stop(r, Result::Mismatch, u.addr + k, u.region, nullptr);
  }
  return true;
}

// Вся область — 0xFF; иначе стоп с адресом первого другого байта. Читает кусками буфера.
bool verify_erased(Report &r, const chip::Region &reg, const Ops &ops) {
  for (uint32_t off = 0; off < reg.size; off += MAX_UNIT) {
    uint32_t addr = reg.start + off;
    uint32_t len = reg.size - off < MAX_UNIT ? reg.size - off : MAX_UNIT;
    if (!ops.read(ops.ctx, addr, readback, len)) return stop(r, Result::ReadFailed, addr, &reg, nullptr);
    uint32_t k = first_not_erased(readback, len);
    if (k < len) return stop(r, Result::NotErased, addr + k, &reg, nullptr);
  }
  return true;
}

// 3500 мкс → «3,5»
void ms1(char *buf, size_t size, uint32_t us) {
  uint32_t tenths = (us + 50) / 100;
  snprintf(buf, size, "%lu,%lu", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

unsigned long ms0(uint32_t us) { return (us + 500) / 1000; }

}  // namespace

void erase_main(Report &out, const chip::Desc &desc, const Ops &ops) {
  memset(&out, 0, sizeof out);
  out.result = Result::Ok;
  const chip::Region *main = chip::main_region(desc);
  if (!main) {
    stop(out, Result::NoMainRegion, 0, nullptr, nullptr);
    return;
  }
  const char *err = nullptr;
  if (!ops.unlock(ops.ctx, &err)) {
    stop(out, Result::UnlockFailed, 0, nullptr, err);
    return;
  }
  uint32_t busy = 0;
  bool ok = ops.erase_main(ops.ctx, main->start, &busy, &err);
  ops.lock(ops.ctx);  // при любом исходе
  if (!ok) {
    stop(out, Result::MainEraseFailed, main->start, main, err);
    return;
  }
  out.main_erase_us = busy;
  out.out_of_datasheet = busy > desc.main_erase_max_us;
  ops.reload_watchdog(ops.ctx);
  uint32_t t0 = ops.now_us(ops.ctx);
  verify_erased(out, *main, ops);
  out.verify_us = ops.now_us(ops.ctx) - t0;
}

void run(Report &out, const image::Image &img, const plan::Plan &pl, const chip::Desc &desc,
         const Ops &ops) {
  memset(&out, 0, sizeof out);
  out.result = Result::Ok;
  const char *err = nullptr;
  if (!ops.unlock(ops.ctx, &err)) {
    stop(out, Result::UnlockFailed, 0, nullptr, err);
    return;
  }
  bool ok = write_units(out, img, pl, ops);
  ops.lock(ops.ctx);  // при любом исходе
  for (uint8_t i = 0; i < out.region_count; i++) {
    const RegionStats &s = out.regions[i];
    if (s.written && (s.erase_max_us > desc.erase_max_us || s.write_max_us > desc.write_max_us))
      out.out_of_datasheet = true;
  }
  if (!ok) return;
  uint32_t t0 = ops.now_us(ops.ctx);
  verify(out, img, pl, ops);
  out.verify_us = ops.now_us(ops.ctx) - t0;
}

void failure_text(const Report &r, char *buf, size_t size) {
  switch (r.result) {
    case Result::Ok:
      snprintf(buf, size, "%s", "");
      break;
    case Result::UnlockFailed:
      snprintf(buf, size, "ошибка записи: разблокировка, %s", r.detail ? r.detail : "");
      break;
    case Result::EraseFailed:
    case Result::WriteFailed:
      snprintf(buf, size, "ошибка записи: 0x%08lX, %s", (unsigned long)r.addr, r.detail ? r.detail : "");
      break;
    case Result::NotErased:
      snprintf(buf, size, "%s не стёрлась: 0x%08lX", r.region ? r.region->title : "",
               (unsigned long)r.addr);
      break;
    case Result::ReadFailed:
      snprintf(buf, size, "чтение не удалось: 0x%08lX", (unsigned long)r.addr);
      break;
    case Result::Mismatch:
      snprintf(buf, size, "сверка: расхождение по 0x%08lX", (unsigned long)r.addr);
      break;
    case Result::MainEraseFailed:
      // WRPERR при полном стирании — защита записи или Load flash (спека swd-flash)
      snprintf(buf, size, "полное стирание невозможно: %s",
               r.detail && strcmp(r.detail, "WRPERR") == 0 ? "защита записи" : (r.detail ? r.detail : ""));
      break;
    case Result::NoMainRegion:
      snprintf(buf, size, "%s", "полное стирание: у чипа нет основной flash");
      break;
  }
}

void describe(const Report &r, LineFn emit, void *ctx) {
  char line[160], a[16], b[16], c[16], d[16];
  for (uint8_t i = 0; i < r.region_count; i++) {
    const RegionStats &s = r.regions[i];
    const chip::Region &reg = *s.region;
    if (reg.size == reg.unit) {  // область из одной единицы — USER OTP
      if (s.written)
        snprintf(line, sizeof line, "# %s: записано, %lu мс", reg.name, ms0(s.time_us));
      else if (s.skipped)
        snprintf(line, sizeof line, "# %s: совпадает", reg.name);
      else
        continue;
    } else if (s.written) {
      ms1(a, sizeof a, s.erase_min_us);
      ms1(b, sizeof b, s.erase_max_us);
      ms1(c, sizeof c, s.write_min_us);
      ms1(d, sizeof d, s.write_max_us);
      int n = snprintf(line, sizeof line, "# %s: %u стр., %lu мс, стирание %s–%s мс, запись %s–%s мс",
                       reg.name, s.written, ms0(s.time_us), a, b, c, d);
      if (s.skipped && n > 0 && (size_t)n < sizeof line)
        snprintf(line + n, sizeof line - n, ", совпало %u стр.", s.skipped);
    } else if (s.skipped) {
      snprintf(line, sizeof line, "# %s: совпадает, %u стр.", reg.name, s.skipped);
    } else {
      continue;
    }
    emit(ctx, line);
  }
  if (r.out_of_datasheet) emit(ctx, "# время записи вне даташита");
  if (r.result == Result::Ok) {
    snprintf(line, sizeof line, "# сверка: %lu мс", ms0(r.verify_us));
  } else {
    char reason[120];
    failure_text(r, reason, sizeof reason);
    snprintf(line, sizeof line, "# %s", reason);
  }
  emit(ctx, line);
}

void describe_erase(const Report &r, LineFn emit, void *ctx) {
  char line[160], a[16];
  if (r.main_erase_us) {
    ms1(a, sizeof a, r.main_erase_us);
    snprintf(line, sizeof line, "# полное стирание: %s мс", a);
    emit(ctx, line);
  }
  if (r.out_of_datasheet) emit(ctx, "# время стирания вне даташита");
  if (r.result == Result::Ok) {
    snprintf(line, sizeof line, "# проверка 0xFF: %lu мс", ms0(r.verify_us));
  } else {
    char reason[120];
    failure_text(r, reason, sizeof reason);
    snprintf(line, sizeof line, "# %s", reason);
  }
  emit(ctx, line);
}

}  // namespace writer
