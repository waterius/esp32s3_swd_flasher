// Режимы порта и вывод: кто открыл порт, текст и gdb, буфер 4 КБ (design §9, §10).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "../fixtures/gdb_pio_upload.h"
#include "chips/chips.h"
#include "core/console.h"
#include "core/gdb.h"
#include "core/rsp.h"

// ---------------------------------------------------------------- поддельная цель и команды

static bool session;
static int ends;
static bool t_active(void *) { return session; }
static const chip::Desc *t_chip(void *) { return &chips::py32f002b; }
static bool t_start(void *, gdb::LineFn, void *) { return session = true; }
static void t_end(void *) {
  if (session) ends++;
  session = false;
}
static bool t_read(void *, uint32_t, uint8_t *buf, uint32_t len) {
  memset(buf, 0xFF, len);
  return true;
}
static bool t_rreg(void *, uint8_t, uint32_t *v) { return *v = 0, true; }
static bool t_wreg(void *, uint8_t, uint32_t) { return true; }
static bool t_flash(void *, const image::Image &) { return true; }
static gdb::Outcome t_command(void *, const char *, gdb::LineFn, void *) { return gdb::Outcome::None; }
static void t_print(void *, const char *) {}
static const gdb::Target TARGET = {nullptr, t_chip, t_active, t_start,   t_end,  t_read,
                                   t_rreg,  t_wreg, t_flash,  t_command, t_print};

static char commands[256];
static void command(void *, const char *line, gdb::LineFn emit, void *ectx) {
  strncat(commands, line, sizeof commands - strlen(commands) - 2);
  strcat(commands, "|");
  if (strcmp(line, "info") == 0) emit(ectx, "# чип: py32f002b");
}

// ---------------------------------------------------------------- всё, что ушло в USB

static uint8_t usb[32768];
static size_t usb_len;
static void usb_write(void *, const uint8_t *d, size_t n) {
  if (usb_len + n <= sizeof usb) {
    memcpy(usb + usb_len, d, n);
    usb_len += n;
  }
}

static image::Image img;
static gdb::Server server(TARGET, img, usb_write, nullptr);
static const console::Io IO = {nullptr, usb_write, command};
static console::Console *con;
static uint32_t now;

static void send(const char *s) { con->usb((const uint8_t *)s, strlen(s), now); }
static bool usb_has(const char *s) {
  size_t k = strlen(s);
  for (size_t i = 0; i + k <= usb_len; i++)
    if (memcmp(usb + i, s, k) == 0) return true;
  return false;
}
static void send_packet(const char *payload) {
  uint8_t buf[128];
  size_t n = rsp::frame((const uint8_t *)payload, strlen(payload), buf, sizeof buf);
  con->usb(buf, n, now);
}

#include <new>
alignas(console::Console) static unsigned char storage[sizeof(console::Console)];

void setUp(void) {
  con = new (storage) console::Console(server, IO);  // каждый тест — с нового порта
  session = false;
  ends = 0;
  commands[0] = 0;
  usb_len = 0;
  now = 1000;
}
void tearDown(void) {}

static void open_port(void) { con->dtr(true, now); }
static void wait_ms(uint32_t ms) {
  now += ms;
  con->tick(now);
}

// ---------------------------------------------------------------- кто открыл порт

static void test_nothing_is_written_while_port_closed(void) {
  con->line("# итог: ок");
  TEST_ASSERT_EQUAL(0, usb_len);
}

static void test_monitor_gets_output_after_250_ms_of_silence(void) {
  con->line("# готово");
  open_port();
  wait_ms(200);
  TEST_ASSERT_EQUAL(0, usb_len);
  wait_ms(60);
  TEST_ASSERT_TRUE(usb_has("# готово\r\n"));
  TEST_ASSERT_EQUAL(console::Mode::Text, con->mode());
}

static void test_human_byte_frees_output_at_once(void) {
  con->line("# готово");
  open_port();
  send("i");
  TEST_ASSERT_TRUE(usb_has("# готово\r\n"));
  send("nfo\n");
  TEST_ASSERT_EQUAL_STRING("info|", commands);
}

// spec: "gdb-server/Кто открыл порт/Заливка, пока плата пишет лог"
static void test_upload_while_board_logs_is_not_disturbed(void) {
  const char *noise = "temp -5 $12#34 ok\r\n";
  for (int i = 0; i < 100; i++) con->log((const uint8_t *)noise, strlen(noise));
  open_port();
  con->usb(GDB_PIO_UPLOAD, GDB_PIO_UPLOAD_LEN, now);
  TEST_ASSERT_EQUAL(console::Mode::Gdb, con->mode());
  TEST_ASSERT_EQUAL_CHAR('+', usb[0]);  // первым ушло подтверждение gdb, а не лог
  TEST_ASSERT_FALSE(usb_has("temp"));
  TEST_ASSERT_TRUE(usb_has("$OK#9a"));  // ответ на vKill
}

// ---------------------------------------------------------------- текстовый режим

static void test_lone_ack_bytes_are_ignored_and_crlf_is_one_line(void) {
  open_port();
  wait_ms(300);
  send("+-info\r\n");
  TEST_ASSERT_EQUAL_STRING("info|", commands);
}

// spec: "gdb-server/Команды строкой и через monitor/Пустая строка"
static void test_empty_line_is_skipped(void) {
  open_port();
  wait_ms(300);
  send("\n\r\n");
  TEST_ASSERT_EQUAL_STRING("", commands);
}

// spec: "gdb-server/Один порт, два режима/Строка с `$` от человека"
static void test_dollar_line_from_human_stays_text(void) {
  open_port();
  wait_ms(300);
  send("$abc\n");
  TEST_ASSERT_EQUAL(console::Mode::Text, con->mode());
  TEST_ASSERT_EQUAL_STRING("$abc|", commands);
}

static void test_command_output_goes_to_port(void) {
  open_port();
  wait_ms(300);
  send("info\n");
  TEST_ASSERT_TRUE(usb_has("# чип: py32f002b\r\n"));
}

static void test_valid_packet_switches_text_to_gdb(void) {
  open_port();
  wait_ms(300);
  send_packet("?");
  TEST_ASSERT_EQUAL(console::Mode::Gdb, con->mode());
  TEST_ASSERT_TRUE(usb_has("$W00#"));
}

// ---------------------------------------------------------------- режим gdb

// spec: "gdb-server/Один порт, два режима/Человек набрал команду после gdb"
static void test_human_after_gdb_ends_session_and_runs_command(void) {
  open_port();
  send("+");
  send_packet("vAttach;1");
  TEST_ASSERT_TRUE(session);
  send("info\n");
  TEST_ASSERT_FALSE(session);
  TEST_ASSERT_EQUAL(console::Mode::Text, con->mode());
  TEST_ASSERT_EQUAL_STRING("info|", commands);
}

// spec: "target-log/Вывод, пока порт закрыт/Первые строки после заливки"
static void test_lines_after_kill_wait_for_port_and_keep_order(void) {
  open_port();
  send("+");
  send_packet("vAttach;1");
  send_packet("vKill;1");
  con->line("# итог: ок, 120 мс");
  const char *first = "board version 1.0\r\n";  // строка платы сразу после старта
  con->log((const uint8_t *)first, strlen(first));
  TEST_ASSERT_FALSE(usb_has("# итог"));  // gdb ещё держит порт
  con->dtr(false, now);
  open_port();
  wait_ms(300);
  const char *want = "# итог: ок, 120 мс\r\nboard version 1.0\r\n";
  TEST_ASSERT_TRUE(usb_has(want));
}

static void test_port_close_in_gdb_ends_session(void) {
  open_port();
  send("+");
  send_packet("vAttach;1");
  con->dtr(false, now);
  TEST_ASSERT_FALSE(session);
  TEST_ASSERT_EQUAL(console::Mode::Closed, con->mode());
}

static void test_60_s_without_packets_ends_session(void) {
  open_port();
  send("+");
  send_packet("vAttach;1");
  wait_ms(59000);
  TEST_ASSERT_TRUE(session);
  wait_ms(1500);
  TEST_ASSERT_FALSE(session);
  TEST_ASSERT_EQUAL(1, ends);
}

static void test_bad_checksum_gets_nak_before_noack(void) {
  open_port();
  send("+$?#00");
  TEST_ASSERT_TRUE(usb_has("-"));
}

// ---------------------------------------------------------------- буфер вывода

static void test_buffer_keeps_first_4_kb_and_reports_dropped(void) {
  static uint8_t big[5000];
  memset(big, 'a', sizeof big);
  big[sizeof big - 1] = '\n';
  con->log(big, sizeof big);
  open_port();
  wait_ms(300);
  TEST_ASSERT_TRUE(usb_has("# пропущено 904 Б\r\n"));
  TEST_ASSERT_EQUAL(4096 + 2 + strlen("# пропущено 904 Б\r\n"), usb_len);
}

static void test_flasher_line_starts_on_new_line(void) {
  open_port();
  wait_ms(300);
  con->log((const uint8_t *)"abc", 3);
  con->line("# x");
  TEST_ASSERT_TRUE(usb_has("abc\r\n# x\r\n"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_is_written_while_port_closed);
  RUN_TEST(test_monitor_gets_output_after_250_ms_of_silence);
  RUN_TEST(test_human_byte_frees_output_at_once);
  RUN_TEST(test_upload_while_board_logs_is_not_disturbed);
  RUN_TEST(test_lone_ack_bytes_are_ignored_and_crlf_is_one_line);
  RUN_TEST(test_empty_line_is_skipped);
  RUN_TEST(test_dollar_line_from_human_stays_text);
  RUN_TEST(test_command_output_goes_to_port);
  RUN_TEST(test_valid_packet_switches_text_to_gdb);
  RUN_TEST(test_human_after_gdb_ends_session_and_runs_command);
  RUN_TEST(test_lines_after_kill_wait_for_port_and_keep_order);
  RUN_TEST(test_port_close_in_gdb_ends_session);
  RUN_TEST(test_60_s_without_packets_ends_session);
  RUN_TEST(test_bad_checksum_gets_nak_before_noack);
  RUN_TEST(test_buffer_keeps_first_4_kb_and_reports_dropped);
  RUN_TEST(test_flasher_line_starts_on_new_line);
  return UNITY_END();
}
