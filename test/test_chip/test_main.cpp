// Описание py32f002b: правило USER OTP, тайминги flash, области.
#include <string.h>
#include <unity.h>

#include "chips/chips.h"
#include "core/chip.h"

void setUp(void) {}
void tearDown(void) {}

static const chip::Desc &D = chips::py32f002b;

static const chip::Region &region(const char *name) {
  for (uint8_t i = 0; i < D.region_count; i++)
    if (strcmp(D.regions[i].name, name) == 0) return D.regions[i];
  TEST_FAIL_MESSAGE("нет области");
  return D.regions[0];
}

static void fill(uint8_t *unit, uint8_t value) { memset(unit, value, 128); }

static void test_otp_identical_is_skipped(void) {
  uint8_t target[128], current[128];
  fill(target, 0x11);
  fill(current, 0x11);
  chip::Verdict v = D.decide(region("otp"), 0x1FFF0280, target, current, 128);
  TEST_ASSERT_EQUAL(chip::Decision::Skip, v.decision);
}

static void test_otp_lock_in_target_is_refused(void) {
  uint8_t target[128], current[128];
  fill(target, 0xFF);
  fill(current, 0xFF);
  target[0] = 0x55;  // замок — младшие 16 бит слова 0: байт 0x55 по 0x1FFF0280, 0xAA по 0x1FFF0281
  target[1] = 0xAA;
  chip::Verdict v = D.decide(region("otp"), 0x1FFF0280, target, current, 128);
  TEST_ASSERT_EQUAL(chip::Decision::Refuse, v.decision);
  TEST_ASSERT_EQUAL_STRING("замок OTP не ставится", v.reason);
}

static void test_otp_swapped_bytes_are_not_a_lock(void) {
  uint8_t target[128], current[128];
  fill(target, 0xFF);
  fill(current, 0xFF);
  target[0] = 0xAA;  // 0x55AA — не замок
  target[1] = 0x55;
  TEST_ASSERT_EQUAL(chip::Decision::Write, D.decide(region("otp"), 0x1FFF0280, target, current, 128).decision);
}

static void test_otp_locked_and_different_is_refused(void) {
  uint8_t target[128], current[128];
  fill(current, 0x00);
  current[0] = 0x55;
  current[1] = 0xAA;
  fill(target, 0x00);  // другие байты
  chip::Verdict v = D.decide(region("otp"), 0x1FFF0280, target, current, 128);
  TEST_ASSERT_EQUAL(chip::Decision::Refuse, v.decision);
  TEST_ASSERT_EQUAL_STRING("OTP заперта", v.reason);
}

static void test_otp_locked_and_identical_is_skipped(void) {
  uint8_t target[128], current[128];
  fill(current, 0x00);
  current[0] = 0x55;
  current[1] = 0xAA;
  memcpy(target, current, 128);
  TEST_ASSERT_EQUAL(chip::Decision::Skip, D.decide(region("otp"), 0x1FFF0280, target, current, 128).decision);
}

static void test_otp_different_and_unlocked_is_written(void) {
  uint8_t target[128], current[128];
  fill(current, 0xFF);
  fill(target, 0x00);
  target[0] = target[1] = target[2] = target[3] = 0xFF;
  TEST_ASSERT_EQUAL(chip::Decision::Write, D.decide(region("otp"), 0x1FFF0280, target, current, 128).decision);
}

static void test_main_flash_is_always_written(void) {
  uint8_t target[128];
  fill(target, 0x00);
  target[0] = 0x55;
  target[1] = 0xAA;  // в основной flash это просто данные
  TEST_ASSERT_EQUAL(chip::Decision::Write, D.decide(region("flash"), 0x08000000, target, nullptr, 128).decision);
}

// Слова информационной области, собранные вручную из значений регистров после сброса
// (RM §4.8.11–4.8.19) по раскладке RM §4.5.2–4.5.6.
static const uint32_t INFO_RESET[5] = {
    0x06C168B4,  // TS1 0x1B0 << 18 | TS3 0xB4 << 9 | TS0 0xB4
    0x06C000B4,  // TPS3 0x6C0 << 16 | TS2P 0xB4
    0x00014820,  // PERTPE
    0x00014820,  // SMERTPE
    0x12C05DC0,  // PRETPE 0x12C0 << 16 | PRGTPE 0x5DC0
};
// Заводское слово «HSI 24 МГц» по 0x1FFF0100 (RM §4.5.1): HSI_FS в битах 15:13 и подстройка.
static const uint32_t HSI24_REF = 0x000090A7;    // HSI_FS = 100
static const uint32_t ICSCR_24MHZ = 0x00FF90A7;  // в RCC_ICSCR то же поле частоты
static const uint32_t ICSCR_8MHZ = 0x00FF20FF;   // HSI_FS = 001 — 8 МГц

static void test_timing_words_map_to_registers(void) {
  chip::Timing t = D.timing(ICSCR_24MHZ, HSI24_REF, INFO_RESET);
  TEST_ASSERT_TRUE(t.ok);
  // TS0, TS1, TS2P, TPS3, TS3, PERTPE, SMERTPE, PRGTPE, PRETPE — порядок адресов 0x100…0x120
  const uint32_t want[9] = {0xB4, 0x1B0, 0xB4, 0x6C0, 0xB4, 0x14820, 0x14820, 0x5DC0, 0x12C0};
  TEST_ASSERT_EQUAL_HEX32_ARRAY(want, t.regs, 9);
}

static void test_timing_fields_do_not_leak(void) {
  // все биты слов в единицах: каждое поле берёт только свою ширину
  const uint32_t ones[5] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  chip::Timing t = D.timing(ICSCR_24MHZ, HSI24_REF, ones);
  const uint32_t want[9] = {0x1FF, 0x3FF, 0x1FF, 0xFFF, 0x1FF, 0x3FFFF, 0x3FFFF, 0xFFFF, 0x3FFF};
  TEST_ASSERT_EQUAL_HEX32_ARRAY(want, t.regs, 9);
}

// spec: "swd-flash/Тайминги flash PY32F002B/HSI не 24 МГц"
static void test_timing_refuses_hsi_other_than_24mhz(void) {
  chip::Timing t = D.timing(ICSCR_8MHZ, HSI24_REF, INFO_RESET);
  TEST_ASSERT_FALSE(t.ok);
  TEST_ASSERT_EQUAL_STRING("HSI не 24 МГц: снимите питание платы", t.reason);
}

// Поле частоты сравнивается с заводским словом, а не с кодом из таблицы RM: значение после
// сброса в RM (0x00FF10FF) и подпись «100 — 24 МГц» противоречат друг другу.
static void test_timing_follows_factory_hsi_code(void) {
  TEST_ASSERT_TRUE(D.timing(0x000010FF, 0x000010FF, INFO_RESET).ok);    // оба поля 000
  TEST_ASSERT_FALSE(D.timing(0x000010FF, HSI24_REF, INFO_RESET).ok);    // 000 против 100
  TEST_ASSERT_TRUE(D.timing(0x00FF9FFF, HSI24_REF, INFO_RESET).ok);     // подстройка не важна
}

static void test_find_region_bounds(void) {
  TEST_ASSERT_NOT_NULL(chip::find_region(D, 0x08000000, 0x6000));
  TEST_ASSERT_NULL(chip::find_region(D, 0x08005FFF, 2));  // выходит за конец flash
  TEST_ASSERT_NULL(chip::find_region(D, 0x1FFF0080, 1));  // option bytes — не область записи
  TEST_ASSERT_EQUAL_STRING("otp", chip::find_region(D, 0x1FFF02FF, 1)->name);
  TEST_ASSERT_NULL(chip::find_region(D, 0x20000000, 4));  // ОЗУ — не для записи
}

static void test_chip_is_found_by_name(void) {
  TEST_ASSERT_EQUAL_PTR(&chips::py32f002b, chips::find("py32f002b"));
  TEST_ASSERT_NULL(chips::find("stm32f103"));
}

static void test_main_region_is_flash(void) {
  const chip::Region *m = chip::main_region(chips::py32f002b);
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_STRING("flash", m->name);
  TEST_ASSERT_EQUAL_HEX32(0x08000000, m->start);
  TEST_ASSERT_EQUAL_UINT32(24 * 1024, m->size);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_otp_identical_is_skipped);
  RUN_TEST(test_otp_lock_in_target_is_refused);
  RUN_TEST(test_otp_swapped_bytes_are_not_a_lock);
  RUN_TEST(test_otp_locked_and_different_is_refused);
  RUN_TEST(test_otp_locked_and_identical_is_skipped);
  RUN_TEST(test_otp_different_and_unlocked_is_written);
  RUN_TEST(test_main_flash_is_always_written);
  RUN_TEST(test_timing_words_map_to_registers);
  RUN_TEST(test_timing_fields_do_not_leak);
  RUN_TEST(test_timing_refuses_hsi_other_than_24mhz);
  RUN_TEST(test_timing_follows_factory_hsi_code);
  RUN_TEST(test_find_region_bounds);
  RUN_TEST(test_chip_is_found_by_name);
  RUN_TEST(test_main_region_is_flash);
  return UNITY_END();
}
