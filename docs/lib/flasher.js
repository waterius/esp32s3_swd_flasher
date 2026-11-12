// Разговор с программатором Jaffa Flasher: текст и команды, разбор строк «# », сеанс записи и чтения по протоколу
// gdb — тот же порядок, что у PlatformIO (design §5–§7). Web Serial и страницу не знает: порт
// приходит протоколом «порт программатора» (design §4). Об изделиях ничего не знает.

import { frame, framePacket, fromHex, toHex, needsEscape, Parser } from './rsp.js';
import { parseFile, buildImage, parseMemoryMap, writeChunks, hex32, ImageError } from './image.js';

/**
 * Порт программатора — что flasher.js ждёт от порта (реализация в браузере — webserial.js):
 * @typedef {object} FlasherPort
 * @property {() => Promise<void>} open  открыть и поднять DTR и RTS; повторный open — ошибка
 * @property {() => Promise<void>} close  закрыть, отпустив потоки и опустив линии
 * @property {(bytes: Uint8Array) => Promise<void>} write  запись в закрытый порт — ошибка
 * @property {(cb: (bytes: Uint8Array) => void) => void} onData  принятые байты
 * @property {(cb: () => void) => void} onClose  порт пропал (выдернули USB)
 */

/** Сроки ответов программатора, мс (design §5). */
export const TIMEOUTS = {
  packet: 2000, // обычный пакет
  erase: 10000, // qRcmd erase: стирание и чтение всей основной flash по SWD
  done: 10000, // vFlashDone
  settle: 2000, // строки итога после возврата в текстовый режим
  quiet: 300, // тишина, после которой строк сеанса больше не ждём
};

const PACKET_SIZE = 0x1000; // PacketSize программатора
const WRITE_LIMIT = PACKET_SIZE - 64; // кусок vFlashWrite после экранирования (design §5)
const READ_CHUNK = 0x400; // m: ответ в hex вдвое длиннее (design §7)
const XFER_CHUNK = 0x800;

const enc = new TextEncoder();
const dec = new TextDecoder();

/** Строки hex по 16 Б с адресом: «0x1FFF0280  FF FF …». */
export function hexDump(addr, bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 16) {
    const row = Array.from(bytes.subarray(i, i + 16), (b) => b.toString(16).toUpperCase().padStart(2, '0'));
    out.push(`${hex32(addr + i)}  ${row.join(' ')}`);
  }
  return out;
}

/** Итог из строки программатора «# итог: ок, 120 мс» / «# итог: ошибка, причина» или null. */
export function parseResult(line) {
  const m = /^# итог: (ок|ошибка)(?:, (.*))?$/.exec(line);
  return m ? { ok: m[1] === 'ок', text: m[2] || '' } : null;
}

class Timeout extends Error {
  constructor() {
    super('программатор не отвечает');
  }
}

export class Flasher {
  /**
   * @param {FlasherPort} port
   * @param {object} [opts]
   * @param {(line: string, own: boolean) => void} [opts.onLine]  строка вывода; own — строка программатора «# »
   * @param {(state: object) => void} [opts.onState]  подключение, чип, сеанс изменились
   * @param {object} [opts.timeouts]  сроки вместо TIMEOUTS
   */
  constructor(port, opts = {}) {
    this.port = port;
    this.onLine = opts.onLine || (() => {});
    this.onState = opts.onState || (() => {});
    this.t = { ...TIMEOUTS, ...(opts.timeouts || {}) };
    this.connected = false;
    this.busy = false; // идёт сеанс gdb: поле команд недоступно
    this.chip = null;
    this.supported = [];
    this.map = null;
    this.mode = 'text';
    this.parser = new Parser();
    this.decoder = new TextDecoder();
    this.partial = '';
    this.waiter = null;
    this.collect = null; // строки, принятые после сеанса
    this.lastLineAt = 0;
    this.console = []; // строки из пакетов O в сеансе — по ним видно итог erase
    port.onData((bytes) => this._data(bytes));
    port.onClose(() => this._lost());
  }

  state() {
    return { connected: this.connected, busy: this.busy, chip: this.chip, supported: this.supported, map: this.map };
  }

  _changed() {
    this.onState(this.state());
  }

  // ---------------------------------------------------------------- порт

  /** Открыть порт. После открытия страница молчит, а потом спрашивает chip (design §6). */
  async connect() {
    await this.port.open();
    this.connected = true;
    this.mode = 'text';
    this.partial = '';
    this._changed();
    try {
      await this.command('chip');
    } catch (e) {
      await this.disconnect().catch(() => {}); // порт не остаётся открытым без хозяина
      throw e;
    }
  }

  async disconnect() {
    if (!this.connected) return;
    this.connected = false;
    this._fail(new Error('порт закрыт'));
    try {
      await this.port.close();
    } finally {
      this.busy = false;
      this._changed();
    }
  }

  _lost() {
    if (!this.connected) return;
    this.connected = false;
    this.busy = false;
    this._fail(new Error('программатор отключён'));
    this._changed();
  }

  /** Команда программатора строкой (help, chip, info, erase, reset, baud). */
  async command(line) {
    if (!this.connected) throw new Error('программатор не подключён');
    if (this.busy) throw new Error('идёт сеанс');
    await this.port.write(enc.encode(line + '\n'));
  }

  // ---------------------------------------------------------------- приём

  _data(bytes) {
    if (this.mode === 'text') return this._text(bytes);
    const other = [];
    for (const b of bytes) {
      const e = this.parser.feed(b);
      if (!e) continue;
      if (e.type === 'packet') this._packet(e.payload);
      else if (e.type === 'other' && b !== 0x2b && b !== 0x2d) other.push(b); // лог до перехода
    }
    if (other.length) this._text(Uint8Array.from(other));
  }

  _text(bytes) {
    this.partial += this.decoder.decode(bytes, { stream: true });
    let i;
    while ((i = this.partial.indexOf('\n')) >= 0) {
      const line = this.partial.slice(0, i).replace(/\r$/, '');
      this.partial = this.partial.slice(i + 1);
      this._line(line);
    }
  }

  _line(line) {
    const own = line.startsWith('# ');
    this.lastLineAt = Date.now();
    if (own) {
      let m;
      if ((m = /^# чип: ([^,\s]+)/.exec(line))) this._setChip(m[1]);
      else if (line.startsWith('# чип не выбран')) this._setChip(null);
      else if ((m = /^# поддержаны: (.*)$/.exec(line))) {
        this.supported = m[1].split(',').map((s) => s.trim()).filter(Boolean);
        this._changed();
      }
    }
    if (this.collect) this.collect.push(line);
    this.onLine(line, own);
  }

  _setChip(name) {
    if (this.chip !== name) this.map = null;
    this.chip = name;
    this._changed();
  }

  _packet(payload) {
    const s = dec.decode(payload);
    // O<hex> — вывод команды monitor; «OK» — ответ
    if (s.length > 1 && s[0] === 'O' && s !== 'OK') {
      const bytes = fromHex(s.slice(1));
      if (bytes) {
        for (const l of dec.decode(bytes).split('\n')) if (l) this.console.push(l.replace(/\r$/, ''));
        return;
      }
    }
    if (this.waiter) {
      const w = this.waiter;
      this.waiter = null;
      w.resolve(s);
    } // ответ без запроса — опоздавший после срока: отбрасываем
  }

  _fail(err) {
    if (this.waiter) {
      const w = this.waiter;
      this.waiter = null;
      w.reject(err);
    }
  }

  // ---------------------------------------------------------------- пакеты

  // Отправить пакет и ждать ответ. Ожидание одно: ответ, срок или ошибка записи снимают его.
  async _send(payload, timeout = this.t.packet) {
    const bytes = typeof payload === 'string' ? frame(payload) : payload;
    const reply = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.waiter === waiter) this.waiter = null;
        reject(new Timeout());
      }, timeout);
      const waiter = {
        resolve: (v) => (clearTimeout(timer), resolve(v)),
        reject: (e) => (clearTimeout(timer), reject(e)),
      };
      this.waiter = waiter;
    });
    try {
      await this.port.write(bytes);
    } catch (e) {
      this._fail(e);
    }
    return reply;
  }

  // Начало сеанса gdb: подтверждение, qSupported, режим без подтверждений (design §5, шаг 1).
  async _hello() {
    this.mode = 'gdb';
    this.parser.reset();
    this.console = [];
    await this.port.write(enc.encode('+'));
    const sup = await this._send('qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+');
    if (!/PacketSize=/.test(sup)) throw new Error(`программатор не отвечает как gdb-сервер: ${sup || 'пусто'}`);
    await this._send('QStartNoAckMode');
  }

  async _memoryMap() {
    let xml = '';
    for (;;) {
      const r = await this._send(`qXfer:memory-map:read::${xml.length.toString(16)},${XFER_CHUNK.toString(16)}`);
      if (r[0] !== 'm' && r[0] !== 'l') throw new Error(`карта памяти: ${r || 'пусто'}`);
      xml += r.slice(1);
      if (r[0] === 'l') break;
    }
    const map = parseMemoryMap(xml);
    if (!map.length) throw new Error('программатор отдал пустую карту памяти: выбран ли чип?');
    this.map = map;
    return map;
  }

  async _monitor(cmd, timeout) {
    return this._send('qRcmd,' + toHex(enc.encode(cmd)), timeout);
  }

  // Подключение к плате: swdp_scan и vAttach, как PlatformIO (design §5, шаг 3).
  async _attach() {
    await this._monitor('swdp_scan');
    const r = await this._send('vAttach;1');
    return r.startsWith('T');
  }

  /**
   * Конец сеанса: last — 'vKill;1' или 'D'; затем перевод строки — программатор выводит
   * накопленное (design §5, шаги 6–7). expect — сколько строк «# итог:» ждать.
   * Возвращает {reply, lines}.
   */
  async _finish(last, expect) {
    let reply = '';
    try {
      reply = await this._send(last);
    } finally {
      this.mode = 'text';
      this.collect = [];
    }
    this.lastLineAt = Date.now();
    await this.port.write(enc.encode('\n'));
    const lines = this.collect;
    const started = Date.now();
    await new Promise((resolve) => {
      const check = () => {
        const results = lines.map(parseResult).filter(Boolean);
        const done =
          !this.connected ||
          results.some((r) => !r.ok) ||
          (expect > 0 && results.length >= expect) ||
          (expect === 0 && Date.now() - this.lastLineAt >= this.t.quiet) ||
          Date.now() - started >= this.t.settle;
        if (done) resolve();
        else setTimeout(check, 20);
      };
      check();
    });
    this.collect = null;
    return { reply, lines };
  }

  // Сеанс целиком: busy; при любой ошибке — завершить сеанс программатора. Вышел срок — закрыть порт:
  // программатор увидит падение DTR. Другая ошибка в режиме gdb — D и перевод строки, как обычно.
  async _session(body) {
    if (!this.connected) throw new Error('программатор не подключён');
    if (this.busy) throw new Error('идёт сеанс');
    this.busy = true;
    this._changed();
    try {
      return await body();
    } catch (e) {
      if (e instanceof Timeout) await this._drop();
      else if (this.mode === 'gdb' && this.connected) await this._finish('D', 0).catch(() => this._drop());
      throw e;
    } finally {
      this.mode = 'text';
      this.collect = null;
      this.busy = false;
      this._changed();
    }
  }

  async _drop() {
    this.mode = 'text';
    await this.disconnect().catch(() => {});
  }

  // ---------------------------------------------------------------- сеансы

  /** Карта памяти выбранного чипа — без подключения к плате. */
  async memoryMap() {
    return this._session(async () => {
      await this._hello();
      await this._send('!');
      const map = await this._memoryMap();
      await this._finish('D', 0);
      return map;
    });
  }

  /**
   * Прошить файлы одним сеансом (design §5).
   * @param {{name: string, text: string}[]} files  Intel HEX
   * @param {object} [opts]
   * @param {boolean} [opts.eraseAll]  полное стирание основной flash перед записью
   * @param {(p: {phase: string, sent: number, total: number}) => void} [opts.onProgress]
   * @returns {Promise<{ok: boolean, reason: string, lines: string[], touched: boolean}>}
   *   touched — программатор подключался к плате.
   */
  async flash(files, opts = {}) {
    const progress = opts.onProgress || (() => {});
    if (!files.length) return { ok: false, reason: 'не выбран ни один файл', lines: [], touched: false };
    let parsed;
    try {
      parsed = files.map((f) => parseFile(f.name, f.text)); // ошибки разбора — до сеанса
    } catch (e) {
      return { ok: false, reason: e.message, lines: [], touched: false };
    }
    return this._session(async () => {
      await this._hello();
      await this._send('!');
      const map = await this._memoryMap();
      let img;
      try {
        img = buildImage(parsed, map);
      } catch (e) {
        if (!(e instanceof ImageError)) throw e;
        await this._finish('D', 0); // к плате программатор не подключался
        return { ok: false, reason: e.message, lines: [], touched: false };
      }
      const chunks = writeChunks(img.runs, WRITE_LIMIT, needsEscape);
      const total = img.size;
      progress({ phase: 'connect', sent: 0, total });
      if (!(await this._attach())) {
        const end = await this._finish('D', 0);
        return this._result(false, end.lines, 'чип не отвечает', true);
      }
      let expect = 0;
      let failed = '';
      if (opts.eraseAll) {
        progress({ phase: 'erase', sent: 0, total });
        this.console = [];
        await this._monitor('erase', this.t.erase);
        expect++;
        const r = this.console.map(parseResult).find(Boolean);
        if (!r || !r.ok) failed = r ? r.text : 'полное стирание: нет итога';
      }
      if (!failed) {
        for (const b of img.erase) {
          const r = await this._send(`vFlashErase:${b.addr.toString(16)},${b.len.toString(16)}`);
          if (r !== 'OK') {
            failed = 'программатор отверг образ';
            break;
          }
        }
      }
      let sent = 0;
      if (!failed) {
        progress({ phase: 'write', sent, total });
        for (const c of chunks) {
          const r = await this._send(framePacket(`vFlashWrite:${c.addr.toString(16)}:`, c.bytes));
          if (r !== 'OK') {
            failed = 'программатор отверг образ';
            break;
          }
          sent += c.bytes.length;
          progress({ phase: 'write', sent, total });
        }
      }
      if (!failed) {
        progress({ phase: 'done', sent, total });
        expect++;
        const r = await this._send('vFlashDone', this.t.done);
        if (r !== 'OK') failed = 'запись не удалась';
      }
      const end = await this._finish('vKill;1', expect);
      return this._result(end.reply === 'OK' && !failed, end.lines, failed || 'запись не удалась', true);
    });
  }

  // Итог страницы — по строкам «# итог:» программатора; своя причина — если их нет.
  _result(okByPackets, lines, fallback, touched) {
    const results = lines.map(parseResult).filter(Boolean);
    const bad = results.filter((r) => !r.ok);
    const ok = okByPackets && bad.length === 0;
    let reason = '';
    if (!ok) {
      if (bad.length) reason = bad[bad.length - 1].text;
      else {
        const own = lines.filter((l) => l.startsWith('# ') && !l.startsWith('# сеанс:'));
        reason = own.length && !okByPackets ? own[own.length - 1].slice(2) : fallback;
      }
    }
    return { ok, reason, lines, touched };
  }

  /**
   * Прочитать память чипа (design §7): подключиться, m кусками по 0x400 Б, D.
   * @returns {Promise<{ok: boolean, bytes?: Uint8Array, reason: string, lines: string[]}>}
   */
  async read(addr, len) {
    if (!(len > 0)) return { ok: false, reason: 'длина должна быть больше нуля', lines: [] };
    return this._session(async () => {
      await this._hello();
      if (!(await this._attach())) {
        const end = await this._finish('D', 0);
        return { ...this._result(false, end.lines, 'чип не отвечает', true), bytes: undefined };
      }
      const out = new Uint8Array(len);
      let reason = '';
      for (let off = 0; off < len; off += READ_CHUNK) {
        const n = Math.min(READ_CHUNK, len - off);
        const a = addr + off;
        const r = await this._send(`m${a.toString(16)},${n.toString(16)}`);
        const bytes = fromHex(r);
        if (!bytes || bytes.length !== n) {
          reason = `чтение не удалось: ${hex32(a)}`;
          break;
        }
        out.set(bytes, off);
      }
      const end = await this._finish('D', 0);
      return reason ? { ok: false, reason, lines: end.lines } : { ok: true, bytes: out, reason: '', lines: end.lines };
    });
  }
}
