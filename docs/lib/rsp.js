// Кадры протокола gdb (GDB Remote Serial Protocol): $данные#xx, сумма, экранирование
// #, $, }, * и hex. Смысла пакетов не знает — это flasher.js (design §3).

const ENC = new TextEncoder();

/** Байты из строки (UTF-8) или как есть. */
export function bytesOf(data) {
  return typeof data === 'string' ? ENC.encode(data) : data;
}

/** Сумма байтов по модулю 256. */
export function checksum(bytes) {
  let sum = 0;
  for (const b of bytes) sum = (sum + b) & 0xff;
  return sum;
}

/** Байт, который в двоичных данных пакета экранируется. */
export function needsEscape(b) {
  return b === 0x23 || b === 0x24 || b === 0x7d || b === 0x2a; // # $ } *
}

/** Длина данных после экранирования. */
export function escapedLength(bytes) {
  let n = bytes.length;
  for (const b of bytes) if (needsEscape(b)) n++;
  return n;
}

/** Экранирование двоичных данных: } и байт XOR 0x20. */
export function escape(bytes) {
  const out = new Uint8Array(escapedLength(bytes));
  let k = 0;
  for (const b of bytes) {
    if (needsEscape(b)) {
      out[k++] = 0x7d;
      out[k++] = b ^ 0x20;
    } else {
      out[k++] = b;
    }
  }
  return out;
}

const HEX = '0123456789abcdef';

/** Кадр $payload#xx; payload — строка или байты, экранируется целиком. */
export function frame(payload) {
  const body = escape(bytesOf(payload));
  const sum = checksum(body);
  const out = new Uint8Array(body.length + 4);
  out[0] = 0x24;
  out.set(body, 1);
  out[body.length + 1] = 0x23;
  out[body.length + 2] = HEX.charCodeAt(sum >> 4);
  out[body.length + 3] = HEX.charCodeAt(sum & 15);
  return out;
}

/** Кадр из уже собранных частей: заголовок-строка и двоичные данные. */
export function framePacket(head, data) {
  const h = bytesOf(head);
  const all = new Uint8Array(h.length + data.length);
  all.set(h, 0);
  all.set(data, h.length);
  return frame(all);
}

/** Байты → hex строчными. */
export function toHex(bytes) {
  let s = '';
  for (const b of bytes) s += HEX[b >> 4] + HEX[b & 15];
  return s;
}

/** hex (чётная длина, любой регистр) → байты или null. */
export function fromHex(text) {
  if (text.length % 2 || /[^0-9a-fA-F]/.test(text)) return null;
  const out = new Uint8Array(text.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(text.substr(2 * i, 2), 16);
  return out;
}

function hexDigit(b) {
  if (b >= 0x30 && b <= 0x39) return b - 0x30;
  if (b >= 0x61 && b <= 0x66) return b - 0x61 + 10;
  if (b >= 0x41 && b <= 0x46) return b - 0x41 + 10;
  return -1;
}

/**
 * Разбор входящих байтов. feed(byte) возвращает:
 *   {type: 'packet', payload: Uint8Array} — пакет с верной суммой, экранирование снято;
 *   {type: 'bad'} — пакет с неверной суммой;
 *   {type: 'other', byte} — байт вне пакета (лог, подтверждения);
 *   null — байт принят, события нет.
 * Новый «$» посреди пакета начинает пакет заново: так находится верный пакет среди лишних байтов.
 */
export class Parser {
  constructor() {
    this.reset();
  }

  reset() {
    this.state = 'idle';
    this.buf = [];
    this.sum = 0;
    this.got = 0;
  }

  feed(b) {
    switch (this.state) {
      case 'idle':
        if (b === 0x24) {
          this.state = 'data';
          this.buf = [];
          this.sum = 0;
          return null;
        }
        return { type: 'other', byte: b };
      case 'data':
      case 'escape':
        if (b === 0x24) {
          this.state = 'data';
          this.buf = [];
          this.sum = 0;
          return null;
        }
        if (this.state === 'data' && b === 0x23) {
          this.state = 'sum1';
          return null;
        }
        this.sum = (this.sum + b) & 0xff;
        if (this.state === 'data' && b === 0x7d) {
          this.state = 'escape';
          return null;
        }
        this.buf.push(this.state === 'escape' ? b ^ 0x20 : b);
        this.state = 'data';
        return null;
      case 'sum1': {
        const d = hexDigit(b);
        if (d < 0) {
          this.state = 'idle';
          return { type: 'bad' };
        }
        this.got = d << 4;
        this.state = 'sum2';
        return null;
      }
      case 'sum2': {
        const d = hexDigit(b);
        this.state = 'idle';
        if (d < 0 || (this.got | d) !== this.sum) return { type: 'bad' };
        return { type: 'packet', payload: Uint8Array.from(this.buf) };
      }
    }
    return null;
  }
}
