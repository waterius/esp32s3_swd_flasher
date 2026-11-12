// Терминал и метки файлов без DOM (jaffa-flasher-page design §5, §7–§9).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { baudCommand, terminalControls, LogView, DEFAULT_UART } from '../../docs/lib/terminal.js';
import { fileTags, parseFile } from '../../docs/lib/image.js';
import { hexFile, fill } from './ihex_util.mjs';

const REGIONS = [
  { start: 0x08000000, end: 0x08006000, name: 'flash', main: true },
  { start: 0x1fff0280, end: 0x1fff0300, name: 'USER OTP', main: false },
];
const file = (name, addr, len) => ({ name, parsed: parseFile(name, hexFile(addr, fill(len, 0x11))) });

// spec: "web-flash/Терминал/Формат лога со страницы"
test('выбор 9 600, 8 бит, чёт, 1 стоп — команда baud 9600 8E1', () => {
  assert.equal(baudCommand({ speed: 9600, bits: 8, parity: 'E', stop: 1 }), 'baud 9600 8E1');
  assert.equal(baudCommand(DEFAULT_UART), 'baud 115200 8N1');
});

// spec: "web-flash/Терминал/Во время прошивки"
test('во время сеанса настройки, кнопка лога и команда недоступны, очистка — да', () => {
  assert.deepEqual(terminalControls({ connected: true, busy: false, working: true }), {
    uart: false,
    logToggle: false,
    command: false,
    clear: true,
  });
  assert.equal(terminalControls({ connected: false, busy: false, working: false }).uart, false);
  assert.equal(terminalControls({ connected: true, busy: false, working: false }).uart, true);
});

// spec: "web-flash/Терминал/Лог выключен"
test('выключенный лог не выводит строки и не копит их', () => {
  const shown = [];
  const view = new LogView((l) => shown.push(l));
  assert.equal(view.toggle(), false);
  view.show('плата: 1');
  assert.deepEqual(shown, []);
  assert.equal(view.toggle(), true);
  view.show('плата: 2');
  assert.deepEqual(shown, ['плата: 2']);
});

// spec: "web-flash/Метки файлов/Файл USER OTP"
test('файл в USER OTP получает метку области', () => {
  const tags = fileTags([file('fw.hex', 0x08000000, 256), file('otp.hex', 0x1fff0280, 128)], REGIONS);
  assert.deepEqual(tags, [[], [{ kind: 'region', text: 'USER OTP' }]]);
});

test('без областей (программатор не подключён) меток областей нет', () => {
  assert.deepEqual(fileTags([file('otp.hex', 0x1fff0280, 128)]), [[]]);
});

// spec: "web-flash/Метки файлов/Два варианта прошивки"
test('второй вариант прошивки — «пересекается с» только у нижнего файла', () => {
  const tags = fileTags([file('a.hex', 0x08000000, 0x4d58), file('b.hex', 0x08000000, 0x4114)], REGIONS);
  assert.deepEqual(tags, [[], [{ kind: 'overlap', text: 'пересекается с a.hex' }]]);
});

test('соседние файлы без общих адресов не пересекаются; файл с ошибкой без меток', () => {
  const tags = fileTags(
    [file('a.hex', 0x08000000, 0x100), file('b.hex', 0x08000100, 0x100), { name: 'bad.hex', error: 'x' }],
    REGIONS,
  );
  assert.deepEqual(tags, [[], [], []]);
});
