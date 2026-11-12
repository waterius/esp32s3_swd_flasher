// Порт отладки ADIv5 поверх port/swd: вход в SWD, память через AHB-AP, регистры ядра
// Cortex-M, NRST (design §3, §4). Чипа не знает: адреса регистров чипа — в chips/*_ops.
#pragma once
#include <stdint.h>

namespace dap {

// Регистры отладки Cortex-M (ARMv6-M).
constexpr uint32_t DHCSR = 0xE000EDF0;
constexpr uint32_t DCRSR = 0xE000EDF4;
constexpr uint32_t DCRDR = 0xE000EDF8;
constexpr uint32_t DEMCR = 0xE000EDFC;
constexpr uint32_t AIRCR = 0xE000ED0C;
constexpr uint32_t DHCSR_S_REGRDY = 1u << 16;
constexpr uint32_t DHCSR_S_HALT = 1u << 17;
constexpr uint32_t DHCSR_S_RESET_ST = 1u << 25;

// Вход в SWD: line reset, 0xE79E, line reset, IDCODE, снятие ошибок, включение отладки,
// AHB-AP со словами и автоинкрементом. false — чип не ответил.
bool connect(uint32_t *idcode);
// Сброс запросов питания отладки (CTRL/STAT = 0) — в конце сеанса.
bool power_down();

bool read32(uint32_t addr, uint32_t *value);
bool write32(uint32_t addr, uint32_t value);
bool read_block(uint32_t addr, uint32_t *words, uint32_t count);
bool write_block(uint32_t addr, const uint32_t *words, uint32_t count);
// Байты с любого адреса и любой длины: читает покрывающие их слова кусками по 0x800 Б.
bool read_bytes(uint32_t addr, uint8_t *buf, uint32_t len);

// Регистры ядра: 0–12 r0–r12, 13 sp, 14 lr, 15 pc, 16 xpsr — номера DCRSR.
bool read_core_reg(uint8_t index, uint32_t *value);
bool write_core_reg(uint8_t index, uint32_t value);

// S_RESET_ST сбрасывается любым чтением DHCSR, поэтому каждое чтение копит его здесь.
bool reset_seen();
void clear_reset_seen();

// NRST: true — держать в 0 (открытый сток), false — отпустить на подтяжку платы.
void nrst(bool low);
// SWCLK, SWDIO и NRST — вход без подтяжки.
void release();

// Короткая причина последней ошибки: «WAIT дольше 10 мс», «FAULT», «нет ответа SWD», «STICKYERR».
const char *last_error();

}  // namespace dap
