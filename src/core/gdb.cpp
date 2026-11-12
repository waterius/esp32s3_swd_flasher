#include "core/gdb.h"

#include <stdio.h>
#include <string.h>

#include "core/rsp.h"

namespace gdb {

namespace {

const char SUPPORTED[] = "PacketSize=1000;qXfer:memory-map:read+;qXfer:features:read+;QStartNoAckMode+";

const char TARGET_XML[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\"><target>"
    "<architecture>arm</architecture><feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\"/><reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/><reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/>"
    "<reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/><reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/><reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/><reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\" regnum=\"25\"/></feature></target>";

constexpr size_t MAX_READ = 0x800;  // байтов на один m: ответ в hex — 0x1000 символов
char out[2 * MAX_READ + 64];
uint8_t framed[2 * MAX_READ + 128];
uint8_t mem[MAX_READ];
char text[1024];  // карта памяти и строки команд

bool starts(const uint8_t *d, size_t n, const char *prefix) {
  size_t k = strlen(prefix);
  return n >= k && memcmp(d, prefix, k) == 0;
}

bool equals(const uint8_t *d, size_t n, const char *s) { return n == strlen(s) && memcmp(d, s, n) == 0; }

// «addr,len» в hex
bool parse_pair(const char *s, size_t n, uint32_t *a, uint32_t *b) {
  const char *comma = static_cast<const char *>(memchr(s, ',', n));
  if (!comma) return false;
  return rsp::parse_hex_u32(s, comma - s, a) && rsp::parse_hex_u32(comma + 1, n - (comma - s) - 1, b);
}

// regnum gdb → номер регистра ядра: 0–15 как есть, xpsr — regnum 25 (target.xml)
int core_reg(uint32_t regnum) {
  if (regnum <= 15) return (int)regnum;
  if (regnum == 25) return 16;
  return -1;
}

void hex_le(uint32_t v, char *dst) {
  const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  rsp::to_hex(b, 4, dst);
}

size_t memory_map(const chip::Desc *d, char *buf, size_t size) {
  int n = snprintf(buf, size,
                   "<?xml version=\"1.0\"?><!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory "
                   "Map V1.0//EN\" \"http://sourceware.org/gdb/gdb-memory-map.dtd\"><memory-map>");
  for (uint8_t i = 0; d && i < d->region_count && n > 0 && (size_t)n < size; i++) {
    const chip::Region &r = d->regions[i];
    if (r.kind == chip::Kind::Flash && r.unit)
      n += snprintf(buf + n, size - n,
                    "<memory type=\"flash\" start=\"0x%lx\" length=\"0x%lx\"><property "
                    "name=\"blocksize\">0x%lx</property></memory>",
                    (unsigned long)r.start, (unsigned long)r.size, (unsigned long)r.unit);
    else
      n += snprintf(buf + n, size - n, "<memory type=\"ram\" start=\"0x%lx\" length=\"0x%lx\"/>",
                    (unsigned long)r.start, (unsigned long)r.size);
  }
  if (n > 0 && (size_t)n < size) n += snprintf(buf + n, size - n, "</memory-map>");
  return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

}  // namespace

Server::Server(const Target &target, image::Image &img, SendFn send, void *send_ctx)
    : t_(target), img_(img), send_(send), send_ctx_(send_ctx) {}

void Server::reset_connection() { noack_ = false; }

void Server::send_raw(const char *s, size_t n) { send_(send_ctx_, reinterpret_cast<const uint8_t *>(s), n); }

void Server::reply(const char *s, size_t n) {
  size_t k = rsp::frame(reinterpret_cast<const uint8_t *>(s), n, framed, sizeof framed);
  if (k) send_(send_ctx_, framed, k);
}

void Server::reply(const char *s) { reply(s, strlen(s)); }

void Server::reply_xfer(const char *doc, size_t doc_len, const char *args, size_t args_len) {
  uint32_t off, len;
  if (!parse_pair(args, args_len, &off, &len)) return reply("E01");
  if (off >= doc_len) return reply("l");
  size_t chunk = doc_len - off;
  if (chunk > len) chunk = len;
  if (chunk > sizeof out - 2) chunk = sizeof out - 2;
  out[0] = off + chunk < doc_len ? 'm' : 'l';
  memcpy(out + 1, doc + off, chunk);
  reply(out, chunk + 1);
}

void Server::end_session() {
  if (t_.session_active(t_.ctx)) t_.end_session(t_.ctx);
  write_ok_ = write_failed_ = image_open_ = false;
}

void Server::on_leave() { end_session(); }

void Server::on_bad_packet() {
  if (!noack_) send_raw("-", 1);
}

void Server::emit_console(void *self, const char *line) {
  Server *s = static_cast<Server *>(self);
  size_t n = strlen(line);
  if (n > (sizeof out - 4) / 2) n = (sizeof out - 4) / 2;
  out[0] = 'O';
  size_t k = 1 + rsp::to_hex(reinterpret_cast<const uint8_t *>(line), n, out + 1);
  k += rsp::to_hex(reinterpret_cast<const uint8_t *>("\n"), 1, out + k);
  s->reply(out, k);
}

bool Server::on_packet(const uint8_t *d, size_t n) {
  if (!noack_) send_raw("+", 1);
  const char *s = reinterpret_cast<const char *>(d);
  bool session = t_.session_active(t_.ctx);

  if (starts(d, n, "qSupported")) {
    reply(SUPPORTED);
  } else if (equals(d, n, "QStartNoAckMode")) {
    reply("OK");
    noack_ = true;
  } else if (equals(d, n, "!") || starts(d, n, "H") || starts(d, n, "qSymbol") || starts(d, n, "T")) {
    reply("OK");
  } else if (equals(d, n, "?")) {
    reply(session ? "T05" : "W00");
  } else if (starts(d, n, "qRcmd,")) {
    size_t hexlen = n - 6;
    char cmd[129];
    if (hexlen / 2 >= sizeof cmd || !rsp::from_hex(s + 6, hexlen, reinterpret_cast<uint8_t *>(cmd)))
      return reply("E01"), false;
    cmd[hexlen / 2] = 0;
    t_.command(t_.ctx, cmd, emit_console, this);
    reply("OK");
  } else if (starts(d, n, "vAttach;")) {
    if (!session) session = t_.start_session(t_.ctx, nullptr, nullptr);
    reply(session ? "T05" : "E01");
  } else if (equals(d, n, "qC")) {
    reply("QC1");
  } else if (equals(d, n, "qfThreadInfo")) {
    reply("m1");
  } else if (equals(d, n, "qsThreadInfo")) {
    reply("l");
  } else if (equals(d, n, "qAttached")) {
    reply("1");
  } else if (starts(d, n, "qXfer:features:read:target.xml:")) {
    const size_t k = strlen("qXfer:features:read:target.xml:");
    reply_xfer(TARGET_XML, sizeof TARGET_XML - 1, s + k, n - k);
  } else if (starts(d, n, "qXfer:memory-map:read::")) {
    const size_t k = strlen("qXfer:memory-map:read::");
    size_t len = memory_map(t_.chip(t_.ctx), text, sizeof text);
    reply_xfer(text, len, s + k, n - k);
  } else if (equals(d, n, "g")) {
    if (!session) return reply("E01"), false;
    for (uint8_t i = 0; i < 17; i++) {
      uint32_t v;
      if (!t_.read_reg(t_.ctx, i, &v)) return reply("E01"), false;
      hex_le(v, out + 8 * i);
    }
    reply(out, 17 * 8);
  } else if (starts(d, n, "p")) {
    uint32_t regnum, v;
    int idx = rsp::parse_hex_u32(s + 1, n - 1, &regnum) ? core_reg(regnum) : -1;
    if (!session || idx < 0 || !t_.read_reg(t_.ctx, (uint8_t)idx, &v)) return reply("E01"), false;
    hex_le(v, out);
    reply(out, 8);
  } else if (starts(d, n, "P")) {
    const char *eq = static_cast<const char *>(memchr(s, '=', n));
    uint32_t regnum;
    uint8_t b[4];
    int idx = eq && rsp::parse_hex_u32(s + 1, eq - s - 1, &regnum) ? core_reg(regnum) : -1;
    if (!session || idx < 0 || n - (eq - s) - 1 != 8 || !rsp::from_hex(eq + 1, 8, b)) return reply("E01"), false;
    uint32_t v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    reply(t_.write_reg(t_.ctx, (uint8_t)idx, v) ? "OK" : "E01");
  } else if (starts(d, n, "m")) {
    uint32_t addr, len;
    if (!session || !parse_pair(s + 1, n - 1, &addr, &len)) return reply("E01"), false;
    if (len > MAX_READ) len = MAX_READ;
    if (!t_.read_mem(t_.ctx, addr, mem, len)) return reply("E01"), false;
    reply(out, rsp::to_hex(mem, len, out));
  } else if (starts(d, n, "vFlashErase:")) {
    uint32_t addr, len;
    if (!session || !parse_pair(s + 12, n - 12, &addr, &len)) {
      write_failed_ = true;
      return reply("E01"), false;
    }
    if (!image_open_ || img_.failed()) {
      const chip::Desc *c = t_.chip(t_.ctx);
      if (!c) return write_failed_ = true, reply("E01"), false;
      img_.reset(*c);
      image_open_ = true;
    }
    if (!img_.erase(addr, len)) return write_failed_ = true, reply("E01"), false;
    reply("OK");
  } else if (starts(d, n, "vFlashWrite:")) {
    const char *colon = static_cast<const char *>(memchr(s + 12, ':', n - 12));
    uint32_t addr;
    if (!session || !image_open_ || !colon || !rsp::parse_hex_u32(s + 12, colon - s - 12, &addr)) {
      write_failed_ = true;
      return reply("E01"), false;
    }
    const uint8_t *data = reinterpret_cast<const uint8_t *>(colon + 1);
    if (!img_.write(addr, data, (uint32_t)(n - (colon + 1 - s)))) return write_failed_ = true, reply("E01"), false;
    reply("OK");
  } else if (equals(d, n, "vFlashDone")) {
    if (!session || !image_open_ || img_.failed()) {
      image_open_ = false;
      write_failed_ = true;
      return reply("E01"), false;
    }
    image_open_ = false;
    bool ok = t_.flash(t_.ctx, img_);
    if (ok) write_ok_ = true;
    else write_failed_ = true;
    reply(ok ? "OK" : "E01");
  } else if (starts(d, n, "vKill")) {
    bool ok = write_ok_ && !write_failed_;
    end_session();
    reply(ok ? "OK" : "E01");
    return true;
  } else if (equals(d, n, "D") || starts(d, n, "D;")) {
    end_session();
    reply("OK");
    return true;
  } else if (equals(d, n, "c") || equals(d, n, "s") || starts(d, n, "C") || starts(d, n, "S") ||
             starts(d, n, "vCont;") || starts(d, n, "Z") || starts(d, n, "z") || starts(d, n, "M") ||
             starts(d, n, "X")) {
    reply("E01");  // отладки нет: ядро стоит до конца сеанса
  } else {
    reply("");  // не поддержано
  }
  return false;
}

}  // namespace gdb
