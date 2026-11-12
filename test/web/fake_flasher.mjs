// Поддельный программатор для тестов flasher.js: порт программатора (design §4), текстовый режим и режим
// gdb по таблице пакетов swd-flash design §9, «кто открыл порт», вывод после vKill и D — по байту
// от человека, поддельный чип с картой памяти py32f002b. Поведение — как у src/core/console.cpp,
// src/core/gdb.cpp и src/core/commands.cpp; здесь его упрощённый двойник.
import { frame, toHex, fromHex, Parser } from '../../docs/lib/rsp.js';

const enc = new TextEncoder();
const dec = new TextDecoder();

const FLASH = 0x08000000;
const FLASH_SIZE = 24 * 1024;
const OTP = 0x1fff0280;
const RAM = 0x20000000;

const MAP =
  '<?xml version="1.0"?><!DOCTYPE memory-map PUBLIC "+//IDN gnu.org//DTD GDB Memory Map V1.0//EN" ' +
  '"http://sourceware.org/gdb/gdb-memory-map.dtd"><memory-map>' +
  '<memory type="flash" start="0x8000000" length="0x6000"><property name="blocksize">0x80</property></memory>' +
  '<memory type="flash" start="0x1fff0280" length="0x80"><property name="blocksize">0x80</property></memory>' +
  '<memory type="ram" start="0x20000000" length="0xc00"/></memory-map>';

const SUPPORTED = 'PacketSize=1000;qXfer:memory-map:read+;qXfer:features:read+;QStartNoAckMode+';

export class FakeFlasher {
  constructor({ chip = 'py32f002b' } = {}) {
    this.chip = chip;
    this.log = { baud: 115200, format: '8N1' };
    this.flash = new Uint8Array(FLASH_SIZE).fill(0x00); // старая прошивка
    this.otp = new Uint8Array(128).fill(0xff);
    // подстройки
    this.silent = false; // на пакеты не отвечает
    this.attachFails = false;
    this.wrperr = false; // полное стирание и запись — WRPERR
    // наблюдение
    this.packets = []; // принятые пакеты строками (vFlashWrite — без данных)
    this.commands = []; // команды строкой
    this.opened = 0;
    this.closed = 0;
    this.isOpen = false;
    this.attaches = 0;
    // состояние консоли
    this.mode = 'closed';
    this.parser = new Parser();
    this.line = '';
    this.candidate = false;
    this.out = [];
    this.session = false;
    this.flashed = false;
    this.writeOk = false;
    this.writeFailed = false;
    this.imageOpen = false;
    this.image = null;
    this.noack = false;
    this._data = () => {};
    this._close = () => {};
  }

  // ---------------------------------------------------------------- порт программатора

  async open() {
    if (this.isOpen) throw new Error('порт уже открыт');
    this.isOpen = true;
    this.opened++;
    this.mode = 'unknown';
    this.parser.reset();
    this.line = '';
  }

  async close() {
    if (!this.isOpen) return;
    this.isOpen = false;
    this.closed++;
    if (this.mode === 'gdb') this._endSession(); // DTR упал
    this.mode = 'closed';
  }

  async write(bytes) {
    if (!this.isOpen) throw new Error('порт закрыт');
    const copy = Uint8Array.from(bytes);
    await Promise.resolve();
    for (const b of copy) this._byte(b);
  }

  onData(cb) {
    this._data = cb;
  }

  onClose(cb) {
    this._close = cb;
  }

  /** Выдернули USB. */
  unplug() {
    this.isOpen = false;
    this.mode = 'closed';
    this._close();
  }

  /** Плата пишет строку лога. */
  boardLog(text) {
    this._put(enc.encode(text + '\r\n'));
  }

  // ---------------------------------------------------------------- вывод

  _put(bytes) {
    if (this.mode === 'text') {
      const copy = Uint8Array.from(bytes);
      queueMicrotask(() => this.isOpen && this._data(copy));
    } else if (this.mode !== 'closed') {
      this.out.push(...bytes);
    }
  }

  _print(line) {
    this._put(enc.encode(line + '\r\n'));
  }

  _flush() {
    if (!this.out.length) return;
    const copy = Uint8Array.from(this.out);
    this.out = [];
    queueMicrotask(() => this.isOpen && this._data(copy));
  }

  _reply(text) {
    if (this.silent) return;
    const f = frame(text);
    queueMicrotask(() => this.isOpen && this._data(f));
  }

  // ---------------------------------------------------------------- режимы

  _byte(b) {
    if (this.mode === 'unknown') {
      if (b === 0x2b || b === 0x24) this._enterGdb();
      else this._toText();
    }
    if (this.mode === 'gdb') return this._gdbByte(b);
    if (this.mode === 'text') return this._textByte(b);
  }

  _enterGdb() {
    this.mode = 'gdb';
    this.noack = false;
  }

  _toText() {
    this.mode = 'text';
    this.parser.reset();
    this.candidate = false;
    this.line = '';
    this._flush();
  }

  _gdbByte(b) {
    const e = this.parser.feed(b);
    if (!e) return;
    if (e.type === 'packet') this._packet(e.payload);
    else if (e.type === 'other' && b !== 0x2b && b !== 0x2d) {
      this._endSession(); // байт вне пакета — человек
      this._toText();
      this._textByte(b);
    }
  }

  _textByte(b) {
    if (this.candidate) {
      if (b === 0x0a || b === 0x0d) {
        this.candidate = false;
        return this._exec();
      }
      this.line += String.fromCharCode(b);
      const e = this.parser.feed(b);
      if (e && e.type === 'packet') {
        this.candidate = false;
        this.line = '';
        this._enterGdb();
        this._packet(e.payload);
      } else if (e && e.type === 'bad') {
        this.candidate = false;
      }
      return;
    }
    if (b === 0x0a || b === 0x0d) return this._exec();
    if (!this.line && (b === 0x2b || b === 0x2d)) return;
    if (!this.line && b === 0x24) {
      this.candidate = true;
      this.parser.reset();
      this.parser.feed(b);
    }
    this.line += String.fromCharCode(b);
  }

  _exec() {
    const line = dec.decode(Uint8Array.from(this.line, (c) => c.charCodeAt(0)));
    this.line = '';
    if (!line) return; // пустая строка
    this.commands.push(line);
    this._command(line, (l) => this._print(l), 'text');
  }

  // ---------------------------------------------------------------- команды (core/commands)

  // Как core/commands: записываемые области py32f002b.
  _regions(emit) {
    emit('# область 0x08000000–0x08005FFF: flash, основная');
    emit('# область 0x1FFF0280–0x1FFF02FF: USER OTP');
  }

  _command(line, emit, source) {
    const [word, ...rest] = line.split(' ');
    const arg = rest.join(' ').trim();
    if (word === 'chip') {
      if (arg) {
        if (this.session) return emit('# идёт сеанс'), 'none';
        if (arg !== 'py32f002b') {
          emit(`# чип не поддержан: ${arg}`);
          emit('# поддержаны: py32f002b');
          return 'none';
        }
        this.chip = arg;
        emit(`# чип: ${arg}`);
        this._regions(emit);
        return 'none';
      }
      if (this.chip) {
        emit(`# чип: ${this.chip}`);
        this._regions(emit);
      } else {
        emit('# чип не выбран: chip <имя>');
      }
      emit('# поддержаны: py32f002b');
      return 'none';
    }
    if (word === 'baud') {
      const [speed, format] = arg.split(' ').filter(Boolean);
      if (speed) {
        const v = Number(speed);
        if (!/^\d+$/.test(speed) || v < 1200 || v > 2000000) return emit('# скорость лога: от 1200 до 2000000'), 'none';
        if (format && !/^[5-8][NEO][12]$/.test(format)) {
          return emit('# формат лога: биты 5–8, чётность N, E или O, стоп 1 или 2 — например 8N1'), 'none';
        }
        this.log = { baud: v, format: format || this.log.format };
      }
      emit(`# лог: ${this.log.baud} ${this.log.format}`);
      return 'none';
    }
    if (word === 'reset') {
      if (this.session) return emit('# идёт сеанс'), 'none';
      emit('# сброс платы');
      this.boardLog('плата: старт');
      return 'none';
    }
    if (word === 'info') {
      if (!this.chip) return emit('# чип не выбран: chip <имя>'), 'none';
      emit(`# чип: ${this.chip}, SWD под сбросом`);
      emit('# DBG_IDCODE: 0x20220064');
      return 'none';
    }
    if (word === 'swdp_scan') {
      if (source !== 'gdb') return emit('# swdp_scan — только через monitor в gdb'), 'none';
      if (!this.chip) return emit('# чип не выбран: chip <имя>'), 'none';
      if (!this.session && !this._start(emit)) return 'none';
      emit('1 py32f002b');
      return 'none';
    }
    if (word === 'erase') {
      if (!this.chip) return emit('# чип не выбран: chip <имя>'), 'none';
      const own = !this.session;
      if (own && !this._start(emit)) return 'failed';
      const ok = this._eraseMain((l) => (this._print(l), source === 'gdb' && emit(l)));
      if (own) this._endSession();
      return ok ? 'ok' : 'failed';
    }
    emit('# команды:');
    emit('#   help             эта подсказка');
    return 'none';
  }

  // ---------------------------------------------------------------- сеанс и чип

  _start(emit) {
    this.attaches++;
    if (this.attachFails) {
      this._print('# чип не отвечает');
      if (emit) emit('# чип не отвечает');
      return false;
    }
    this.session = true;
    this._print('# SWD: под сбросом, IDCODE 0x0BC11477, 3 мс');
    return true;
  }

  _endSession() {
    if (this.session) {
      if (this.flashed) this._print('# сеанс: 250 мс');
      this.boardLog('плата: старт'); // плата запущена заново — первые строки её лога
    }
    this.session = false;
    this.flashed = false;
    this.writeOk = this.writeFailed = this.imageOpen = false;
  }

  _eraseMain(say) {
    this.flashed = true;
    if (this.wrperr) {
      say('# полное стирание невозможно: защита записи');
      say('# итог: ошибка, полное стирание невозможно: защита записи');
      return false;
    }
    this.flash.fill(0xff);
    say('# полное стирание: 3,5 мс');
    say('# проверка 0xFF: 300 мс');
    say('# итог: ок, 310 мс');
    return true;
  }

  _mem(addr, len) {
    if (addr >= FLASH && addr + len <= FLASH + FLASH_SIZE) return this.flash.subarray(addr - FLASH, addr - FLASH + len);
    if (addr >= OTP && addr + len <= OTP + 128) return this.otp.subarray(addr - OTP, addr - OTP + len);
    if (addr >= RAM && addr + len <= RAM + 0xc00) return new Uint8Array(len);
    return null;
  }

  _inWritable(addr, len) {
    return (addr >= FLASH && addr + len <= FLASH + FLASH_SIZE) || (addr >= OTP && addr + len <= OTP + 128);
  }

  // Запись образа единицами по 128 Б: задетая единица пишется целиком (core/write).
  _flashImage() {
    this.flashed = true;
    const units = new Map(); // начало единицы → Uint8Array(128)
    for (const [addr, bytes] of this.image.parts) {
      for (let i = 0; i < bytes.length; i++) {
        const a = addr + i;
        const u = a - ((a - (a >= OTP ? OTP : FLASH)) % 128);
        if (!units.has(u)) units.set(u, new Uint8Array(128).fill(0xff));
        units.get(u)[a - u] = bytes[i];
      }
    }
    for (const u of this.image.erased) if (!units.has(u)) units.set(u, new Uint8Array(128).fill(0xff));
    const otp = units.get(OTP);
    if (otp && otp[0] === 0x55 && otp[1] === 0xaa) {
      this._print('# замок OTP не ставится: 0x1FFF0280');
      this._print('# итог: ошибка, замок OTP не ставится');
      return false;
    }
    if (this.wrperr) {
      this._print('# ошибка записи: 0x08000000, WRPERR');
      this._print('# итог: ошибка, ошибка записи: 0x08000000, WRPERR');
      return false;
    }
    for (const [u, data] of units) this._mem(u, 128).set(data);
    this._print(`# flash: ${units.size} стр.`);
    this._print('# сверка: 5 мс');
    this._print('# итог: ок, 120 мс');
    return true;
  }

  // ---------------------------------------------------------------- пакеты (core/gdb)

  _packet(payload) {
    if (!this.noack && !this.silent) queueMicrotask(() => this.isOpen && this._data(enc.encode('+')));
    const s = dec.decode(payload);
    this.packets.push(s.startsWith('vFlashWrite:') ? s.slice(0, s.indexOf(':', 12)) : s);
    const session = this.session;
    if (s.startsWith('qSupported')) return this._reply(SUPPORTED);
    if (s === 'QStartNoAckMode') {
      this._reply('OK');
      this.noack = true;
      return;
    }
    if (s === '!') return this._reply('OK');
    if (s.startsWith('qRcmd,')) {
      const cmd = dec.decode(fromHex(s.slice(6)));
      const emit = (l) => this._reply('O' + toHex(enc.encode(l + '\n')));
      const outcome = this._command(cmd, emit, 'gdb');
      if (outcome === 'ok') this.writeOk = true;
      if (outcome === 'failed') this.writeFailed = true;
      return this._reply('OK');
    }
    if (s.startsWith('vAttach;')) {
      const ok = session || (this.chip && this._start(null));
      return this._reply(ok ? 'T05' : 'E01');
    }
    if (s.startsWith('qXfer:memory-map:read::')) {
      if (!this.chip) return this._reply('l');
      const [off, len] = s.slice(23).split(',').map((x) => parseInt(x, 16));
      if (off >= MAP.length) return this._reply('l');
      const part = MAP.slice(off, off + len);
      return this._reply((off + part.length < MAP.length ? 'm' : 'l') + part);
    }
    if (s[0] === 'm') {
      const [addr, len] = s.slice(1).split(',').map((x) => parseInt(x, 16));
      const mem = session && this._mem(addr, len);
      return this._reply(mem ? toHex(mem) : 'E01');
    }
    if (s.startsWith('vFlashErase:')) {
      const [addr, len] = s.slice(12).split(',').map((x) => parseInt(x, 16));
      if (!session) {
        this.writeFailed = true;
        return this._reply('E01');
      }
      if (!this.imageOpen) {
        this.image = { parts: [], erased: [] };
        this.imageOpen = true;
      }
      if (!this._inWritable(addr, len)) return this._refuse(addr);
      for (let u = addr; u < addr + len; u += 128) this.image.erased.push(u);
      return this._reply('OK');
    }
    if (s.startsWith('vFlashWrite:')) {
      const colon = s.indexOf(':', 12);
      const addr = parseInt(s.slice(12, colon), 16);
      const data = payload.subarray(enc.encode(s.slice(0, colon + 1)).length);
      if (!session || !this.imageOpen) {
        this.writeFailed = true;
        return this._reply('E01');
      }
      if (!this._inWritable(addr, data.length)) return this._refuse(addr);
      this.image.parts.push([addr, Uint8Array.from(data)]);
      return this._reply('OK');
    }
    if (s === 'vFlashDone') {
      if (!session || !this.imageOpen) {
        this.writeFailed = true;
        return this._reply('E01');
      }
      this.imageOpen = false;
      const ok = this._flashImage();
      if (ok) this.writeOk = true;
      else this.writeFailed = true;
      return this._reply(ok ? 'OK' : 'E01');
    }
    if (s.startsWith('vKill')) {
      const ok = this.writeOk && !this.writeFailed;
      this._endSession();
      return this._reply(ok ? 'OK' : 'E01');
    }
    if (s === 'D' || s.startsWith('D;')) {
      this._endSession();
      return this._reply('OK');
    }
    return this._reply('');
  }

  _refuse(addr) {
    this.writeFailed = true;
    this._print(`# адрес вне памяти чипа: 0x${addr.toString(16).toUpperCase().padStart(8, '0')}`);
    this._print('# итог: ошибка, адрес вне памяти чипа');
    return this._reply('E01');
  }
}
