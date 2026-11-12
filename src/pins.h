// Ноги ESP32-S3 Super Mini и что к ним подключено (design §2).
// В сеансе SWD — с резистором 100–330 Ом последовательно, сила выхода наименьшая.
// Вне сеанса SWCLK, SWDIO и NRST отпущены: вход без подтяжки.
#pragma once

#define PIN_SWCLK 1   // SWCLK платы (у PY32F002B — PA2)
#define PIN_SWDIO 2   // SWDIO платы (у PY32F002B — PB6)
#define PIN_NRST 4    // NRST платы (у PY32F002B — PC0), открытый сток
#define PIN_LOG_RX 5  // TX лога платы → приём UART1 оснастки, подтяжка вверх
