// Пакеты gdb на поддельной цели: таблица design §9 и прогон байтов, записанных с gdb 15.2
// из тулчейна PlatformIO (tools/gdb_record.py).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "../fixtures/gdb_attach_fail.h"
#include "../fixtures/gdb_close_mid_write.h"
#include "../fixtures/gdb_flash_fail.h"
#include "../fixtures/gdb_load_failed.h"
#include "../fixtures/gdb_otp_load.h"
#include "../fixtures/gdb_pio_upload.h"
#include "../fixtures/gdb_two_loads.h"
#include "chips/chips.h"
#include "core/gdb.h"
#include "core/rsp.h"

// ---------------------------------------------------------------- поддельная цель

struct FakeTarget {
  bool session;
  bool attach_fails;
  bool flash_fails;
  int starts, ends, flashes, commands, resumes;
  uint8_t flash[24 * 1024];
  uint8_t otp[128];
  bool otp_written;
  uint32_t regs[17];
};
static FakeTarget ft;
static image::Image img;

static const chip::Desc *t_chip(void *) { return &chips::py32f002b; }
static bool t_active(void *) { return ft.session; }
static bool t_start(void *, gdb::LineFn emit, void *ectx) {
  ft.starts++;
  if (ft.attach_fails) {
    if (emit) emit(ectx, "# чип не отвечает");
    return false;
  }
  ft.session = true;
  return true;
}
static void t_end(void *) {
  if (ft.session) ft.ends++;
  ft.session = false;
}
static bool t_read(void *, uint32_t addr, uint8_t *buf, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    uint32_t a = addr + i;
    if (a >= 0x08000000 && a < 0x08006000) buf[i] = ft.flash[a - 0x08000000];
    else if (a >= 0x1FFF0280 && a < 0x1FFF0300) buf[i] = ft.otp[a - 0x1FFF0280];
    else if (a >= 0x20000000 && a < 0x20000C00) buf[i] = 0;
    else return false;
  }
  return true;
}
static bool t_rreg(void *, uint8_t i, uint32_t *v) {
  if (i > 16) return false;
  *v = ft.regs[i];
  return true;
}
static bool t_wreg(void *, uint8_t i, uint32_t v) {
  if (i > 16) return false;
  ft.regs[i] = v;
  return true;
}
static bool t_flash(void *, const image::Image &im) {
  ft.flashes++;
  if (ft.flash_fails) return false;
  for (uint16_t i = 0; i < im.unit_count(); i++) {
    image::Unit u = im.unit(i);
    if (!u.touched) continue;
    if (u.addr >= 0x1FFF0280) {
      memcpy(ft.otp, u.data, 128);
      ft.otp_written = true;
    } else {
      memcpy(ft.flash + (u.addr - 0x08000000), u.data, u.len);
    }
  }
  return true;
}
static void t_command(void *, const char *line, gdb::LineFn emit, void *ectx) {
  ft.commands++;
  if (strcmp(line, "swdp_scan") == 0) {
    if (t_start(nullptr, emit, ectx)) {
      emit(ectx, "# SWD: под сбросом");
      emit(ectx, "1 py32f002b");
    }
    return;
  }
  emit(ectx, "# неизвестная команда");
}

static char printed[512];
static void t_print(void *, const char *line) {
  strncat(printed, line, sizeof printed - strlen(printed) - 2);
  strcat(printed, "|");
}

static const gdb::Target TARGET = {nullptr, t_chip, t_active, t_start,   t_end,  t_read,
                                   t_rreg,  t_wreg, t_flash,  t_command, t_print};

// ---------------------------------------------------------------- что сервер отправил

static char sent[16384];
static size_t sent_len;
static void capture(void *, const uint8_t *data, size_t len) {
  if (sent_len + len < sizeof sent) {
    memcpy(sent + sent_len, data, len);
    sent_len += len;
  }
}

static gdb::Server server(TARGET, img, capture, nullptr);
static rsp::Parser parser;

// Ответы сервера на последний пакет — тела кадров через «|», без подтверждений.
static char replies[8192];
static void decode_sent(void) {
  replies[0] = 0;
  rsp::Parser p;
  for (size_t i = 0; i < sent_len; i++) {
    if (p.feed((uint8_t)sent[i]) == rsp::Event::Packet) {
      size_t n = strlen(replies);
      if (n + p.length() + 2 < sizeof replies) {
        memcpy(replies + n, p.payload(), p.length());
        replies[n + p.length()] = '|';
        replies[n + p.length() + 1] = 0;
      }
    }
  }
}

static const char *ask(const char *payload) {
  sent_len = 0;
  server.on_packet((const uint8_t *)payload, strlen(payload));
  decode_sent();
  return replies;
}

void setUp(void) {
  memset(&ft, 0, sizeof ft);
  printed[0] = 0;
  memset(ft.flash, 0xFF, sizeof ft.flash);
  memset(ft.otp, 0xFF, sizeof ft.otp);
  server.reset_connection();
  ask("QStartNoAckMode");
}
void tearDown(void) {}

// ---------------------------------------------------------------- пакеты

static void test_ack_before_noack(void) {
  server.reset_connection();
  sent_len = 0;
  server.on_packet((const uint8_t *)"?", 1);
  TEST_ASSERT_EQUAL_CHAR('+', sent[0]);
  ask("QStartNoAckMode");
  sent_len = 0;
  server.on_packet((const uint8_t *)"?", 1);
  TEST_ASSERT_EQUAL_CHAR('$', sent[0]);
}

static void test_qsupported_announces_packet_size_and_xfer(void) {
  TEST_ASSERT_EQUAL_STRING("PacketSize=1000;qXfer:memory-map:read+;qXfer:features:read+;QStartNoAckMode+|",
                           ask("qSupported:multiprocess+;swbreak+"));
}

static void test_halt_reason_depends_on_session(void) {
  TEST_ASSERT_EQUAL_STRING("W00|", ask("?"));
  ask("vAttach;1");
  TEST_ASSERT_EQUAL_STRING("T05|", ask("?"));
}

static void test_memory_map_lists_chip_regions(void) {
  const char *r = ask("qXfer:memory-map:read::0,1000");
  TEST_ASSERT_EQUAL_CHAR('l', r[0]);
  TEST_ASSERT_NOT_NULL(strstr(r, "<memory type=\"flash\" start=\"0x8000000\" length=\"0x6000\"><property name=\"blocksize\">0x80</property></memory>"));
  TEST_ASSERT_NOT_NULL(strstr(r, "<memory type=\"flash\" start=\"0x1fff0280\" length=\"0x80\"><property name=\"blocksize\">0x80</property></memory>"));
  TEST_ASSERT_NOT_NULL(strstr(r, "<memory type=\"ram\" start=\"0x20000000\" length=\"0xc00\"/>"));
}

static void test_xfer_is_read_in_pieces(void) {
  const char *first = ask("qXfer:features:read:target.xml:0,10");
  TEST_ASSERT_EQUAL_CHAR('m', first[0]);
  TEST_ASSERT_EQUAL(1 + 0x10 + 1, strlen(first));  // «m», 16 символов, «|»
}

static void test_monitor_output_goes_as_console_packets(void) {
  char hex[64];
  size_t n = rsp::to_hex((const uint8_t *)"swdp_scan", 9, hex);
  hex[n] = 0;
  char pkt[80];
  snprintf(pkt, sizeof pkt, "qRcmd,%s", hex);
  const char *r = ask(pkt);
  // «1 py32f002b\n» в hex: 31 20 70 79 33 32 66 30 30 32 62 0a
  TEST_ASSERT_NOT_NULL(strstr(r, "O31207079333266303032620a|"));
  TEST_ASSERT_NOT_NULL(strstr(r, "|OK|"));
  TEST_ASSERT_TRUE(ft.session);
}

static void test_registers_in_profile_order(void) {
  ask("vAttach;1");
  for (int i = 0; i < 17; i++) ft.regs[i] = 0x11111111u * (uint32_t)(i % 15 + 1);
  ft.regs[15] = 0x080000C1;
  const char *r = ask("g");
  TEST_ASSERT_EQUAL(17 * 8 + 1, strlen(r));
  TEST_ASSERT_EQUAL_MEMORY("c1000008", r + 15 * 8, 8);  // pc, младший байт первым
  ask("Pf=3c450008");                                    // gdb ставит PC после load
  TEST_ASSERT_EQUAL_HEX32(0x0800453C, ft.regs[15]);
  ft.regs[16] = 1;
  TEST_ASSERT_EQUAL_STRING("01000000|", ask("p19"));  // xpsr — regnum 25 (0x19)
}

static void test_memory_read_and_errors(void) {
  TEST_ASSERT_EQUAL_STRING("E01|", ask("m8000000,4"));  // без сеанса
  ask("vAttach;1");
  ft.flash[0] = 0x00;
  ft.flash[1] = 0x0c;
  TEST_ASSERT_EQUAL_STRING("000cffff|", ask("m8000000,4"));
  TEST_ASSERT_EQUAL_STRING("E01|", ask("m40000000,4"));  // шина ответила ошибкой
}

// spec: "gdb-server/Отладки нет/continue"
static void test_debugging_is_refused(void) {
  ask("vAttach;1");
  TEST_ASSERT_EQUAL_STRING("E01|", ask("c"));
  TEST_ASSERT_EQUAL_STRING("E01|", ask("s"));
  TEST_ASSERT_EQUAL_STRING("E01|", ask("vCont;c"));
  TEST_ASSERT_EQUAL_STRING("E01|", ask("Z1,8002b04,2"));
  TEST_ASSERT_EQUAL_STRING("E01|", ask("M20000000,4:01000000"));
  TEST_ASSERT_TRUE(ft.session);  // ядро стоит, сеанс идёт
}

static void test_unknown_packets_get_empty_reply(void) {
  TEST_ASSERT_EQUAL_STRING("|", ask("vCont?"));
  TEST_ASSERT_EQUAL_STRING("|", ask("vMustReplyEmpty"));
  TEST_ASSERT_EQUAL_STRING("|", ask("qCRC:8000000,c0"));
}

static void test_flash_without_session_is_refused(void) {
  TEST_ASSERT_EQUAL_STRING("E01|", ask("vFlashErase:08000000,00000080"));
  TEST_ASSERT_EQUAL_STRING("E01|", ask("vFlashDone"));
  TEST_ASSERT_EQUAL(0, ft.flashes);
}

static void test_kill_without_write_is_an_error_detach_is_not(void) {
  ask("vAttach;1");
  TEST_ASSERT_EQUAL_STRING("E01|", ask("vKill;1"));
  TEST_ASSERT_FALSE(ft.session);
  ask("vAttach;1");
  TEST_ASSERT_EQUAL_STRING("OK|", ask("D"));
  TEST_ASSERT_FALSE(ft.session);
}

static void test_kill_and_detach_leave_gdb_mode(void) {
  ask("vAttach;1");
  TEST_ASSERT_TRUE(server.on_packet((const uint8_t *)"D", 1));
  TEST_ASSERT_FALSE(server.on_packet((const uint8_t *)"?", 1));
}

static void test_write_outside_regions_fails_and_next_erase_starts_new_image(void) {
  ask("vAttach;1");
  TEST_ASSERT_EQUAL_STRING("E01|", ask("vFlashErase:1fff0080,00000080"));
  // причина видна и тому, кто шлёт пакеты не через gdb: gdb после ошибки vFlashDone не шлёт
  TEST_ASSERT_EQUAL_STRING("# адрес вне памяти чипа: 0x1FFF0080|# итог: ошибка, адрес вне памяти чипа|", printed);
  TEST_ASSERT_EQUAL_STRING("OK|", ask("vFlashErase:08000000,00000080"));
  TEST_ASSERT_EQUAL_STRING("OK|", ask("vFlashDone"));
  TEST_ASSERT_EQUAL(1, ft.flashes);
  TEST_ASSERT_EQUAL_STRING("E01|", ask("vKill;1"));  // в сеансе была неудача
}

// ---------------------------------------------------------------- прогон записанных байтов

struct Pair {
  char req[48];
  char rep[48];
};
static Pair pairs[256];
static int pair_count;

static void replay(const uint8_t *data, size_t len) {
  server.reset_connection();
  parser.reset();
  pair_count = 0;
  for (size_t i = 0; i < len; i++) {
    if (parser.feed(data[i]) != rsp::Event::Packet) continue;
    sent_len = 0;
    server.on_packet(parser.payload(), parser.length());
    decode_sent();
    if (pair_count < 256) {
      Pair &p = pairs[pair_count++];
      size_t n = parser.length() < sizeof p.req - 1 ? parser.length() : sizeof p.req - 1;
      memcpy(p.req, parser.payload(), n);
      p.req[n] = 0;
      snprintf(p.rep, sizeof p.rep, "%s", replies);
    }
  }
}

static const char *reply_to(const char *prefix) {
  for (int i = pair_count; i-- > 0;)
    if (strncmp(pairs[i].req, prefix, strlen(prefix)) == 0) return pairs[i].rep;
  return "(нет такого пакета)";
}

static void test_replay_pio_upload(void) {
  replay(GDB_PIO_UPLOAD, GDB_PIO_UPLOAD_LEN);
  TEST_ASSERT_EQUAL(1, ft.flashes);
  TEST_ASSERT_EQUAL_STRING("OK|", reply_to("vFlashDone"));
  TEST_ASSERT_EQUAL_STRING("OK|", reply_to("vKill"));
  // начало образа — таблица векторов нейтральной прошивки: SP = 0x20000c00
  const uint8_t sp[4] = {0x00, 0x0c, 0x00, 0x20};
  TEST_ASSERT_EQUAL_MEMORY(sp, ft.flash, 4);
  TEST_ASSERT_EQUAL_HEX8(0xA5, ft.flash[0x200]);  // константа pattern в .rodata
  TEST_ASSERT_FALSE(ft.session);
}

static void test_replay_otp_load(void) {
  replay(GDB_OTP_LOAD, GDB_OTP_LOAD_LEN);
  TEST_ASSERT_TRUE(ft.otp_written);
  TEST_ASSERT_EQUAL_HEX8(0xFF, ft.otp[0]);
  TEST_ASSERT_EQUAL_HEX8(0x04, ft.otp[4]);  // узор файла otp.hex
  TEST_ASSERT_EQUAL_STRING("OK|", reply_to("vKill"));
}

static void test_replay_two_loads(void) {
  replay(GDB_TWO_LOADS, GDB_TWO_LOADS_LEN);
  TEST_ASSERT_EQUAL(2, ft.flashes);
  TEST_ASSERT_TRUE(ft.otp_written);
  TEST_ASSERT_EQUAL_STRING("OK|", reply_to("vKill"));
}

// spec: "gdb-server/Провал заливки виден PlatformIO/Образ отвергнут"
static void test_replay_flash_fail(void) {
  ft.flash_fails = true;
  replay(GDB_FLASH_FAIL, GDB_FLASH_FAIL_LEN);
  TEST_ASSERT_EQUAL_STRING("E01|", reply_to("vFlashDone"));
  TEST_ASSERT_EQUAL_STRING("E01|", reply_to("vKill"));
  TEST_ASSERT_EQUAL_STRING("OK|", reply_to("D"));
  TEST_ASSERT_FALSE(ft.session);
}

// spec: "gdb-server/Провал заливки виден PlatformIO/Образ не помещается в чип"
static void test_replay_load_failed(void) {
  replay(GDB_LOAD_FAILED, GDB_LOAD_FAILED_LEN);
  TEST_ASSERT_EQUAL(0, ft.flashes);
  TEST_ASSERT_EQUAL_STRING("E01|", reply_to("vKill"));
}

// spec: "gdb-server/Память и регистры для gdb/attach не удался"
static void test_replay_attach_fail(void) {
  ft.attach_fails = true;
  replay(GDB_ATTACH_FAIL, GDB_ATTACH_FAIL_LEN);
  TEST_ASSERT_EQUAL_STRING("E01|", reply_to("vAttach"));
  TEST_ASSERT_EQUAL_STRING("E01|", reply_to("vFlashErase"));
  TEST_ASSERT_EQUAL(0, ft.flashes);
}

static void test_replay_close_mid_write_then_leave_ends_session(void) {
  replay(GDB_CLOSE_MID_WRITE, GDB_CLOSE_MID_WRITE_LEN);
  TEST_ASSERT_TRUE(ft.session);  // gdb пропал посреди записи
  server.on_leave();            // порт закрылся
  TEST_ASSERT_FALSE(ft.session);
  TEST_ASSERT_EQUAL(0, ft.flashes);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ack_before_noack);
  RUN_TEST(test_qsupported_announces_packet_size_and_xfer);
  RUN_TEST(test_halt_reason_depends_on_session);
  RUN_TEST(test_memory_map_lists_chip_regions);
  RUN_TEST(test_xfer_is_read_in_pieces);
  RUN_TEST(test_monitor_output_goes_as_console_packets);
  RUN_TEST(test_registers_in_profile_order);
  RUN_TEST(test_memory_read_and_errors);
  RUN_TEST(test_debugging_is_refused);
  RUN_TEST(test_unknown_packets_get_empty_reply);
  RUN_TEST(test_flash_without_session_is_refused);
  RUN_TEST(test_kill_without_write_is_an_error_detach_is_not);
  RUN_TEST(test_kill_and_detach_leave_gdb_mode);
  RUN_TEST(test_write_outside_regions_fails_and_next_erase_starts_new_image);
  RUN_TEST(test_replay_pio_upload);
  RUN_TEST(test_replay_otp_load);
  RUN_TEST(test_replay_two_loads);
  RUN_TEST(test_replay_flash_fail);
  RUN_TEST(test_replay_load_failed);
  RUN_TEST(test_replay_attach_fail);
  RUN_TEST(test_replay_close_mid_write_then_leave_ends_session);
  return UNITY_END();
}
