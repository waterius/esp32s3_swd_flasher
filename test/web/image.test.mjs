// Образ из файлов: карта памяти, противоречия, блоки стирания, куски записи (design §3, §5).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseFile, buildImage, parseMemoryMap, writeChunks, hex32, ImageError } from '../../docs/lib/image.js';
import { escapedLength } from '../../docs/lib/rsp.js';
import { hexFile, fill } from './ihex_util.mjs';

// Карта памяти, которую отдаёт оснастка с py32f002b (src/core/gdb.cpp, memory_map).
export const MAP_XML =
  '<?xml version="1.0"?><!DOCTYPE memory-map PUBLIC "+//IDN gnu.org//DTD GDB Memory Map V1.0//EN" ' +
  '"http://sourceware.org/gdb/gdb-memory-map.dtd"><memory-map>' +
  '<memory type="flash" start="0x8000000" length="0x6000"><property name="blocksize">0x80</property></memory>' +
  '<memory type="flash" start="0x1fff0280" length="0x80"><property name="blocksize">0x80</property></memory>' +
  '<memory type="ram" start="0x20000000" length="0xc00"/></memory-map>';
const MAP = parseMemoryMap(MAP_XML);

test('карта памяти разбирается', () => {
  assert.deepEqual(MAP, [
    { type: 'flash', start: 0x08000000, length: 0x6000, blocksize: 0x80 },
    { type: 'flash', start: 0x1fff0280, length: 0x80, blocksize: 0x80 },
    { type: 'ram', start: 0x20000000, length: 0xc00, blocksize: 0 },
  ]);
});

test('адрес для человека', () => assert.equal(hex32(0x8000010), '0x08000010'));

test('прошивка и OTP — блоки и куски', () => {
  const fw = parseFile('fw.hex', hexFile(0x08000000, fill(300, (i) => i & 0xff)));
  const otp = parseFile('otp.hex', hexFile(0x1fff0280, fill(128, 0x11)));
  assert.deepEqual(fw.ranges, [{ start: 0x08000000, end: 0x0800012c }]);
  const img = buildImage([fw, otp], MAP);
  assert.equal(img.size, 428);
  assert.deepEqual(img.erase, [
    { addr: 0x08000000, len: 0x180 },
    { addr: 0x1fff0280, len: 0x80 },
  ]);
  assert.deepEqual(img.runs.map((r) => [r.addr, r.bytes.length]), [
    [0x08000000, 300],
    [0x1fff0280, 128],
  ]);
});

test('адрес вне карты — имя файла и адрес', () => {
  const f = parseFile('big.hex', hexFile(0x08005ff0, fill(32, 0)));
  assert.throws(() => buildImage([f], MAP), (e) => e instanceof ImageError && e.message === 'big.hex: адрес 0x08006000 вне карты памяти оснастки');
});

test('ОЗУ — не область записи', () => {
  const f = parseFile('ram.hex', hexFile(0x20000000, [1]));
  assert.throws(() => buildImage([f], MAP), /ram\.hex: адрес 0x20000000 вне карты/);
});

test('файлы противоречат — оба имени и адрес', () => {
  const a = parseFile('a.hex', hexFile(0x08000000, fill(32, 0x01)));
  const b = parseFile('b.hex', hexFile(0x08000010, [0x02]));
  assert.throws(() => buildImage([a, b], MAP), (e) => e.message === 'a.hex и b.hex: разные байты по 0x08000010');
});

test('совпадающие байты двух файлов — не ошибка', () => {
  const a = parseFile('a.hex', hexFile(0x08000000, fill(32, 0x01)));
  const b = parseFile('b.hex', hexFile(0x08000010, [0x01, 0x01]));
  assert.equal(buildImage([a, b], MAP).size, 32);
});

test('ошибка разбора — с именем файла', () => {
  assert.throws(() => parseFile('x.hex', ':00000001FF\n:00000001FF'), /^Error: x\.hex: строка 2: данные после записи конца/);
});

test('блоки в одной области сливаются, через разрыв — нет', () => {
  const f = parseFile('f.hex', hexFile(0x08000000, [1]) .replace(/:00000001FF\r\n$/, '') + hexFile(0x08000100, [2]));
  const img = buildImage([f], MAP);
  assert.deepEqual(img.erase, [
    { addr: 0x08000000, len: 0x80 },
    { addr: 0x08000100, len: 0x80 },
  ]);
});

test('куски записи с экранированием помещаются в предел', () => {
  const bytes = Uint8Array.from(fill(5000, (i) => (i % 3 ? 0x23 : 0x01))); // много «#»
  const chunks = writeChunks([{ addr: 0x08000000, bytes }], 4032, escapedLength);
  let total = 0;
  for (const c of chunks) {
    const head = `vFlashWrite:${c.addr.toString(16)}:`.length;
    assert.ok(head + escapedLength(c.bytes) <= 4032);
    total += c.bytes.length;
  }
  assert.equal(total, 5000);
  assert.equal(chunks[1].addr, 0x08000000 + chunks[0].bytes.length);
});
