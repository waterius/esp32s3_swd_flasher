// Данные и чистые правила PY32F002B (design §6). Источники — datasheets/README.md.
#include <string.h>

#include "chips/chips.h"

namespace {

using chip::Decision;
using chip::Kind;
using chip::Region;
using chip::Verdict;

const Region REGIONS[] = {
    // имя, для сообщений, начало, размер, единица, читать до записи, проверять стирание, вид
    {"flash", "flash", 0x08000000, 24 * 1024, 128, false, false, Kind::Flash},  // RM §4.2.1
    {"otp", "OTP", 0x1FFF0280, 128, 128, true, true, Kind::Flash},  // USER OTP, RM §4.5.8
    {"ram", "ОЗУ", 0x20000000, 3 * 1024, 0, false, false, Kind::Ram},
};

// Замок USER OTP — младшие 16 бит слова 0 равны 0xAA55 (RM §3.4, табл. 4-6).
bool otp_locked(const uint8_t *unit) { return unit[0] == 0x55 && unit[1] == 0xAA; }

Verdict decide(const Region &region, uint32_t, const uint8_t *target, const uint8_t *current,
               uint32_t len) {
  if (strcmp(region.name, "otp") != 0) return {Decision::Write, nullptr};
  // порядок — спека swd-flash, «USER OTP PY32F002B не запирается»
  if (current && memcmp(target, current, len) == 0) return {Decision::Skip, nullptr};
  if (current && otp_locked(current)) return {Decision::Refuse, "OTP заперта"};
  if (otp_locked(target)) return {Decision::Refuse, "замок OTP не ставится"};
  return {Decision::Write, nullptr};
}

// RCC_ICSCR.HSI_FS — биты 15:13, 100 — 24 МГц (RM §8.6.2). Слова заводской области для
// 24 МГц — RM §4.5.2–4.5.6; регистры — в порядке адресов FLASH_TS0 … FLASH_PRETPE.
chip::Timing timing(uint32_t icscr, const uint32_t info[5]) {
  chip::Timing t{};
  if (((icscr >> 13) & 0x7) != 0x4) {
    t.ok = false;
    t.reason = "HSI не 24 МГц: снимите питание платы";
    return t;
  }
  t.ok = true;
  t.regs[0] = info[0] & 0x1FF;            // TS0
  t.regs[1] = (info[0] >> 18) & 0x3FF;    // TS1
  t.regs[2] = info[1] & 0x1FF;            // TS2P
  t.regs[3] = (info[1] >> 16) & 0xFFF;    // TPS3
  t.regs[4] = (info[0] >> 9) & 0x1FF;     // TS3
  t.regs[5] = info[2] & 0x3FFFF;          // PERTPE
  t.regs[6] = info[3] & 0x3FFFF;          // SMERTPE
  t.regs[7] = info[4] & 0xFFFF;           // PRGTPE
  t.regs[8] = (info[4] >> 16) & 0x3FFF;   // PRETPE
  return t;
}

}  // namespace

namespace chips {

const chip::Desc py32f002b = {
    "py32f002b",
    REGIONS,
    sizeof REGIONS / sizeof REGIONS[0],
    0x40015800,  // DBG_IDCODE, RM §22.9.1
    0x20200061,
    0x1FFF0000,  // UID, RM §4.3
    16,
    decide,
    timing,
    3500,  // стирание страницы, DS табл. 5-16
    5000,
    1000,  // запись страницы, DS табл. 5-15
    1500,
};

}  // namespace chips
