// Порядок записи по плану на поддельном чипе: стоп на первой неудаче, проверка стирания OTP,
// сверка всех единиц, время операций (design §7, §8).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "chips/chips.h"
#include "core/image.h"
#include "core/plan.h"
#include "core/write.h"

static image::Image img;
static plan::Plan pl;
static write::Report rep;

// Поддельный чип: память, контроллер flash с замком и подстроенные сбои.
struct FakeChip {
  uint8_t flash[24 * 1024];
  uint8_t otp[128];
  bool unlocked;
  int unlocks, locks, reloads;
  uint32_t wrperr_from;     // стирание с этого адреса и выше — WRPERR (0 — нет)
  bool otp_erase_leaves;    // стирание OTP оставляет слово 0 = 0xAAFF
  uint32_t corrupt_addr;    // запись портит этот байт (0 — нет)
  uint32_t erase_busy_us, write_busy_us;
  uint32_t clock_us;
  int erases_after_wrperr;  // сколько стираний пришло после отказа
  bool wrperr_seen;
};
static FakeChip fc;

static uint8_t *mem(uint32_t addr) {
  if (addr >= 0x1FFF0280 && addr < 0x1FFF0300) return fc.otp + (addr - 0x1FFF0280);
  return fc.flash + (addr - 0x08000000);
}

static bool f_unlock(void *, const char **err) {
  fc.unlocks++;
  if (fc.unlocked) {  // повторные ключи — неверная последовательность: FLASH_CR заперт
    *err = "повторная разблокировка";
    return false;
  }
  fc.unlocked = true;
  return true;
}
static void f_lock(void *) {
  fc.locks++;
  fc.unlocked = false;
}
static bool f_erase(void *, uint32_t addr, uint32_t *busy, const char **err) {
  if (fc.wrperr_seen) fc.erases_after_wrperr++;
  if (fc.wrperr_from && addr >= fc.wrperr_from && addr < 0x1FFF0000) {
    fc.wrperr_seen = true;
    *err = "WRPERR";
    return false;
  }
  memset(mem(addr), 0xFF, 128);
  if (addr == 0x1FFF0280 && fc.otp_erase_leaves) {
    fc.otp[0] = 0xFF;
    fc.otp[1] = 0xAA;
  }
  *busy = fc.erase_busy_us;
  return true;
}
static bool f_write(void *, uint32_t addr, const uint8_t *data, uint32_t len, uint32_t *busy,
                    const char **) {
  uint8_t *m = mem(addr);
  for (uint32_t i = 0; i < len; i++) m[i] &= data[i];  // запись только сбрасывает биты
  if (fc.corrupt_addr >= addr && fc.corrupt_addr < addr + len) *mem(fc.corrupt_addr) ^= 0x01;
  *busy = fc.write_busy_us;
  return true;
}
static void f_reload(void *) { fc.reloads++; }
static bool f_read(void *, uint32_t addr, uint8_t *buf, uint32_t len) {
  memcpy(buf, mem(addr), len);
  return true;
}
static uint32_t f_now(void *) { return fc.clock_us += 100; }

static const write::Ops OPS = {nullptr, f_unlock, f_lock, f_erase, f_write, f_reload, f_read, f_now};

static bool plan_read(void *, uint32_t addr, uint8_t *buf, uint32_t len) { return f_read(nullptr, addr, buf, len); }

void setUp(void) {
  img.reset(chips::py32f002b);
  memset(&fc, 0, sizeof fc);
  memset(fc.flash, 0x00, sizeof fc.flash);  // старая прошивка: всё в нулях
  memset(fc.otp, 0xFF, sizeof fc.otp);
  fc.erase_busy_us = 3500;
  fc.write_busy_us = 1000;
}
void tearDown(void) {}

static void run(void) {
  plan::make(pl, img, chips::py32f002b, plan_read, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(pl.ok, pl.reason);
  write::run(rep, img, pl, chips::py32f002b, OPS);
}

// Строки, которые выдал describe, — одной строкой через «|».
static char lines[1024];
static void collect(void *, const char *line) {
  strncat(lines, line, sizeof lines - strlen(lines) - 2);
  strcat(lines, "|");
}
static const char *describe(void) {
  lines[0] = 0;
  write::describe(rep, collect, nullptr);
  return lines;
}

static void put(uint32_t addr, uint8_t value, uint32_t len) {
  static uint8_t buf[2048];
  TEST_ASSERT_TRUE(len <= sizeof buf);
  memset(buf, value, len);
  TEST_ASSERT_TRUE(img.write(addr, buf, len));
}

static void test_written_pages_hold_image(void) {
  put(0x08000000, 0x42, 256);
  run();
  TEST_ASSERT_EQUAL(write::Result::Ok, rep.result);
  TEST_ASSERT_EQUAL_HEX8(0x42, fc.flash[0]);
  TEST_ASSERT_EQUAL_HEX8(0x42, fc.flash[255]);
  TEST_ASSERT_EQUAL_HEX8(0x00, fc.flash[256]);  // соседняя страница не тронута
}

// spec: "swd-flash/Пишутся только единицы стирания образа/Образ кончается посреди страницы"
static void test_page_tail_becomes_erased(void) {
  put(0x08000080, 0x11, 40);
  run();
  TEST_ASSERT_EQUAL_HEX8(0x11, fc.flash[0x80 + 39]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, fc.flash[0x80 + 40]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, fc.flash[0x80 + 127]);
}

// spec: "swd-flash/Пишутся только единицы стирания образа/Данные вне образа переживают прошивку"
static void test_data_outside_image_survives(void) {
  memset(fc.flash + 0x3200, 0x5C, 128);  // страница 100
  put(0x08000000, 0x01, 128 * 10);       // страницы 0–9 — по 256 Б за раз
  run();
  TEST_ASSERT_EQUAL(write::Result::Ok, rep.result);
  for (int i = 0; i < 128; i++) TEST_ASSERT_EQUAL_HEX8(0x5C, fc.flash[0x3200 + i]);
}

// spec: "swd-flash/Ошибка записи останавливает прошивку/Сектор под защитой записи"
static void test_write_protection_stops_at_first_page(void) {
  fc.wrperr_from = 0x08001000;
  put(0x08000F80, 0x22, 128);  // перед защищённым сектором
  put(0x08001000, 0x22, 128);  // в нём
  put(0x08001080, 0x22, 128);  // после
  run();
  TEST_ASSERT_EQUAL(write::Result::EraseFailed, rep.result);
  TEST_ASSERT_EQUAL_HEX32(0x08001000, rep.addr);
  TEST_ASSERT_EQUAL(0, fc.erases_after_wrperr);
  TEST_ASSERT_EQUAL_HEX8(0x22, fc.flash[0xF80]);   // до него — записано
  TEST_ASSERT_EQUAL_HEX8(0x00, fc.flash[0x1080]);  // после — не тронуто
  TEST_ASSERT_NOT_NULL(strstr(describe(), "# ошибка записи: 0x08001000, WRPERR|"));
  char text[96];
  write::failure_text(rep, text, sizeof text);
  TEST_ASSERT_EQUAL_STRING("ошибка записи: 0x08001000, WRPERR", text);
}

// spec: "swd-flash/Стирание OTP проверяется до записи/OTP не стёрлась"
static void test_otp_not_erased_is_not_written(void) {
  memset(fc.otp, 0x00, sizeof fc.otp);
  fc.otp[0] = 0xFF;
  fc.otp[1] = 0xAA;  // слово 0 = 0xAAFF: запись 0xFF55 поверх дала бы замок 0xAA55
  fc.otp_erase_leaves = true;
  uint8_t otp[128];
  memset(otp, 0xFF, sizeof otp);
  otp[0] = 0x55;
  otp[1] = 0xFF;
  TEST_ASSERT_TRUE(img.write(0x1FFF0280, otp, 128));
  run();
  TEST_ASSERT_EQUAL(write::Result::NotErased, rep.result);
  TEST_ASSERT_EQUAL_HEX8(0xFF, fc.otp[0]);  // запись не начиналась
  TEST_ASSERT_EQUAL_HEX8(0xAA, fc.otp[1]);
  TEST_ASSERT_NOT_NULL(strstr(describe(), "# OTP не стёрлась: 0x1FFF0280|"));
}

// spec: "swd-flash/Сверка после записи/Запись не удалась"
static void test_mismatch_after_write_is_reported(void) {
  fc.corrupt_addr = 0x08000010;
  put(0x08000000, 0x80, 128);
  run();
  TEST_ASSERT_EQUAL(write::Result::Mismatch, rep.result);
  TEST_ASSERT_EQUAL_HEX32(0x08000010, rep.addr);
  TEST_ASSERT_NOT_NULL(strstr(describe(), "# сверка: расхождение по 0x08000010|"));
}

static void test_skipped_unit_is_verified_too(void) {
  uint8_t otp[128];
  memset(otp, 0x33, sizeof otp);
  TEST_ASSERT_TRUE(img.write(0x1FFF0280, otp, 128));
  plan::make(pl, img, chips::py32f002b, plan_read, nullptr);
  // план ошибочно решил «совпадает» — чтение подвело, — сверка должна поймать
  for (uint16_t i = 0; i < img.unit_count(); i++)
    if (pl.action[i] == plan::Action::Write) pl.action[i] = plan::Action::Skip;
  write::run(rep, img, pl, chips::py32f002b, OPS);
  TEST_ASSERT_EQUAL(write::Result::Mismatch, rep.result);
  TEST_ASSERT_EQUAL_HEX32(0x1FFF0280, rep.addr);
}

static void test_lock_after_run_and_second_run_unlocks_again(void) {
  put(0x08000000, 0x01, 128);
  run();
  TEST_ASSERT_EQUAL(write::Result::Ok, rep.result);
  TEST_ASSERT_FALSE(fc.unlocked);
  img.reset(chips::py32f002b);
  TEST_ASSERT_TRUE(img.write(0x1FFF0280, fc.flash, 128));  // второй load в том же сеансе
  run();
  TEST_ASSERT_EQUAL(write::Result::Ok, rep.result);
  TEST_ASSERT_EQUAL(2, fc.unlocks);
  TEST_ASSERT_EQUAL(2, fc.locks);
}

static void test_lock_also_after_failure(void) {
  fc.wrperr_from = 0x08000000;
  put(0x08000000, 0x01, 128);
  run();
  TEST_ASSERT_EQUAL(write::Result::EraseFailed, rep.result);
  TEST_ASSERT_EQUAL(1, fc.locks);
}

static void test_watchdog_reloaded_after_each_page(void) {
  put(0x08000000, 0x01, 256);
  put(0x08000100, 0x01, 128);
  run();
  TEST_ASSERT_EQUAL(3, fc.reloads);
}

static void test_lines_show_counts_and_times(void) {
  put(0x08000000, 0x01, 256);
  uint8_t otp[128];
  memset(otp, 0x07, sizeof otp);
  memcpy(fc.otp, otp, 128);
  TEST_ASSERT_TRUE(img.write(0x1FFF0280, otp, 128));
  run();
  const char *out = describe();
  TEST_ASSERT_NOT_NULL(strstr(out, "# flash: 2 стр., "));
  TEST_ASSERT_NOT_NULL(strstr(out, ", стирание 3,5–3,5 мс, запись 1,0–1,0 мс|"));
  TEST_ASSERT_NOT_NULL(strstr(out, "# otp: совпадает|"));
  TEST_ASSERT_NULL(strstr(out, "вне даташита"));
}

static void test_slow_erase_warns(void) {
  fc.erase_busy_us = 6200;  // больше наибольших 5,0 мс по DS
  put(0x08000000, 0x01, 128);
  run();
  TEST_ASSERT_EQUAL(write::Result::Ok, rep.result);
  TEST_ASSERT_TRUE(rep.out_of_datasheet);
  TEST_ASSERT_NOT_NULL(strstr(describe(), "# время записи вне даташита|"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_written_pages_hold_image);
  RUN_TEST(test_page_tail_becomes_erased);
  RUN_TEST(test_data_outside_image_survives);
  RUN_TEST(test_write_protection_stops_at_first_page);
  RUN_TEST(test_otp_not_erased_is_not_written);
  RUN_TEST(test_mismatch_after_write_is_reported);
  RUN_TEST(test_skipped_unit_is_verified_too);
  RUN_TEST(test_lock_after_run_and_second_run_unlocks_again);
  RUN_TEST(test_lock_also_after_failure);
  RUN_TEST(test_watchdog_reloaded_after_each_page);
  RUN_TEST(test_lines_show_counts_and_times);
  RUN_TEST(test_slow_erase_warns);
  return UNITY_END();
}
