# Реестр первоисточников

Факты о чипах, протоколе и USB брать отсюда, а не по памяти; в коде и в `design.md` ссылаться на
документ, раздел и таблицу. Файлы не переименовывать и не править: новую редакцию класть рядом,
в реестре отметить, какая действующая.

## PY32F002B

| Документ | Версия | Файл | Для чего нам |
|---|---|---|---|
| Puya **PY32F002B** Datasheet | V1.0 | `PY32F002B_Datasheet_V1.0.pdf` | ноги SWD и NRST (табл. 3-2), питание и уровни (табл. 5-1, 5-2), HSI 24 МГц (§5.3.7), время записи страницы 1,0/1,5 мс и стирания страницы и всей flash 3,5/5,0 мс (табл. 5-15), 100 000 циклов (табл. 5-16) |
| Puya **PY32F002B-C** Reference Manual | V1.0, 271 с. | `PY32F002B-C_Reference_Manual_V1.0.pdf` | память и USER OTP (§3.4, §4.2.1, §4.5.8, табл. 4-6, 4-7), запись, стирание и полное стирание (§4.2.3), тайминги flash и информационная область (§4.2.3.6, §4.5.2–4.5.6, §4.8.11–4.8.19), защита записи (§4.6), `RCC_ICSCR` (§8.6.2), `RCC_APBENR1.DBGEN` (§8.6.14), отладка и `DBG_*` (§22) |
| Пак Puya `PY32F0xx_DFP`: алгоритм записи PY32F002B | 1.1.7 | `puya_pack_1.1.7/FlashPrg.c` (из `Flash/PY32F002Bxx/`), лицензия zlib (ARM Ltd.) | последовательность записи и стирания регистрами, `EraseChip`, перезарядка IWDG — эталон для `chips/py32f002b_ops` |
| Пак Puya `PY32F0xx_DFP`: описание пака | 1.1.7 | `puya_pack_1.1.7/Puya.PY32F0xx_DFP.pdsc` | `DebugCoreStart` — включение `DBGEN` перед `DBG_*`; какие алгоритмы привязаны к PY32F002Bx5 |

Сам пак (`Puya.PY32F0xx_DFP.1.1.7.pack`, SHA-256 начинается с `138da5cb28641ef3`) в репозиторий не
кладём: нужны только два файла выше.

## Протокол gdb и USB

| Документ | Версия | Где | Для чего нам |
|---|---|---|---|
| GDB, приложение «Remote Protocol» | GDB 15 | <https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html> | пакеты, `vFlash*`, `qXfer:memory-map`, описание регистров `org.gnu.gdb.arm.m-profile`, `QStartNoAckMode`, экранирование |
| Arduino-ESP32, `cores/esp32/USBCDC.cpp` | 3.3.12 (из pioarduino `55.03.312-1`) | `~/.platformio/packages/framework-arduinoespressif32/cores/esp32/USBCDC.cpp` | DTR и RTS, перезагрузка в загрузчик по линиям и скорости 1 200, очередь приёма, неполная запись |

## Эталоны для сверки

Код отсюда не копируем: только сверяем последовательность и числа.

| Документ | Где | Лицензия | Для чего нам |
|---|---|---|---|
| Black Magic Debug, драйвер Puya `src/target/puya.c` | <https://codeberg.org/blackmagic-debug/blackmagic/src/branch/main/src/target/puya.c> | BSD-3-Clause | запись PY32 регистрами, загрузка таймингов из информационной области |
| Black Magic Debug, PR #2000 «puya: add support for PY32F002B» | <https://github.com/blackmagic-debug/blackmagic/pull/2000> | — | тайминги и идентификатор PY32F002B |
| Black Magic Debug, `src/target/cortexm.c` | <https://codeberg.org/blackmagic-debug/blackmagic/src/branch/main/src/target/cortexm.c> | GPL-3.0 — только читать | подключение под сбросом |
