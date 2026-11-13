## Why

В терминале страницы строки лога платы идут без времени, а часов у платы нет: не видно, когда
было событие и сколько прошло между строками (business.md, БТ/log-timestamps/1).

## What Changes

- Каждая строка терминала начинается с метки времени ПК: `ГГГГ-ММ-ДД чч-мм-сс.ммм: `.

## Capabilities

### New Capabilities

### Modified Capabilities
- `web-flash`: строки терминала с меткой времени.

## Impact

- `docs/lib/terminal.js` (`stamp`), `docs/app.js` (вывод строки), `test/web/terminal.test.mjs`.
- Программатор не меняется.
