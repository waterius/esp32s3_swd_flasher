// План записи: писать, пропустить или отказать — до первого стирания (design §7).
#include <string.h>
#include <unity.h>

#include "chips/chips.h"
#include "core/image.h"
#include "core/plan.h"

static image::Image img;
static plan::Plan pl;

// Поддельная память чипа: основная flash и USER OTP.
struct FakeChip {
  uint8_t flash[24 * 1024];
  uint8_t otp[128];
  int reads_flash;
  int reads_otp;
  bool fail_reads;
};
static FakeChip fake;

static bool fake_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len) {
  FakeChip *c = static_cast<FakeChip *>(ctx);
  if (c->fail_reads) return false;
  if (addr >= 0x1FFF0280 && addr + len <= 0x1FFF0300) {
    c->reads_otp++;
    memcpy(buf, c->otp + (addr - 0x1FFF0280), len);
    return true;
  }
  c->reads_flash++;
  memcpy(buf, c->flash + (addr - 0x08000000), len);
  return true;
}

void setUp(void) {
  img.reset(chips::py32f002b);
  memset(&fake, 0, sizeof fake);
  memset(fake.flash, 0xFF, sizeof fake.flash);
  memset(fake.otp, 0xFF, sizeof fake.otp);
}
void tearDown(void) {}

static uint16_t unit_of(uint32_t addr) {
  for (uint16_t i = 0; i < img.unit_count(); i++)
    if (img.unit(i).addr == addr) return i;
  TEST_FAIL_MESSAGE("нет единицы");
  return 0;
}

static void make(void) { plan::make(pl, img, chips::py32f002b, fake_read, &fake); }

static void put_otp_image(const uint8_t *bytes) { TEST_ASSERT_TRUE(img.write(0x1FFF0280, bytes, 128)); }

static void test_untouched_units_are_left_alone(void) {
  const uint8_t b = 1;
  img.write(0x08000000, &b, 1);
  make();
  TEST_ASSERT_TRUE(pl.ok);
  TEST_ASSERT_EQUAL(plan::Action::Write, pl.action[unit_of(0x08000000)]);
  TEST_ASSERT_EQUAL(plan::Action::None, pl.action[unit_of(0x08000080)]);
  TEST_ASSERT_EQUAL(plan::Action::None, pl.action[unit_of(0x1FFF0280)]);
  TEST_ASSERT_EQUAL_UINT16(1, pl.write_count);
}

static void test_main_flash_is_not_read(void) {
  const uint8_t b = 1;
  img.write(0x08000000, &b, 1);
  make();
  TEST_ASSERT_EQUAL(0, fake.reads_flash);
}

// spec: "swd-flash/USER OTP PY32F002B не запирается/OTP уже такая"
static void test_identical_otp_is_skipped(void) {
  uint8_t otp[128];
  memset(otp, 0x33, sizeof otp);
  memcpy(fake.otp, otp, 128);
  put_otp_image(otp);
  make();
  TEST_ASSERT_TRUE(pl.ok);
  TEST_ASSERT_EQUAL(plan::Action::Skip, pl.action[unit_of(0x1FFF0280)]);
  TEST_ASSERT_EQUAL_UINT16(0, pl.write_count);
  TEST_ASSERT_EQUAL_UINT16(1, pl.skip_count);
}

// spec: "swd-flash/USER OTP PY32F002B не запирается/Образ с замком"
static void test_lock_in_image_refuses_whole_plan(void) {
  const uint8_t b = 0;
  img.write(0x08000000, &b, 1);  // и основная flash тоже не будет тронута
  uint8_t otp[128];
  memset(otp, 0xFF, sizeof otp);
  otp[0] = 0x55;
  otp[1] = 0xAA;
  put_otp_image(otp);
  make();
  TEST_ASSERT_FALSE(pl.ok);
  TEST_ASSERT_EQUAL_STRING("замок OTP не ставится", pl.reason);
  TEST_ASSERT_EQUAL_HEX32(0x1FFF0280, pl.addr);
  TEST_ASSERT_EQUAL_UINT16(0, pl.write_count);
}

// spec: "swd-flash/USER OTP PY32F002B не запирается/Заперта и совпадает"
static void test_locked_and_identical_writes_flash_only(void) {
  memset(fake.otp, 0x00, sizeof fake.otp);
  fake.otp[0] = 0x55;
  fake.otp[1] = 0xAA;
  put_otp_image(fake.otp);
  const uint8_t b = 7;
  img.write(0x08000100, &b, 1);
  make();
  TEST_ASSERT_TRUE(pl.ok);
  TEST_ASSERT_EQUAL(plan::Action::Skip, pl.action[unit_of(0x1FFF0280)]);
  TEST_ASSERT_EQUAL(plan::Action::Write, pl.action[unit_of(0x08000100)]);
}

// spec: "swd-flash/USER OTP PY32F002B не запирается/Заперта и отличается"
static void test_locked_and_different_refuses(void) {
  memset(fake.otp, 0x00, sizeof fake.otp);
  fake.otp[0] = 0x55;
  fake.otp[1] = 0xAA;
  uint8_t otp[128];
  memset(otp, 0x01, sizeof otp);
  put_otp_image(otp);
  make();
  TEST_ASSERT_FALSE(pl.ok);
  TEST_ASSERT_EQUAL_STRING("OTP заперта", pl.reason);
}

static void test_read_failure_refuses(void) {
  uint8_t otp[128];
  memset(otp, 0x01, sizeof otp);
  put_otp_image(otp);
  fake.fail_reads = true;
  make();
  TEST_ASSERT_FALSE(pl.ok);
  TEST_ASSERT_EQUAL_STRING("чтение не удалось", pl.reason);
  TEST_ASSERT_EQUAL_HEX32(0x1FFF0280, pl.addr);
}

static void test_failed_image_refuses(void) {
  const uint8_t b = 0;
  img.write(0x1FFF0080, &b, 1);
  make();
  TEST_ASSERT_FALSE(pl.ok);
  TEST_ASSERT_EQUAL_STRING("адрес вне памяти чипа", pl.reason);
  TEST_ASSERT_EQUAL_HEX32(0x1FFF0080, pl.addr);
}

static void test_empty_image_refuses(void) {
  make();
  TEST_ASSERT_FALSE(pl.ok);
  TEST_ASSERT_EQUAL_STRING("образа нет", pl.reason);
}

// spec: "swd-flash/На плате тот чип, что выбран/Другой чип на плате"
static void test_other_chip_id_does_not_match(void) {
  TEST_ASSERT_FALSE(plan::id_matches(chips::py32f002b, 0x10006440));
  TEST_ASSERT_TRUE(plan::id_matches(chips::py32f002b, 0x20200061));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_untouched_units_are_left_alone);
  RUN_TEST(test_main_flash_is_not_read);
  RUN_TEST(test_identical_otp_is_skipped);
  RUN_TEST(test_lock_in_image_refuses_whole_plan);
  RUN_TEST(test_locked_and_identical_writes_flash_only);
  RUN_TEST(test_locked_and_different_refuses);
  RUN_TEST(test_read_failure_refuses);
  RUN_TEST(test_failed_image_refuses);
  RUN_TEST(test_empty_image_refuses);
  RUN_TEST(test_other_chip_id_does_not_match);
  return UNITY_END();
}
