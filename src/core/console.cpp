#include "core/console.h"

#include <stdio.h>
#include <string.h>

namespace console {

Console::Console(gdb::Server &server, const Io &io) : server_(server), io_(io) {}

void Console::dtr(bool high, uint32_t now) {
  if (high && mode_ == Mode::Closed) {
    mode_ = Mode::Unknown;
    opened_ms_ = now;
    parser_.reset();
    candidate_ = false;
    line_len_ = 0;
    line_overflow_ = false;
  } else if (!high && mode_ != Mode::Closed) {
    if (mode_ == Mode::Gdb) server_.on_leave();
    mode_ = Mode::Closed;
  }
}

void Console::tick(uint32_t now) {
  if (mode_ == Mode::Unknown && now - opened_ms_ >= MONITOR_DELAY_MS) to_text();
  if (mode_ == Mode::Gdb && !idle_fired_ && now - last_packet_ms_ >= GDB_IDLE_MS) {
    idle_fired_ = true;
    server_.on_leave();  // брошенный сеанс: плата запускается, порт остаётся занят до DTR или человека
  }
}

void Console::usb(const uint8_t *data, size_t len, uint32_t now) {
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    switch (mode_) {
      case Mode::Closed:
        break;  // порт закрыт: байтов быть не может
      case Mode::Unknown:
        if (b == '+' || b == '$') {
          enter_gdb(now);
          byte_gdb(b, now);
        } else {
          to_text();
          byte_text(b, now);
        }
        break;
      case Mode::Text:
        byte_text(b, now);
        break;
      case Mode::Gdb:
        byte_gdb(b, now);
        break;
    }
  }
}

void Console::enter_gdb(uint32_t now) {
  // разборщик не сбрасываем: из текстового режима сюда приходят с только что разобранным
  // пакетом, а из Unknown — с пустым разборщиком
  mode_ = Mode::Gdb;
  server_.reset_connection();
  last_packet_ms_ = now;
  idle_fired_ = false;
}

void Console::to_text() {
  mode_ = Mode::Text;
  parser_.reset();
  candidate_ = false;
  line_len_ = 0;
  line_overflow_ = false;
  flush();
}

void Console::byte_gdb(uint8_t b, uint32_t now) {
  switch (parser_.feed(b)) {
    case rsp::Event::Packet:
      last_packet_ms_ = now;
      idle_fired_ = false;
      server_.on_packet(parser_.payload(), parser_.length());  // после vKill и D вывод ждёт
      break;
    case rsp::Event::BadChecksum:
      server_.on_bad_packet();
      break;
    case rsp::Event::Other:  // байт вне пакета — человек
      server_.on_leave();
      to_text();
      byte_text(b, now);
      break;
    default:
      break;
  }
}

void Console::append(uint8_t b) {
  if (line_len_ < LINE_LEN - 1) line_[line_len_++] = (char)b;
  else line_overflow_ = true;
}

void Console::exec_line() {
  line_[line_len_] = 0;
  bool run = line_len_ && !line_overflow_;
  line_len_ = 0;
  line_overflow_ = false;
  if (run) io_.command(io_.ctx, line_, emit, this);
}

void Console::byte_text(uint8_t b, uint32_t now) {
  if (candidate_) {
    if (b == '\r' || b == '\n') {
      candidate_ = false;
      parser_.reset();
      exec_line();
      return;
    }
    append(b);
    rsp::Event e = parser_.feed(b);
    if (e == rsp::Event::Packet) {
      candidate_ = false;
      line_len_ = 0;
      enter_gdb(now);
      server_.on_packet(parser_.payload(), parser_.length());
    } else if (e == rsp::Event::BadChecksum) {
      candidate_ = false;  // не пакет — обычная строка, ждём её конца
    }
    return;
  }
  if (b == '\r' || b == '\n') return exec_line();
  if (line_len_ == 0 && (b == '+' || b == '-')) return;  // подтверждения вне строки
  if (line_len_ == 0 && b == '$') {
    candidate_ = true;
    parser_.reset();
    parser_.feed(b);
  }
  append(b);
}

void Console::put(const uint8_t *data, size_t len) {
  if (!len) return;
  if (mode_ == Mode::Text) {
    io_.write(io_.ctx, data, len);
    at_line_start_ = data[len - 1] == '\n';
    return;
  }
  size_t room = OUT_BUF - out_len_;
  size_t k = len < room ? len : room;
  memcpy(out_ + out_len_, data, k);
  out_len_ += k;
  dropped_ += (uint32_t)(len - k);
  if (k) at_line_start_ = data[k - 1] == '\n';
}

void Console::flush() {
  if (out_len_) io_.write(io_.ctx, out_, out_len_);
  out_len_ = 0;
  if (dropped_) {
    char msg[48];
    snprintf(msg, sizeof msg, "# пропущено %lu Б", (unsigned long)dropped_);
    dropped_ = 0;
    line(msg);
  }
}

void Console::line(const char *text) {
  if (!at_line_start_) put(reinterpret_cast<const uint8_t *>("\r\n"), 2);
  put(reinterpret_cast<const uint8_t *>(text), strlen(text));
  put(reinterpret_cast<const uint8_t *>("\r\n"), 2);
}

void Console::log(const uint8_t *data, size_t len) { put(data, len); }

void Console::emit(void *self, const char *text) { static_cast<Console *>(self)->line(text); }

}  // namespace console
