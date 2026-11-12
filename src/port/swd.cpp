// Последовательность битов — как в CMSIS-DAP SW_DP.c: SWCLK в покое высокий, хост выставляет
// SWDIO до спада и читает его перед фронтом; такт перехода (turnaround) — один.
#include "port/swd.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_cpu.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

#include "pins.h"

namespace swd {

namespace {

constexpr uint32_t CLK = 1u << PIN_SWCLK;
constexpr uint32_t DIO = 1u << PIN_SWDIO;
static_assert(PIN_SWCLK < 32 && PIN_SWDIO < 32, "ноги SWD должны быть в банке GPIO0–31");

inline void half() {
  uint32_t start = esp_cpu_get_cycle_count();
  while (esp_cpu_get_cycle_count() - start < HALF_PERIOD_CYCLES) {
  }
}
inline void clk_lo() { REG_WRITE(GPIO_OUT_W1TC_REG, CLK); }
inline void clk_hi() { REG_WRITE(GPIO_OUT_W1TS_REG, CLK); }
inline void dio_out(bool b) { REG_WRITE(b ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, DIO); }
inline bool dio_in() { return (REG_READ(GPIO_IN_REG) & DIO) != 0; }
inline void dio_drive(bool on) { REG_WRITE(on ? GPIO_ENABLE_W1TS_REG : GPIO_ENABLE_W1TC_REG, DIO); }

inline void cycle() {
  clk_lo();
  half();
  clk_hi();
  half();
}
inline void write_bit(bool b) {
  dio_out(b);
  clk_lo();
  half();
  clk_hi();
  half();
}
inline bool read_bit() {
  clk_lo();
  half();
  bool b = dio_in();
  clk_hi();
  half();
  return b;
}

void write_bits(uint32_t v, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) write_bit((v >> i) & 1);
}
uint32_t read_bits(uint8_t n) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < n; i++) v |= (uint32_t)read_bit() << i;
  return v;
}

}  // namespace

void begin() {
  clk_hi();
  dio_out(true);
  pinMode(PIN_SWCLK, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)PIN_SWCLK, GPIO_DRIVE_CAP_0);
  pinMode(PIN_SWDIO, INPUT_PULLUP);
  gpio_set_direction((gpio_num_t)PIN_SWDIO, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_drive_capability((gpio_num_t)PIN_SWDIO, GPIO_DRIVE_CAP_0);
  dio_drive(true);
}

void release() {
  pinMode(PIN_SWCLK, INPUT);  // INPUT в Arduino-ESP32 — без подтяжки
  pinMode(PIN_SWDIO, INPUT);
}

void line_reset() {
  dio_drive(true);
  dio_out(true);
  for (int i = 0; i < 56; i++) cycle();  // не меньше 50 единиц
}

void jtag_to_swd() {
  dio_drive(true);
  write_bits(0xE79E, 16);
}

void idle(uint8_t cycles) {
  dio_drive(true);
  dio_out(false);
  for (uint8_t i = 0; i < cycles; i++) cycle();
}

Ack transfer(bool ap, bool read, uint8_t a, uint32_t *data) {
  const bool a2 = a & 0x4, a3 = a & 0x8;
  const bool parity = ap ^ read ^ a2 ^ a3;
  const uint8_t request = 0x81 | ap << 1 | read << 2 | a2 << 3 | a3 << 4 | parity << 5;

  dio_drive(true);
  write_bits(request, 8);
  dio_drive(false);
  cycle();  // переход к цели
  const uint32_t ack = read_bits(3);

  if (ack == (uint32_t)Ack::Ok) {
    if (read) {
      uint32_t v = read_bits(32);
      bool p = read_bit();
      cycle();  // переход к хосту
      dio_drive(true);
      idle(8);
      if ((bool)__builtin_parity(v) != p) return Ack::Error;
      *data = v;
    } else {
      cycle();
      dio_drive(true);
      write_bits(*data, 32);
      write_bit(__builtin_parity(*data));
      idle(8);
    }
    return Ack::Ok;
  }
  if (ack == (uint32_t)Ack::Wait || ack == (uint32_t)Ack::Fault) {
    cycle();
    dio_drive(true);
    idle(8);
    return (Ack)ack;
  }
  // нет ответа или ошибка протокола: пропустить возможную фазу данных
  for (int i = 0; i < 33; i++) cycle();
  dio_drive(true);
  idle(8);
  return Ack::Error;
}

}  // namespace swd
