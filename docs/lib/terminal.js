// Терминал страницы без DOM: команда формата лога и что доступно на экране (jaffa-flasher-page
// design §7, §9). Чистый код — тесты в test/web/terminal.test.mjs.

export const SPEEDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
export const DATA_BITS = [8, 7, 6, 5];
export const PARITY = [
  ['N', 'нет'],
  ['E', 'чёт'],
  ['O', 'нечёт'],
];
export const STOP_BITS = [1, 2];
export const DEFAULT_UART = { speed: 115200, bits: 8, parity: 'N', stop: 1 };

/** Команда программатору: {speed: 9600, bits: 8, parity: 'E', stop: 1} → «baud 9600 8E1». */
export function baudCommand({ speed, bits, parity, stop }) {
  return `baud ${speed} ${bits}${parity}${stop}`;
}

/**
 * Что доступно: настройки UART, кнопка лога и ввод команды — только при подключённом порте и без
 * сеанса; «Очистить экран» — всегда.
 */
export function terminalControls({ connected, busy, working }) {
  const free = Boolean(connected && !busy && !working);
  return { uart: free, logToggle: free, command: free, clear: true };
}

/**
 * Метка времени строки терминала по часам ПК (у платы часов нет, log-timestamps design §1):
 * «2026-11-13 14-05-33.123: ». Местное время — то, что у человека на часах.
 */
export function stamp(date) {
  const p = (n, w = 2) => String(n).padStart(w, '0');
  return (
    `${date.getFullYear()}-${p(date.getMonth() + 1)}-${p(date.getDate())} ` +
    `${p(date.getHours())}-${p(date.getMinutes())}-${p(date.getSeconds())}.${p(date.getMilliseconds(), 3)}: `
  );
}

/**
 * Лог на экране: пока выключен, строки не выводятся и не копятся (design §8).
 * show(line) вызывает вывод только при включённом логе.
 */
export class LogView {
  constructor(print) {
    this.print = print;
    this.on = true;
  }
  toggle() {
    this.on = !this.on;
    return this.on;
  }
  show(line, cls) {
    if (this.on) this.print(line, cls);
  }
}
