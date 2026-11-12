// Разговор с программатором на поддельном программаторе: строки и команды, сеанс записи и чтения в порядке
// PlatformIO, отказы до записи, сроки ответов (design §4–§7).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Flasher, hexDump, parseResult } from '../../docs/lib/flasher.js';
import { FakeFlasher } from './fake_flasher.mjs';
import { hexFile, fill } from './ihex_util.mjs';

const FAST = { packet: 200, erase: 200, done: 500, settle: 300, quiet: 30 };

async function connected(opts = {}) {
  const fake = new FakeFlasher(opts);
  const lines = [];
  const flasher = new Flasher(fake, { onLine: (l, own) => lines.push([l, own]), timeouts: FAST });
  await flasher.connect();
  await tick();
  return { fake, flasher, lines };
}

const tick = () => new Promise((r) => setTimeout(r, 10));

// Типы пакетов в порядке прихода, без аргументов.
function kinds(fake) {
  return fake.packets.map((p) => (p.startsWith('qRcmd,') ? 'qRcmd ' + Buffer.from(p.slice(6), 'hex').toString() : p.split(/[:;,]/)[0]));
}

const FW = { name: 'fw.hex', text: hexFile(0x08000000, fill(300, (i) => i & 0xff)) };
const OTP = { name: 'otp.hex', text: hexFile(0x1fff0280, fill(128, (i) => i)) };

// ---------------------------------------------------------------- текст и команды

test('после подключения — chip: выбранный чип и список', async () => {
  const { fake, flasher, lines } = await connected();
  assert.deepEqual(fake.commands, ['chip']);
  assert.equal(flasher.chip, 'py32f002b');
  assert.deepEqual(flasher.supported, ['py32f002b']);
  assert.deepEqual(lines, [
    ['# чип: py32f002b', true],
    ['# область 0x08000000–0x08005FFF: flash, основная', true],
    ['# область 0x1FFF0280–0x1FFF02FF: USER OTP', true],
    ['# поддержаны: py32f002b', true],
  ]);
});

test('области чипа из ответа chip; info их не сбрасывает', async () => {
  const { flasher } = await connected();
  assert.deepEqual(flasher.regions, [
    { start: 0x08000000, end: 0x08006000, name: 'flash', main: true },
    { start: 0x1fff0280, end: 0x1fff0300, name: 'USER OTP', main: false },
  ]);
  await flasher.command('info');
  await tick();
  assert.equal(flasher.regions.length, 2);
});

// spec: "web-flash/Терминал/Формат лога со страницы"
test('формат лога — baud со скоростью и форматом', async () => {
  const { fake, flasher, lines } = await connected();
  lines.length = 0;
  await flasher.command('baud 9600 8E1');
  await tick();
  assert.equal(fake.commands.at(-1), 'baud 9600 8E1');
  assert.deepEqual(lines.at(-1), ['# лог: 9600 8E1', true]);
});

test('чип не выбран, затем выбор', async () => {
  const { flasher } = await connected({ chip: null });
  assert.equal(flasher.chip, null);
  await flasher.command('chip py32f002b');
  await tick();
  assert.equal(flasher.chip, 'py32f002b');
});

test('лог платы — отдельно от строк программатора; CR LF и куски строк', async () => {
  const { fake, lines } = await connected();
  lines.length = 0;
  fake._data(new TextEncoder().encode('темпера'));
  fake._data(new TextEncoder().encode('тура 21\r\n# сброс платы\r\n'));
  assert.deepEqual(lines, [
    ['температура 21', false],
    ['# сброс платы', true],
  ]);
});

test('команда во время сеанса недоступна', async () => {
  const { flasher, fake } = await connected();
  fake.silent = true;
  const p = flasher.flash([FW]).catch((e) => e);
  await tick();
  await assert.rejects(flasher.command('info'), /идёт сеанс/);
  await p;
});

test('итог из строки программатора', () => {
  assert.deepEqual(parseResult('# итог: ок, 120 мс'), { ok: true, text: '120 мс' });
  assert.deepEqual(parseResult('# итог: ошибка, замок OTP не ставится'), { ok: false, text: 'замок OTP не ставится' });
  assert.equal(parseResult('# итог'), null);
});

// ---------------------------------------------------------------- запись

// spec: "web-flash/Прошивка за один сеанс/Прошивка и OTP"
test('прошивка и OTP за один сеанс — порядок PlatformIO, итог ок, лог платы', async () => {
  const { fake, flasher } = await connected();
  const progress = [];
  const r = await flasher.flash([FW, OTP], { onProgress: (p) => progress.push(p) });
  assert.equal(r.ok, true, r.reason);
  assert.deepEqual(
    kinds(fake).filter((k, i, a) => k !== a[i - 1]),
    ['qSupported', 'QStartNoAckMode', '!', 'qXfer', 'qRcmd swdp_scan', 'vAttach', 'vFlashErase', 'vFlashWrite', 'vFlashDone', 'vKill'],
  );
  assert.equal(fake.flash[299], 299 & 0xff);
  assert.equal(fake.flash[300], 0xff); // хвост задетой страницы стёрт
  assert.equal(fake.flash[0x180], 0x00); // соседняя страница не тронута
  assert.equal(fake.otp[127], 127);
  assert.ok(r.lines.includes('# итог: ок, 120 мс'));
  assert.ok(r.lines.includes('плата: старт'));
  const last = progress[progress.length - 1];
  assert.equal(last.sent, 428);
  assert.equal(last.total, 428);
  assert.equal(flasher.busy, false);
  assert.equal(fake.session, false);
});

// spec: "web-flash/Прошивка за один сеанс/Файл с замком OTP"
test('файл с замком OTP — итог ошибка, прошивка прежняя', async () => {
  const { fake, flasher } = await connected();
  const lock = { name: 'lock.hex', text: hexFile(0x1fff0280, [0x55, 0xaa]) };
  const r = await flasher.flash([FW, lock]);
  assert.equal(r.ok, false);
  assert.equal(r.reason, 'замок OTP не ставится');
  assert.ok(r.lines.includes('# замок OTP не ставится: 0x1FFF0280'));
  assert.equal(fake.flash[1], 0x00); // прошивка прежняя
  assert.equal(fake.otp[0], 0xff);
});

// spec: "web-flash/Образ из файлов и отказ до записи/Адрес вне карты памяти"
test('адрес вне карты — имя файла и адрес, сеанса записи нет', async () => {
  const { fake, flasher } = await connected();
  const big = { name: 'big.hex', text: hexFile(0x08005ff0, fill(32, 1)) };
  const r = await flasher.flash([big]);
  assert.equal(r.ok, false);
  assert.equal(r.reason, 'big.hex: адрес 0x08006000 вне карты памяти программатора');
  assert.equal(r.touched, false);
  assert.equal(fake.attaches, 0);
  assert.ok(!kinds(fake).some((k) => k.startsWith('vFlash') || k === 'vAttach'));
  assert.equal(kinds(fake).at(-1), 'D');
});

// spec: "web-flash/Образ из файлов и отказ до записи/Файлы противоречат друг другу"
test('файлы противоречат — оба имени и адрес, сеанса записи нет', async () => {
  const { fake, flasher } = await connected();
  const a = { name: 'a.hex', text: hexFile(0x08000000, fill(32, 1)) };
  const b = { name: 'b.hex', text: hexFile(0x08000010, [2]) };
  const r = await flasher.flash([a, b]);
  assert.equal(r.reason, 'a.hex и b.hex: разные байты по 0x08000010');
  assert.equal(fake.attaches, 0);
});

test('ошибка разбора — до порта: ни одного пакета', async () => {
  const { fake, flasher } = await connected();
  const r = await flasher.flash([{ name: 'x.hex', text: ':0000' }]);
  assert.match(r.reason, /^x\.hex: строка 1: /);
  assert.equal(fake.packets.length, 0);
});

test('полное стирание перед записью — erase в том же сеансе, до vFlashErase', async () => {
  const { fake, flasher } = await connected();
  fake.flash.fill(0x5c);
  const r = await flasher.flash([FW], { eraseAll: true });
  assert.equal(r.ok, true, r.reason);
  const k = kinds(fake);
  assert.ok(k.indexOf('qRcmd erase') > k.indexOf('vAttach'));
  assert.ok(k.indexOf('qRcmd erase') < k.indexOf('vFlashErase'));
  assert.equal(fake.flash[0x3200], 0xff); // страница 100 стёрта
  assert.equal(fake.flash[0], 0);
  assert.equal(r.lines.filter((l) => parseResult(l)).length, 2); // итог стирания и итог записи
});

test('стирание отказало — запись не начинается, итог ошибка', async () => {
  const { fake, flasher } = await connected();
  fake.wrperr = true;
  const r = await flasher.flash([FW], { eraseAll: true });
  assert.equal(r.ok, false);
  assert.equal(r.reason, 'полное стирание невозможно: защита записи');
  assert.ok(!kinds(fake).includes('vFlashErase'));
  assert.equal(kinds(fake).at(-1), 'vKill');
});

test('чип не отвечает — D, причина из строк программатора', async () => {
  const { fake, flasher } = await connected();
  fake.attachFails = true;
  const r = await flasher.flash([FW]);
  assert.equal(r.ok, false);
  assert.equal(r.reason, 'чип не отвечает');
  assert.ok(!kinds(fake).includes('vFlashErase'));
  assert.equal(kinds(fake).at(-1), 'D');
});

test('программатор не отвечает — ошибка срока, порт закрыт', async () => {
  const { fake, flasher } = await connected();
  fake.silent = true;
  await assert.rejects(flasher.flash([FW]), /программатор не отвечает/);
  assert.equal(flasher.connected, false);
  assert.equal(fake.closed, 1);
  assert.equal(flasher.busy, false);
});

test('после сеанса команды снова строкой', async () => {
  const { fake, flasher } = await connected();
  await flasher.flash([FW]);
  await flasher.command('info');
  await tick();
  assert.deepEqual(fake.commands, ['chip', 'info']);
});

test('USB выдернули посреди сеанса — ошибка, не зависание', async () => {
  const { fake, flasher } = await connected();
  fake.silent = true;
  const p = flasher.flash([FW]);
  await tick();
  fake.unplug();
  await assert.rejects(p, /программатор отключён/);
  assert.equal(flasher.connected, false);
});

test('ответ chip не ушёл — порт закрыт, а не брошен открытым', async () => {
  const fake = new FakeFlasher();
  const write = fake.write.bind(fake);
  fake.write = async () => {
    throw new Error('порт сбоит');
  };
  const flasher = new Flasher(fake, { timeouts: FAST });
  await assert.rejects(flasher.connect(), /порт сбоит/);
  assert.equal(fake.isOpen, false);
  assert.equal(flasher.connected, false);
  fake.write = write;
  await flasher.connect(); // повторное подключение тем же портом
  assert.equal(flasher.connected, true);
});

test('пакет не записался — ошибка записи, ожидание снято, сеанс закрыт D', async () => {
  const { fake, flasher } = await connected();
  const write = fake.write.bind(fake);
  let calls = 0;
  fake.write = async (bytes) => {
    if (++calls === 2) throw new Error('порт сбоит'); // первый — «+», второй — qSupported
    return write(bytes);
  };
  await assert.rejects(flasher.flash([FW]), /порт сбоит/);
  assert.equal(flasher.waiter, null);
  assert.equal(kinds(fake).at(-1), 'D');
  await new Promise((r) => setTimeout(r, FAST.packet + 50)); // срок старого ожидания прошёл без следа
  assert.equal(flasher.connected, true);
  assert.equal((await flasher.read(0x1fff0280, 16)).ok, true);
});

test('пустая карта памяти — ошибка, сеанс закрыт D и переводом строки', async () => {
  const { fake, flasher } = await connected({ chip: null });
  await assert.rejects(flasher.flash([FW]), /пустую карту памяти/);
  assert.equal(kinds(fake).at(-1), 'D');
  assert.equal(fake.mode, 'text');
  assert.equal(flasher.busy, false);
});

// ---------------------------------------------------------------- чтение и карта

// spec: "web-flash/Чтение памяти/Чтение USER OTP"
test('чтение USER OTP — 8 строк hex, D, без итога ошибка', async () => {
  const { fake, flasher } = await connected();
  fake.otp.set(fill(128, (i) => i));
  const r = await flasher.read(0x1fff0280, 128);
  assert.equal(r.ok, true, r.reason);
  const dump = hexDump(0x1fff0280, r.bytes);
  assert.equal(dump.length, 8);
  assert.equal(dump[0], '0x1FFF0280  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F');
  assert.equal(dump[7].slice(0, 10), '0x1FFF02F0');
  assert.deepEqual(kinds(fake).filter((k) => k[0] !== 'm'), ['qSupported', 'QStartNoAckMode', 'qRcmd swdp_scan', 'vAttach', 'D']);
  assert.ok(!r.lines.some((l) => parseResult(l) && !parseResult(l).ok));
});

test('чтение кусками по 0x400 Б', async () => {
  const { fake, flasher } = await connected();
  const r = await flasher.read(0x08000000, 0x900);
  assert.equal(r.ok, true);
  assert.deepEqual(fake.packets.filter((p) => p[0] === 'm'), ['m8000000,400', 'm8000400,400', 'm8000800,100']);
});

test('чтение вне памяти — причина с адресом', async () => {
  const { flasher } = await connected();
  const r = await flasher.read(0x30000000, 16);
  assert.equal(r.ok, false);
  assert.equal(r.reason, 'чтение не удалось: 0x30000000');
});

test('карта памяти без подключения к плате', async () => {
  const { fake, flasher } = await connected();
  const map = await flasher.memoryMap();
  assert.deepEqual(map.map((r) => [r.type, r.start, r.length]), [
    ['flash', 0x08000000, 0x6000],
    ['flash', 0x1fff0280, 0x80],
    ['ram', 0x20000000, 0xc00],
  ]);
  assert.equal(fake.attaches, 0);
  assert.equal(kinds(fake).at(-1), 'D');
});
