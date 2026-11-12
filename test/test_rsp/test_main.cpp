// Кадры протокола gdb: $…#xx, контрольная сумма, экранирование, hex (design §9).
#include <string.h>
#include <unity.h>

#include "core/rsp.h"

void setUp(void) {}
void tearDown(void) {}

static rsp::Parser parser;

static rsp::Event feed_str(const char *s, size_t n) {
  rsp::Event last = rsp::Event::None;
  for (size_t i = 0; i < n; i++) {
    rsp::Event e = parser.feed((uint8_t)s[i]);
    if (e != rsp::Event::None) last = e;
  }
  return last;
}

static void test_checksum_of_ok(void) {
  TEST_ASSERT_EQUAL_HEX8(0x9a, rsp::checksum((const uint8_t *)"OK", 2));
}

static void test_frame_ok(void) {
  uint8_t out[16];
  size_t n = rsp::frame((const uint8_t *)"OK", 2, out, sizeof out);
  TEST_ASSERT_EQUAL(6, n);
  TEST_ASSERT_EQUAL_MEMORY("$OK#9a", out, 6);
}

static void test_frame_escapes_special_bytes(void) {
  uint8_t out[16];
  size_t n = rsp::frame((const uint8_t *)"a#", 2, out, sizeof out);
  // 'a' 0x61, '}' 0x7d, '#' ^ 0x20 = 0x03 → сумма 0xe1
  const uint8_t want[] = {'$', 'a', 0x7d, 0x03, '#', 'e', '1'};
  TEST_ASSERT_EQUAL(sizeof want, n);
  TEST_ASSERT_EQUAL_MEMORY(want, out, n);
}

static void test_frame_refuses_small_buffer(void) {
  uint8_t out[5];
  TEST_ASSERT_EQUAL(0, rsp::frame((const uint8_t *)"OK", 2, out, sizeof out));
}

static void test_parser_reads_packet(void) {
  parser.reset();
  TEST_ASSERT_EQUAL(rsp::Event::Packet, feed_str("$OK#9a", 6));
  TEST_ASSERT_EQUAL(2, parser.length());
  TEST_ASSERT_EQUAL_MEMORY("OK", parser.payload(), 2);
}

static void test_parser_accepts_uppercase_checksum(void) {
  parser.reset();
  TEST_ASSERT_EQUAL(rsp::Event::Packet, feed_str("$OK#9A", 6));
}

static void test_parser_rejects_wrong_checksum(void) {
  parser.reset();
  TEST_ASSERT_EQUAL(rsp::Event::BadChecksum, feed_str("$OK#00", 6));
}

static void test_parser_unescapes_binary(void) {
  parser.reset();
  const char raw[] = {'$', 'X', 0x7d, 0x03, '#', 'd', '8'};  // 0x58 + 0x7d + 0x03 = 0xd8
  TEST_ASSERT_EQUAL(rsp::Event::Packet, feed_str(raw, sizeof raw));
  TEST_ASSERT_EQUAL(2, parser.length());
  TEST_ASSERT_EQUAL_MEMORY("X#", parser.payload(), 2);
}

static void test_parser_bytes_outside_packet(void) {
  parser.reset();
  TEST_ASSERT_EQUAL(rsp::Event::Ack, parser.feed('+'));
  TEST_ASSERT_EQUAL(rsp::Event::Nak, parser.feed('-'));
  TEST_ASSERT_EQUAL(rsp::Event::Interrupt, parser.feed(0x03));
  TEST_ASSERT_EQUAL(rsp::Event::Other, parser.feed('i'));
}

static void test_parser_in_packet_flag(void) {
  parser.reset();
  TEST_ASSERT_FALSE(parser.in_packet());
  parser.feed('$');
  TEST_ASSERT_TRUE(parser.in_packet());
  feed_str("OK#9a", 5);
  TEST_ASSERT_FALSE(parser.in_packet());
}

static void test_parser_oversize_packet_is_bad(void) {
  parser.reset();
  parser.feed('$');
  rsp::Event e = rsp::Event::None;
  for (size_t i = 0; i < rsp::MAX_PACKET + 10 && e == rsp::Event::None; i++) e = parser.feed('a');
  TEST_ASSERT_EQUAL(rsp::Event::BadChecksum, e);
  TEST_ASSERT_FALSE(parser.in_packet());
}

static void test_hex_roundtrip(void) {
  const uint8_t data[] = {0x01, 0xab, 0x00};
  char hex[8];
  TEST_ASSERT_EQUAL(6, rsp::to_hex(data, 3, hex));
  TEST_ASSERT_EQUAL_MEMORY("01ab00", hex, 6);
  uint8_t back[3];
  TEST_ASSERT_TRUE(rsp::from_hex("01AB00", 6, back));
  TEST_ASSERT_EQUAL_MEMORY(data, back, 3);
  TEST_ASSERT_FALSE(rsp::from_hex("0g", 2, back));
  TEST_ASSERT_FALSE(rsp::from_hex("012", 3, back));
}

static void test_parse_hex_u32(void) {
  uint32_t v = 0;
  TEST_ASSERT_TRUE(rsp::parse_hex_u32("8000000", 7, &v));
  TEST_ASSERT_EQUAL_HEX32(0x08000000, v);
  TEST_ASSERT_TRUE(rsp::parse_hex_u32("1fff0280", 8, &v));
  TEST_ASSERT_EQUAL_HEX32(0x1FFF0280, v);
  TEST_ASSERT_FALSE(rsp::parse_hex_u32("", 0, &v));
  TEST_ASSERT_FALSE(rsp::parse_hex_u32("123456789", 9, &v));
  TEST_ASSERT_FALSE(rsp::parse_hex_u32("12z", 3, &v));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_checksum_of_ok);
  RUN_TEST(test_frame_ok);
  RUN_TEST(test_frame_escapes_special_bytes);
  RUN_TEST(test_frame_refuses_small_buffer);
  RUN_TEST(test_parser_reads_packet);
  RUN_TEST(test_parser_accepts_uppercase_checksum);
  RUN_TEST(test_parser_rejects_wrong_checksum);
  RUN_TEST(test_parser_unescapes_binary);
  RUN_TEST(test_parser_bytes_outside_packet);
  RUN_TEST(test_parser_in_packet_flag);
  RUN_TEST(test_parser_oversize_packet_is_bad);
  RUN_TEST(test_hex_roundtrip);
  RUN_TEST(test_parse_hex_u32);
  return UNITY_END();
}
