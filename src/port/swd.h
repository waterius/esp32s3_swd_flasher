// SWD на двух ногах ESP32-S3: биты, такты, чётность, транзакция ADIv5 без повторов (design §3).
// Чипа не знает. Повторы на WAIT, ABORT и память — port/dap.
#pragma once
#include <stdint.h>

namespace swd {

enum class Ack : uint8_t {
  Ok = 1,
  Wait = 2,
  Fault = 4,
  Error = 0,  // нет ответа, неизвестный ACK или ошибка чётности данных
};

// Полупериод SWCLK в тактах ESP (240 МГц): около 1 МГц вместе с накладными расходами.
// Сбоит на проводах — увеличить (design, Risks).
constexpr uint32_t HALF_PERIOD_CYCLES = 100;

void begin();    // ноги в режим сеанса: SWCLK — выход, SWDIO — вход-выход с подтяжкой
void release();  // ноги отпущены: вход без подтяжки
void line_reset();
void jtag_to_swd();
void idle(uint8_t cycles);

// ap — порт доступа или порт отладки; a — адрес регистра 0x0, 0x4, 0x8, 0xC.
Ack transfer(bool ap, bool read, uint8_t a, uint32_t *data);

}  // namespace swd
