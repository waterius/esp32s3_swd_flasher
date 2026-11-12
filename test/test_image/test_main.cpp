// Образ: единицы стирания, которые задел ПК, и отказ на адресах вне областей (design §7).
#include <string.h>
#include <unity.h>

#include "chips/chips.h"
#include "core/image.h"

static image::Image img;

void setUp(void) { img.reset(chips::py32f002b); }
void tearDown(void) {}

static int touched_count(void) {
  int n = 0;
  for (uint16_t i = 0; i < img.unit_count(); i++) n += img.unit(i).touched;
  return n;
}

static int find_unit(uint32_t addr) {
  for (uint16_t i = 0; i < img.unit_count(); i++)
    if (img.unit(i).addr == addr) return i;
  return -1;
}

static void test_py32f002b_has_193_units(void) {
  // 192 страницы основной flash и одна страница USER OTP; ОЗУ не область записи
  TEST_ASSERT_EQUAL_UINT16(193, img.unit_count());
  TEST_ASSERT_TRUE(img.empty());
}

static void test_write_touches_only_its_pages(void) {
  uint8_t data[100];
  memset(data, 0x42, sizeof data);
  TEST_ASSERT_TRUE(img.write(0x08000050, data, sizeof data));  // 0x08000050…0x080000B3
  TEST_ASSERT_EQUAL(2, touched_count());
  TEST_ASSERT_TRUE(img.unit(find_unit(0x08000000)).touched);
  TEST_ASSERT_TRUE(img.unit(find_unit(0x08000080)).touched);
  TEST_ASSERT_FALSE(img.unit(find_unit(0x08000100)).touched);
}

static void test_bytes_outside_image_are_erased_value(void) {
  const uint8_t b = 0x12;
  img.write(0x08000005, &b, 1);
  image::Unit u = img.unit(find_unit(0x08000000));
  TEST_ASSERT_EQUAL_HEX8(0xFF, u.data[4]);
  TEST_ASSERT_EQUAL_HEX8(0x12, u.data[5]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, u.data[127]);
}

static void test_erase_touches_pages_without_data(void) {
  TEST_ASSERT_TRUE(img.erase(0x08003200, 0x100));
  TEST_ASSERT_EQUAL(2, touched_count());
  image::Unit u = img.unit(find_unit(0x08003280));
  TEST_ASSERT_TRUE(u.touched);
  TEST_ASSERT_EQUAL_HEX8(0xFF, u.data[0]);
}

static void test_otp_page_is_a_unit(void) {
  uint8_t data[128];
  memset(data, 0x00, sizeof data);
  TEST_ASSERT_TRUE(img.write(0x1FFF0280, data, sizeof data));
  image::Unit u = img.unit(find_unit(0x1FFF0280));
  TEST_ASSERT_TRUE(u.touched);
  TEST_ASSERT_EQUAL_STRING("otp", u.region->name);
  TEST_ASSERT_EQUAL_UINT32(128, u.len);
}

// spec: "swd-flash/Образ — только из областей чипа/Адрес option bytes"
static void test_write_outside_regions_fails_with_address(void) {
  const uint8_t b = 0;
  TEST_ASSERT_FALSE(img.write(0x1FFF0080, &b, 1));
  TEST_ASSERT_TRUE(img.failed());
  TEST_ASSERT_EQUAL_HEX32(0x1FFF0080, img.fail_addr());
}

static void test_erase_outside_regions_fails(void) {
  TEST_ASSERT_FALSE(img.erase(0x08005F80, 0x100));  // вторая страница — за концом flash
  TEST_ASSERT_TRUE(img.failed());
}

static void test_write_across_region_end_fails(void) {
  uint8_t data[4] = {1, 2, 3, 4};
  TEST_ASSERT_FALSE(img.write(0x08005FFE, data, 4));
  TEST_ASSERT_EQUAL_HEX32(0x08005FFE, img.fail_addr());
}

static void test_failed_image_ignores_further_writes(void) {
  const uint8_t b = 0;
  img.write(0x1FFF0080, &b, 1);
  TEST_ASSERT_FALSE(img.write(0x08000000, &b, 1));
  TEST_ASSERT_EQUAL(0, touched_count());
  img.reset(chips::py32f002b);
  TEST_ASSERT_FALSE(img.failed());
  TEST_ASSERT_TRUE(img.write(0x08000000, &b, 1));
}

static void test_repeated_byte_last_wins(void) {
  const uint8_t a = 0x11, b = 0x22;
  img.write(0x08000010, &a, 1);
  img.write(0x08000010, &b, 1);
  TEST_ASSERT_EQUAL_HEX8(0x22, img.unit(find_unit(0x08000000)).data[0x10]);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_py32f002b_has_193_units);
  RUN_TEST(test_write_touches_only_its_pages);
  RUN_TEST(test_bytes_outside_image_are_erased_value);
  RUN_TEST(test_erase_touches_pages_without_data);
  RUN_TEST(test_otp_page_is_a_unit);
  RUN_TEST(test_write_outside_regions_fails_with_address);
  RUN_TEST(test_erase_outside_regions_fails);
  RUN_TEST(test_write_across_region_end_fails);
  RUN_TEST(test_failed_image_ignores_further_writes);
  RUN_TEST(test_repeated_byte_last_wins);
  return UNITY_END();
}
