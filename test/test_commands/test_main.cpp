// Команды оснастки: одни и те же строкой и через monitor (design §11).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "chips/chips.h"
#include "core/commands.h"
#include "core/gdb.h"
#include "core/rsp.h"

// ---------------------------------------------------------------- поддельный хозяин

struct FakeHost {
  const chip::Desc *chip;
  int set_chip_calls;
  bool session;
  int starts, ends, resets, infos;
  bool start_fails;
  uint32_t baud;
};
static FakeHost fh;

static const chip::Desc *h_chip(void *) { return fh.chip; }
static void h_set_chip(void *, const chip::Desc *d) {
  fh.chip = d;
  fh.set_chip_calls++;
}
static bool h_active(void *) { return fh.session; }
static bool h_start(void *, gdb::LineFn emit, void *ectx) {
  fh.starts++;
  if (fh.start_fails) {
    emit(ectx, "# чип не отвечает");
    return false;
  }
  emit(ectx, "# SWD: под сбросом");
  return fh.session = true;
}
static void h_end(void *) {
  fh.ends++;
  fh.session = false;
}
static void h_info(void *, gdb::LineFn emit, void *ectx) {
  fh.infos++;
  emit(ectx, "# UID: 0x00112233");
}
static void h_reset(void *) { fh.resets++; }
static uint32_t h_baud(void *) { return fh.baud; }
static void h_set_baud(void *, uint32_t b) { fh.baud = b; }

static const commands::Host HOST = {nullptr, h_chip,  h_set_chip, h_active,  h_start, h_end,
                                    h_info,  h_reset, h_baud,     h_set_baud};

static char out[2048];
static void collect(void *, const char *line) {
  strncat(out, line, sizeof out - strlen(out) - 2);
  strcat(out, "|");
}
static const char *run(const char *line) {
  out[0] = 0;
  commands::run(HOST, line, collect, nullptr);
  return out;
}

void setUp(void) {
  memset(&fh, 0, sizeof fh);
  fh.baud = 115200;
}
void tearDown(void) {}

// ---------------------------------------------------------------- чип

static void test_chip_without_selection_shows_hint_and_list(void) {
  TEST_ASSERT_EQUAL_STRING("# чип не выбран: chip <имя>|# поддержаны: py32f002b|", run("chip"));
}

static void test_chip_selects_known_chip(void) {
  TEST_ASSERT_EQUAL_STRING("# чип: py32f002b|", run("chip py32f002b"));
  TEST_ASSERT_EQUAL_PTR(&chips::py32f002b, fh.chip);
  TEST_ASSERT_EQUAL_STRING("# чип: py32f002b|# поддержаны: py32f002b|", run("chip"));
}

// spec: "swd-flash/Чип выбирается аргументом/Неизвестный чип"
static void test_unknown_chip_is_refused_and_selection_kept(void) {
  fh.chip = &chips::py32f002b;
  TEST_ASSERT_EQUAL_STRING("# чип не поддержан: stm32f103|# поддержаны: py32f002b|", run("chip stm32f103"));
  TEST_ASSERT_EQUAL_PTR(&chips::py32f002b, fh.chip);
  TEST_ASSERT_EQUAL(0, fh.set_chip_calls);
}

// spec: "swd-flash/Чип выбирается аргументом/Чип не выбран"
static void test_scan_without_chip_does_not_connect(void) {
  TEST_ASSERT_EQUAL_STRING("# чип не выбран: chip <имя>|", run("swdp_scan"));
  TEST_ASSERT_EQUAL(0, fh.starts);
}

static void test_info_without_chip_does_not_connect(void) {
  TEST_ASSERT_EQUAL_STRING("# чип не выбран: chip <имя>|", run("info"));
  TEST_ASSERT_EQUAL(0, fh.starts);
}

static void test_chip_change_refused_during_session(void) {
  fh.chip = &chips::py32f002b;
  fh.session = true;
  TEST_ASSERT_EQUAL_STRING("# идёт сеанс|", run("chip py32f002b"));
  TEST_ASSERT_EQUAL(0, fh.set_chip_calls);
}

// ---------------------------------------------------------------- сеанс

static void test_scan_starts_session_and_names_chip(void) {
  fh.chip = &chips::py32f002b;
  TEST_ASSERT_EQUAL_STRING("# SWD: под сбросом|1 py32f002b|", run("swdp_scan"));
  TEST_ASSERT_TRUE(fh.session);  // сеанс остаётся для attach
}

static void test_scan_failure_lists_no_chip(void) {
  fh.chip = &chips::py32f002b;
  fh.start_fails = true;
  TEST_ASSERT_EQUAL_STRING("# чип не отвечает|", run("swdp_scan"));
}

static void test_info_outside_session_is_a_short_session(void) {
  fh.chip = &chips::py32f002b;
  TEST_ASSERT_EQUAL_STRING("# SWD: под сбросом|# UID: 0x00112233|", run("info"));
  TEST_ASSERT_EQUAL(1, fh.ends);  // плата перезапущена
  TEST_ASSERT_FALSE(fh.session);
}

static void test_info_in_session_keeps_session(void) {
  fh.chip = &chips::py32f002b;
  fh.session = true;
  TEST_ASSERT_EQUAL_STRING("# UID: 0x00112233|", run("info"));
  TEST_ASSERT_EQUAL(0, fh.ends);
  TEST_ASSERT_EQUAL(0, fh.starts);
}

static void test_reset_restarts_board(void) {
  TEST_ASSERT_EQUAL_STRING("# сброс платы|", run("reset"));
  TEST_ASSERT_EQUAL(1, fh.resets);
}

static void test_reset_refused_during_session(void) {
  fh.session = true;
  TEST_ASSERT_EQUAL_STRING("# идёт сеанс|", run("reset"));
  TEST_ASSERT_EQUAL(0, fh.resets);
}

// ---------------------------------------------------------------- лог

static void test_baud_sets_speed(void) {
  TEST_ASSERT_EQUAL_STRING("# лог: 9600|", run("baud 9600"));
  TEST_ASSERT_EQUAL_UINT32(9600, fh.baud);
  TEST_ASSERT_EQUAL_STRING("# лог: 9600|", run("baud"));
}

// spec: "target-log/Скорость лога/Неверная скорость"
static void test_baud_out_of_range_is_refused(void) {
  TEST_ASSERT_EQUAL_STRING("# скорость лога: от 1200 до 2000000|", run("baud 7"));
  TEST_ASSERT_EQUAL_STRING("# скорость лога: от 1200 до 2000000|", run("baud 2000001"));
  TEST_ASSERT_EQUAL_STRING("# скорость лога: от 1200 до 2000000|", run("baud 9600x"));
  TEST_ASSERT_EQUAL_UINT32(115200, fh.baud);
}

// ---------------------------------------------------------------- прочее

// spec: "gdb-server/Команды строкой и через monitor/Неизвестная команда"
static void test_unknown_command_prints_help(void) {
  const char *r = run("abc");
  TEST_ASSERT_NOT_NULL(strstr(r, "# команды:|"));
  TEST_ASSERT_NOT_NULL(strstr(r, "chip [имя]"));
}

static void test_empty_line_prints_nothing(void) { TEST_ASSERT_EQUAL_STRING("", run("")); }

// spec: "gdb-server/Команды строкой и через monitor/Команда из gdb"
static image::Image img;
static char sent[4096];
static size_t sent_len;
static void capture(void *, const uint8_t *d, size_t n) {
  if (sent_len + n < sizeof sent) {
    memcpy(sent + sent_len, d, n);
    sent_len += n;
  }
}
static const chip::Desc *t_chip(void *) { return fh.chip; }
static bool t_active(void *c) { return h_active(c); }
static bool t_start(void *c, gdb::LineFn e, void *x) { return h_start(c, e, x); }
static void t_end(void *c) { h_end(c); }
static bool t_read(void *, uint32_t, uint8_t *, uint32_t) { return false; }
static bool t_rreg(void *, uint8_t, uint32_t *) { return false; }
static bool t_wreg(void *, uint8_t, uint32_t) { return false; }
static bool t_flash(void *, const image::Image &) { return false; }
static void t_command(void *, const char *line, gdb::LineFn emit, void *ectx) {
  commands::run(HOST, line, emit, ectx);
}
static const gdb::Target TARGET = {nullptr, t_chip, t_active, t_start, t_end, t_read,
                                   t_rreg,  t_wreg, t_flash,  t_command};

static void test_monitor_prints_the_same_lines(void) {
  fh.chip = &chips::py32f002b;
  fh.session = true;
  const char *text = run("info");
  char want[256];
  snprintf(want, sizeof want, "%s", text);

  gdb::Server server(TARGET, img, capture, nullptr);
  char hex[32];
  size_t n = rsp::to_hex((const uint8_t *)"info", 4, hex);
  char pkt[48];
  snprintf(pkt, sizeof pkt, "qRcmd,%.*s", (int)n, hex);
  sent_len = 0;
  server.on_packet((const uint8_t *)pkt, strlen(pkt));
  // собрать строки из пакетов O<hex>; «OK» — конец ответа, не вывод
  char got[256] = "";
  rsp::Parser p;
  for (size_t i = 0; i < sent_len; i++) {
    if (p.feed((uint8_t)sent[i]) != rsp::Event::Packet || p.payload()[0] != 'O') continue;
    if (p.length() == 2 && p.payload()[1] == 'K') continue;
    char line[128];
    size_t k = (p.length() - 1) / 2;
    rsp::from_hex((const char *)p.payload() + 1, p.length() - 1, (uint8_t *)line);
    line[k - 1] = 0;  // без «\n»
    strcat(got, line);
    strcat(got, "|");
  }
  TEST_ASSERT_EQUAL_STRING(want, got);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_chip_without_selection_shows_hint_and_list);
  RUN_TEST(test_chip_selects_known_chip);
  RUN_TEST(test_unknown_chip_is_refused_and_selection_kept);
  RUN_TEST(test_scan_without_chip_does_not_connect);
  RUN_TEST(test_info_without_chip_does_not_connect);
  RUN_TEST(test_chip_change_refused_during_session);
  RUN_TEST(test_scan_starts_session_and_names_chip);
  RUN_TEST(test_scan_failure_lists_no_chip);
  RUN_TEST(test_info_outside_session_is_a_short_session);
  RUN_TEST(test_info_in_session_keeps_session);
  RUN_TEST(test_reset_restarts_board);
  RUN_TEST(test_reset_refused_during_session);
  RUN_TEST(test_baud_sets_speed);
  RUN_TEST(test_baud_out_of_range_is_refused);
  RUN_TEST(test_unknown_command_prints_help);
  RUN_TEST(test_empty_line_prints_nothing);
  RUN_TEST(test_monitor_prints_the_same_lines);
  return UNITY_END();
}
