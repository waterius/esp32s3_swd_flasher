// Порт оснастки на Web Serial (design §4): выбор порта с VID Espressif, открытие на 115 200 с
// поднятыми DTR и RTS, чтение и запись потоками, закрытие с отпусканием потоков. Протокол gdb
// не знает — его ведёт flasher.js.
//
// Линии DTR и RTS: открытие поднимает обе сразу — переход (0,0) → (1,1), закрытие опускает обе
// сразу. Такие переходы не перезагружают оснастку в загрузчик ESP (Arduino-ESP32, USBCDC.cpp);
// скорость 1 200 и другие сочетания линий страница не задаёт.

export const ESPRESSIF_VID = 0x303a;
const BAUD = 115200;

/** Есть ли Web Serial (Chrome и Edge на компьютере, HTTPS или localhost). */
export function serialAvailable() {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

/** Спросить человека порт оснастки. Отказ человека — null. */
export async function choosePort() {
  try {
    return await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });
  } catch (e) {
    if (e && e.name === 'NotFoundError') return null; // человек закрыл окно выбора
    throw e;
  }
}

/** Порт оснастки поверх SerialPort — реализация протокола из flasher.js. */
export class WebSerialPort {
  constructor(serial) {
    this.serial = serial;
    this.opened = false;
    this.closing = false;
    this.reader = null;
    this.writer = null;
    this.loop = null;
    this._data = () => {};
    this._close = () => {};
  }

  onData(cb) {
    this._data = cb;
  }

  onClose(cb) {
    this._close = cb;
  }

  async open() {
    if (this.opened) throw new Error('порт уже открыт');
    try {
      await this.serial.open({ baudRate: BAUD, bufferSize: 8192 });
    } catch (e) {
      // NetworkError — порт держит другая программа; InvalidStateError — уже открыт здесь
      if (e && e.name === 'InvalidStateError') throw new Error('порт уже открыт');
      throw new Error('порт занят: закройте монитор и gdb');
    }
    this.opened = true;
    this.closing = false;
    try {
      await this.serial.setSignals({ dataTerminalReady: true, requestToSend: true });
      this.writer = this.serial.writable.getWriter();
    } catch (e) {
      await this.close().catch(() => {});
      throw e;
    }
    this.loop = this._readLoop();
  }

  async _readLoop() {
    while (this.serial.readable && !this.closing) {
      this.reader = this.serial.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value && value.length) this._data(value);
        }
      } catch (e) {
        // переполнение и ошибки кадра — поток пересоздаётся, читаем дальше; пропажа порта —
        // readable станет null, и цикл кончится
      } finally {
        try {
          this.reader.releaseLock();
        } catch (e) {
          /* уже отпущен */
        }
        this.reader = null;
      }
    }
    if (!this.closing && this.opened) {
      this.opened = false;
      try {
        this.writer && this.writer.releaseLock();
      } catch (e) {
        /* поток уже закрыт */
      }
      this.writer = null;
      this._close();
    }
  }

  async write(bytes) {
    if (!this.opened || !this.writer) throw new Error('порт закрыт');
    await this.writer.write(bytes);
  }

  async close() {
    if (!this.opened) return;
    this.closing = true;
    try {
      if (this.reader) await this.reader.cancel();
    } catch (e) {
      /* уже закрыт */
    }
    if (this.loop) await this.loop;
    try {
      if (this.writer) this.writer.releaseLock();
    } catch (e) {
      /* уже отпущен */
    }
    this.writer = null;
    try {
      await this.serial.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch (e) {
      /* порт уже пропал */
    }
    try {
      await this.serial.close();
    } finally {
      this.opened = false;
      this.closing = false;
    }
  }
}
