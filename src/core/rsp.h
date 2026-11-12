// Кадры протокола gdb (GDB Remote Serial Protocol): $данные#xx, экранирование } , hex.
// Смысла пакетов не знает — это core/gdb (design §9).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rsp {

// Наибольший пакет, который принимаем: PacketSize = 0x1000 плюс запас на экранирование.
constexpr size_t MAX_PACKET = 0x1000 + 64;

uint8_t checksum(const uint8_t *data, size_t len);

enum class Event : uint8_t {
  None,         // байт принят, событие ещё не готово
  Packet,       // пакет с верной суммой: payload(), length()
  BadChecksum,  // пакет с неверной суммой или длиннее MAX_PACKET
  Ack,          // + вне пакета
  Nak,          // - вне пакета
  Interrupt,    // Ctrl-C (0x03) вне пакета
  Other,        // любой другой байт вне пакета
};

// Разбор входящих байтов по одному.
class Parser {
 public:
  Event feed(uint8_t byte);
  void reset();
  bool in_packet() const { return state_ != State::Idle; }
  const uint8_t *payload() const { return buf_; }
  size_t length() const { return len_; }

 private:
  enum class State : uint8_t { Idle, Data, Escape, Sum1, Sum2 };
  State state_ = State::Idle;
  uint8_t sum_ = 0;  // сумма байтов как пришли, до снятия экранирования
  uint8_t got_ = 0;  // принятая сумма
  size_t len_ = 0;
  uint8_t buf_[MAX_PACKET];
};

// $payload#xx с экранированием #, $, }, *. Возвращает длину или 0, если не влезло в cap.
size_t frame(const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

// Байты → hex строчными, возвращает число символов (2 × len).
size_t to_hex(const uint8_t *data, size_t len, char *out);
// hex (любой регистр, чётная длина) → байты; false — не hex.
bool from_hex(const char *hex, size_t hexlen, uint8_t *out);
// Число в hex без префикса, до 8 цифр; false — пусто, длиннее или не hex.
bool parse_hex_u32(const char *s, size_t len, uint32_t *value);

}  // namespace rsp
