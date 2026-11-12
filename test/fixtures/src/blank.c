// Нейтральная прошивка для записи пакетов gdb (tools/gdb_record.py). Ничего не делает:
// таблица векторов, пустой цикл, одна переменная в .data и константа на 5000 Б, чтобы образ
// не помещался в один пакет PacketSize = 0x1000.
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;

volatile uint32_t counter = 0x12345678u;

__attribute__((used)) const uint8_t pattern[5000] = {[0 ... 4999] = 0xA5};

#ifdef WITH_TAIL
// Секция за пределами 24 КБ основной flash: gdb должен отказать сам («Load failed»).
__attribute__((section(".tail"), used)) const uint8_t tail[0x200] = {[0 ... 0x1FF] = 0x5A};
#endif

void reset_handler(void) {
  uint32_t *src = &_sidata;
  for (uint32_t *dst = &_sdata; dst < &_edata;) *dst++ = *src++;
  for (;;) counter += pattern[counter & 0xFFFu];  // у M0+ нет деления
}

__attribute__((section(".isr_vector"), used)) const void *const vectors[48] = {
    &_estack,
    (const void *)reset_handler,
};
