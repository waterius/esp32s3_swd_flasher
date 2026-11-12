// Разметка страницы: порядок блоков, спонсор, галочка проверки, очистка памяти
// (jaffa-flasher-page, спека web-flash). DOM не нужен: проверяется текст index.html и app.js.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const html = readFileSync(new URL('../../docs/index.html', import.meta.url), 'utf8');
const app = readFileSync(new URL('../../docs/app.js', import.meta.url), 'utf8');
const at = (needle) => {
  const i = html.indexOf(needle);
  assert.ok(i >= 0, `нет в index.html: ${needle}`);
  return i;
};
const text = (key) => {
  const m = new RegExp(`\\n  ${key}: '([^']*)'`).exec(app);
  assert.ok(m, `нет надписи ${key}`);
  return m[1];
};

// spec: "web-flash/Страница ведёт от сборки до прошивки/Порядок блоков"
test('заголовок, затем блоки и шаги 1–4 по порядку', () => {
  assert.equal(text('title'), 'Прошивка через Jaffa Flasher');
  assert.equal(text('lead'), 'Программатор Jaffa Flasher на МК ESP32-S3 для прошивки МК PY32.');
  const order = ['data-t="title"', 'id="sec-make"', 'id="sec-wiring"', 'id="sec-flash"', 'data-t="step1"',
    'data-t="step2"', 'data-t="step3"', 'data-t="step4"', 'id="sec-memory"', 'id="sec-log"'].map(at);
  assert.deepEqual(order, [...order].sort((a, b) => a - b));
  assert.equal(text('secMake'), 'Как сделать программатор');
  assert.equal(text('secWiring'), 'Подключение');
  assert.equal(text('secLog'), 'Терминал');
  assert.equal(text('step1'), 'Подключите Jaffa Flasher к USB');
  assert.equal(text('step4'), 'Нажмите «Прошить»');
  // без запросов к чужим адресам: картинки — свои файлы
  for (const m of html.matchAll(/<(?:img|script|link)[^>]*(?:src|href)="([^"]+)"/g)) {
    assert.ok(!/^https?:/.test(m[1]), `чужой адрес: ${m[1]}`);
  }
});

// spec: "web-flash/Спонсор вверху и в подвале/Ссылка спонсора"
test('спонсор в шапке и подвале — waterius.ru с метками', () => {
  assert.match(app, /const SPONSOR_URL =\s*'https:\/\/waterius\.ru\/\?utm_source=jaffa_flasher&utm_medium=web&utm_campaign=jaffa_flasher'/);
  assert.match(app, /for \(const id of \['sponsor-top', 'sponsor-foot'\]\) \$\(id\)\.href = SPONSOR_URL;/);
  assert.ok(at('id="sponsor-top"') < at('data-t="title"'));
  assert.ok(at('id="sponsor-foot"') > at('<footer'));
  assert.equal(text('sponsoredBy'), 'Sponsored by');
});

// spec: "web-flash/Проверка после записи видна/Галочку не снять"
test('галочка проверки отмечена и недоступна', () => {
  assert.match(html, /<input id="verify" type="checkbox" checked disabled \/>/);
  assert.ok(text('verify').startsWith('Проверить прошивку после записи'));
});

// spec: "web-flash/Очистка экрана памяти/Очистить дамп"
test('«Очистить экран» в памяти убирает дамп', () => {
  assert.ok(at('id="sec-memory"') < at('id="dump-clear"') && at('id="dump-clear"') < at('id="sec-log"'));
  assert.match(app, /el\.dumpClear\.addEventListener\('click', \(\) => \{\s*el\.dump\.textContent = '';\s*el\.dump\.hidden = true;/);
  assert.equal(text('clearScreen'), 'Очистить экран');
});
