#include "core/rsp.h"

namespace rsp {

namespace {

int hex_value(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

const char HEX[] = "0123456789abcdef";

}  // namespace

uint8_t checksum(const uint8_t *data, size_t len) {
  uint8_t s = 0;
  for (size_t i = 0; i < len; i++) s += data[i];
  return s;
}

void Parser::reset() {
  state_ = State::Idle;
  sum_ = got_ = 0;
  len_ = 0;
}

Event Parser::feed(uint8_t b) {
  switch (state_) {
    case State::Idle:
      if (b == '$') {
        state_ = State::Data;
        sum_ = 0;
        len_ = 0;
        return Event::None;
      }
      if (b == '+') return Event::Ack;
      if (b == '-') return Event::Nak;
      if (b == 0x03) return Event::Interrupt;
      return Event::Other;
    case State::Data:
    case State::Escape:
      if (state_ == State::Data && b == '#') {
        state_ = State::Sum1;
        return Event::None;
      }
      sum_ += b;
      if (state_ == State::Data && b == 0x7d) {
        state_ = State::Escape;
        return Event::None;
      }
      if (len_ >= MAX_PACKET) {
        reset();
        return Event::BadChecksum;
      }
      buf_[len_++] = state_ == State::Escape ? (uint8_t)(b ^ 0x20) : b;
      state_ = State::Data;
      return Event::None;
    case State::Sum1: {
      int v = hex_value(b);
      if (v < 0) {
        reset();
        return Event::BadChecksum;
      }
      got_ = (uint8_t)(v << 4);
      state_ = State::Sum2;
      return Event::None;
    }
    case State::Sum2: {
      int v = hex_value(b);
      bool ok = v >= 0 && (uint8_t)(got_ | v) == sum_;
      state_ = State::Idle;
      return ok ? Event::Packet : Event::BadChecksum;
    }
  }
  return Event::None;
}

size_t frame(const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
  size_t n = 0;
  uint8_t sum = 0;
  if (cap < 4) return 0;
  out[n++] = '$';
  for (size_t i = 0; i < len; i++) {
    uint8_t b = payload[i];
    bool esc = b == '#' || b == '$' || b == '}' || b == '*';
    if (n + (esc ? 2 : 1) + 3 > cap) return 0;
    if (esc) {
      out[n++] = 0x7d;
      sum += 0x7d;
      b ^= 0x20;
    }
    out[n++] = b;
    sum += b;
  }
  if (n + 3 > cap) return 0;
  out[n++] = '#';
  out[n++] = HEX[sum >> 4];
  out[n++] = HEX[sum & 0xF];
  return n;
}

size_t to_hex(const uint8_t *data, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = HEX[data[i] >> 4];
    out[2 * i + 1] = HEX[data[i] & 0xF];
  }
  return 2 * len;
}

bool from_hex(const char *hex, size_t hexlen, uint8_t *out) {
  if (hexlen % 2) return false;
  for (size_t i = 0; i < hexlen / 2; i++) {
    int hi = hex_value((uint8_t)hex[2 * i]), lo = hex_value((uint8_t)hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

bool parse_hex_u32(const char *s, size_t len, uint32_t *value) {
  if (len == 0 || len > 8) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) {
    int d = hex_value((uint8_t)s[i]);
    if (d < 0) return false;
    v = v << 4 | (uint32_t)d;
  }
  *value = v;
  return true;
}

}  // namespace rsp
