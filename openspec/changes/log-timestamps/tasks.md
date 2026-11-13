## 1. Страница

- [x] 1.1 `stamp(date)` в `docs/lib/terminal.js`, метка в `logLine` (`docs/app.js`), тест
  `test/web/terminal.test.mjs`.
- [x] 1.2 «Пауза» / «Продолжить»: `LogView.pause`, `follow`; кнопка `log-pause`; тест.

## 2. Проверки

- [x] 2.1 `node --test "test/web/*.test.mjs"`, `pytest test/spec_coverage` — зелёные.
- [ ] 2.2 На плате: строки терминала с меткой времени, пробуждения кнопкой видны с интервалами;
  «Пауза» — экран стоит, строки приходят; «Продолжить» — к последней строке.

## 3. После мержа

- [ ] 3.1 БТ и термин — в `openspec/business/`; `/opsx:archive log-timestamps`.
