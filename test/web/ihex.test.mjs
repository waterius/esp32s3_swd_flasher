// Intel HEX: типы записей, ошибки с номером строки, конец файла (design §3).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseIhex } from '../../docs/lib/ihex.js';
import { rec, hexFile } from './ihex_util.mjs';

test('данные с базой 04 — абсолютные адреса', () => {
  const chunks = parseIhex(hexFile(0x08000000, [1, 2, 3]));
  assert.equal(chunks.length, 1);
  assert.equal(chunks[0].addr, 0x08000000);
  assert.deepEqual([...chunks[0].bytes], [1, 2, 3]);
});

test('база 02 — сегмент × 16', () => {
  const text = [rec(0x02, 0, [0x10, 0x00]), rec(0x00, 0x0010, [0xaa]), rec(0x01, 0)].join('\n');
  assert.equal(parseIhex(text)[0].addr, 0x10010);
});

test('USER OTP по 0x1FFF0280', () => {
  const chunks = parseIhex(hexFile(0x1fff0280, new Array(128).fill(0x11)));
  assert.equal(chunks[0].addr, 0x1fff0280);
  assert.equal(chunks.reduce((n, c) => n + c.bytes.length, 0), 128);
});

test('03 и 05 пропускаются, пустые строки тоже', () => {
  const text = ['', rec(0x03, 0, [0, 0, 0, 0]), rec(0x05, 0, [0x08, 0, 0, 0xc1]), rec(0x00, 0, [7]), '', rec(0x01, 0), ''].join('\n');
  assert.deepEqual(parseIhex(text).map((c) => c.addr), [0]);
});

test('неверная сумма — номер строки', () => {
  const bad = rec(0x00, 0, [1, 2]).slice(0, -2) + '00';
  assert.throws(() => parseIhex([rec(0x04, 0, [8, 0]), bad, rec(0x01, 0)].join('\n')), /строка 2: неверная контрольная сумма/);
});

test('длина не совпадает', () => {
  const r = rec(0x00, 0, [1, 2, 3]);
  const bad = r.slice(0, 1) + '05' + r.slice(3); // заявлено 5 байт, есть 3
  assert.throws(() => parseIhex(bad + '\n' + rec(0x01, 0)), /строка 1: длина 5 не совпадает/);
});

test('неизвестный тип', () => {
  assert.throws(() => parseIhex(rec(0x06, 0, [1]) + '\n' + rec(0x01, 0)), /строка 1: неизвестный тип записи 0x06/);
});

test('не hex и не «:»', () => {
  assert.throws(() => parseIhex('hello\n' + rec(0x01, 0)), /строка 1: строка не начинается с «:»/);
  assert.throws(() => parseIhex(':10zz\n' + rec(0x01, 0)), /строка 1: не hex/);
});

test('нет записи конца', () => {
  assert.throws(() => parseIhex(rec(0x00, 0, [1])), /нет записи конца/);
});

test('данные после конца', () => {
  assert.throws(() => parseIhex([rec(0x01, 0), rec(0x00, 0, [1])].join('\n')), /строка 2: данные после записи конца \(строка 1\)/);
});
