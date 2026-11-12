# CLAUDE.md

## Назначение

`esp32s3_swd_flasher` — прошивка **оснастки**: ESP32-S3 Super Mini сама пишет микроконтроллер на
плате по SWD и отвечает gdb как отладчик Black Magic Probe. Через неё шьют из PlatformIO штатной
командой (`upload_protocol = blackmagic`) и со страницы в браузере (`docs/`, change
`web-flash-page`). Поддержан один чип — PY32F002B.

**Оснастка ничего не знает об изделиях** (БТ/swd-flash-py32f002b/2): ни их имён, ни раскладки
памяти, ни форматов данных. Она пишет присланные байты по их адресам. В код, тесты, примеры и
документы этого репозитория сведения о конкретных изделиях не попадают.

План и решения — `openspec/changes/swd-flash-py32f002b/` (после архивации — `openspec/specs/`):
`design.md` объясняет каждое устройство, ссылки вида «design §N» — туда.

## Железо и первоисточники

- Оснастка: ESP32-S3 Super Mini, ноги — `src/pins.h` (design §2). Плата должна питаться от
  3,0–3,6 В.
- Факты о PY32F002B, протоколе gdb и USB брать из `datasheets/README.md`, а не по памяти.

## Команды

`pio` — `~/.platformio/penv/bin/pio`.

```sh
pio run -e esp32s3              # сборка оснастки
pio run -e esp32s3 -t upload    # заливка оснастки по её USB
pio test -e native              # юнит-тесты чистого кода на ПК
pio test -e native -f test_plan # один набор
python3 -m pytest test/spec_coverage -s -q   # покрытие сценариев спек тестами
python3 tools/gdb_record.py --help           # запись пакетов gdb для тестов (design §9)
node --test "test/web/*.test.mjs"            # тесты страницы прошивки (Node 22+)
python3 -m http.server -d docs 8000          # страница на http://localhost:8000/
```

Результат сборки проверять по коду возврата `pio`.

## Архитектура

- `src/core/` — чистый код без Arduino и железа, собирается в `native` и покрыт тестами в
  `test/`: описание чипа (`chip.h`), образ (`image`), план записи (`plan`), порядок записи
  (`write`), кадры протокола gdb (`rsp`), пакеты (`gdb`), режимы порта и вывод (`console`).
- `src/chips/<чип>_desc.cpp` — данные и чистые правила чипа (тоже `native`);
  `src/chips/<чип>_ops.cpp` — действия с чипом через SWD; `src/chips/chips.cpp` — таблица чипов.
- `src/port/` — железо ESP: `swd` (биты), `dap` (порт отладки, память, регистры ядра, NRST),
  `session` (порядок сеанса).
- `src/main.cpp` — USB, DTR, NVS, лог UART1, команды. Единственное место, где всё встречается.
- `docs/` — страница прошивки (change `web-flash-page`), GitHub Pages из `master`, `/docs`. Обычные
  ES-модули без сборки и внешних файлов: `lib/ihex.js`, `lib/image.js`, `lib/rsp.js` — чистый код;
  `lib/flasher.js` — разговор с оснасткой через протокол «порт оснастки»; `lib/webserial.js` — порт
  на Web Serial; `app.js` — экран, все надписи в объекте `T`. Тесты — `test/web/` на поддельной
  оснастке `fake_flasher.mjs`; меняя пакеты или строки `# ` в прошивке, правьте и её. Страница
  делает запросы только за своими файлами — ни CDN, ни шрифтов, ни счётчиков.
- Зависимости: `main` → {`core/console`, `port/session`}; `port/session` → {`core/write`,
  `chips/*_ops`, `port/dap`}; `core/*` от `port/` не зависит (design §12).

## Как добавить чип

Новое описание `chips/<чип>_desc.cpp` (области, идентификатор, правило «решить про единицу»,
тайминги), действия `chips/<чип>_ops.cpp`, строка в `chips/chips.cpp` — и ничего больше
(design §6). Способ записи — регистрами по `FlashPrg.c` пака или FLM — выбирается в change'е
чипа.

## Тесты и спеки

Тест Unity, который закрывает сценарий спеки, помечается комментарием над функцией:

```cpp
// spec: "swd-flash/USER OTP PY32F002B не запирается/Образ с замком"
static void test_lock_in_image_is_refused(void) { ... }
```

Путь — дословно `<capability>/<Requirement>/<Scenario>`. В тестах `node:test` — так же, комментарием
над `test(...)`. Сценарии «проверяется на плате» — в `MANUAL` файла
`test/spec_coverage/spec_coverage_data.py` с протоколом `dev/board-checks.md` (оснастка) или
`dev/board-checks-web.md` (страница).

## Работа с git

Правила обязательны: [`dev/git-rules.md`](dev/git-rules.md). Главное: не коммитить в `master`,
только rebase, PR только в `master`, PR мерджит только человек, `git stash` не использовать,
`.env` не читать, файлы добавлять в индекс явно.

## Числа

В выводе оснастки, документах и коде шестнадцатеричное число — всегда с `0x`.
