// Кадры gdb: сумма, экранирование, hex, поиск пакета среди лишних байтов (design §3, §5).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { frame, escape, escapedLength, toHex, fromHex, Parser, checksum } from '../../docs/lib/rsp.js';

const dec = new TextDecoder();

test('кадр и сумма, как у gdb', () => {
  assert.equal(dec.decode(frame('OK')), '$OK#9a');
  assert.equal(dec.decode(frame('qSupported')), '$qSupported#37');
});

test('экранирование # $ } *', () => {
  const data = Uint8Array.from([0x23, 0x24, 0x7d, 0x2a, 0x41]);
  assert.deepEqual([...escape(data)], [0x7d, 0x03, 0x7d, 0x04, 0x7d, 0x5d, 0x7d, 0x0a, 0x41]);
  assert.equal(escapedLength(data), 9);
  const f = frame(data);
  assert.equal(f[0], 0x24);
  assert.equal(dec.decode(f.slice(-3, -2)), '#');
  assert.equal(checksum(escape(data)), parseInt(dec.decode(f.slice(-2)), 16));
});

test('hex туда и обратно', () => {
  assert.equal(toHex(Uint8Array.from([0, 0xab, 0x10])), '00ab10');
  assert.deepEqual([...fromHex('00AB10')], [0, 0xab, 0x10]);
  assert.equal(fromHex('0'), null);
  assert.equal(fromHex('zz'), null);
});

function feedAll(p, bytes) {
  const out = [];
  for (const b of bytes) {
    const e = p.feed(b);
    if (e) out.push(e);
  }
  return out;
}

test('пакет среди лога и подтверждений', () => {
  const p = new Parser();
  const enc = new TextEncoder();
  const events = feedAll(p, [...enc.encode('лог платы\r\n+'), ...frame('OK')]);
  const packets = events.filter((e) => e.type === 'packet');
  assert.equal(packets.length, 1);
  assert.equal(dec.decode(packets[0].payload), 'OK');
  assert.ok(events.some((e) => e.type === 'other' && e.byte === 0x2b));
});

test('неверная сумма — bad, следующий пакет принимается', () => {
  const p = new Parser();
  const enc = new TextEncoder();
  const events = feedAll(p, [...enc.encode('$OK#00'), ...frame('E01')]);
  assert.deepEqual(events.map((e) => e.type), ['bad', 'packet']);
  assert.equal(dec.decode(events[1].payload), 'E01');
});

test('«$» посреди пакета начинает пакет заново', () => {
  const p = new Parser();
  const enc = new TextEncoder();
  const events = feedAll(p, [...enc.encode('$обрывок'), ...frame('T05')]);
  assert.deepEqual(events.map((e) => e.type), ['packet']);
  assert.equal(dec.decode(events[0].payload), 'T05');
});

test('экранирование снимается при приёме', () => {
  const p = new Parser();
  const data = Uint8Array.from([0x23, 0x7d, 0x01]);
  const events = feedAll(p, frame(data));
  assert.deepEqual([...events[0].payload], [0x23, 0x7d, 0x01]);
});
