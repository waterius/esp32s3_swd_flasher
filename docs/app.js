// Экран страницы прошивки: кнопки, поля, лог, ход записи; точка сборки (design §3, §6, §11).
// Кадров протокола не знает: разговор с программатором — lib/flasher.js, порт — lib/webserial.js.

import { Flasher, hexDump } from './lib/flasher.js';
import { serialAvailable, choosePort, WebSerialPort } from './lib/webserial.js';
import { parseFile, hex32 } from './lib/image.js';

// Все надписи — здесь, чтобы позже добавить английский (design §11).
const T = {
  title: 'Прошивка через программатор Jaffa Flasher',
  lead: 'Программатор Jaffa Flasher на ESP32-S3 пишет микроконтроллер на плате по SWD. Страница говорит с ним через USB-порт: выберите файлы Intel HEX и нажмите «Прошить».',
  noSerial: 'Нужен Chrome или Edge на компьютере.',
  noSerialFile: 'Нужен Chrome или Edge на компьютере, а страница — открытая по ссылке https:// или с localhost, не из файла.',
  secConnect: 'Программатор Jaffa Flasher',
  connect: 'Подключить',
  disconnect: 'Отключить',
  connecting: 'подключение…',
  notConnected: 'не подключён',
  connected: 'подключён',
  lost: 'программатор отключён',
  chipLabel: 'Чип:',
  chipNone: 'не выбран',
  chipSelect: 'Выбрать чип',
  chipSet: 'Выбрать',
  secFlash: 'Прошивка',
  chooseFiles: 'Выбрать файлы…',
  filesHint: 'Intel HEX: прошивка и, если нужно, файл для USER OTP. Файлы не покидают браузер.',
  noFiles: 'файлы не выбраны',
  eraseAll: 'Стереть всю основную flash перед записью (USER OTP не стирается)',
  flash: 'Прошить',
  phaseConnect: 'подключение к плате…',
  phaseErase: 'полное стирание…',
  phaseWrite: (sent, total) => `запись: ${sent} из ${total} Б`,
  phaseDone: 'стирание и запись страниц…',
  resultOk: 'Итог: ок',
  resultBad: (reason) => `Итог: ошибка — ${reason}`,
  secMemory: 'Память',
  region: 'Область',
  regionManual: 'адрес и длина',
  regionFlash: (r) => `flash ${hex32(r.start)}, ${r.length} Б`,
  regionRam: (r) => `ОЗУ ${hex32(r.start)}, ${r.length} Б`,
  address: 'Адрес',
  length: 'Длина, Б',
  read: 'Прочитать',
  readHint: 'Чтение — короткий сеанс SWD: плата перезапустится. Адрес — hex, длина — десятичная или с 0x.',
  badAddress: 'адрес — hex, например 0x08000000',
  badLength: 'длина — число байт больше нуля',
  reading: 'чтение…',
  secLog: 'Лог и команды',
  clear: 'Очистить',
  cmdLabel: 'Команда программатора',
  send: 'Отправить',
  cmdHint: 'Команды: help, chip, info, erase, reset, baud. Строки «# » — от программатора, остальное — лог платы.',
  privacy: 'Страница работает в браузере: файлы и прочитанная память никуда не отправляются.',
  repo: 'Исходники и инструкция — github.com/waterius/jaffa_flasher',
  busy: 'идёт сеанс',
};

const LOG_LINES = 5000;

const $ = (id) => document.getElementById(id);
const el = {
  noSerial: $('no-serial'),
  connect: $('connect'),
  status: $('status'),
  chip: $('chip'),
  chipSelect: $('chip-select'),
  chipSet: $('chip-set'),
  files: $('files'),
  fileList: $('file-list'),
  eraseAll: $('erase-all'),
  flash: $('flash'),
  progress: $('progress'),
  phase: $('phase'),
  result: $('result'),
  resultHead: $('result-head'),
  resultLines: $('result-lines'),
  region: $('region'),
  addr: $('addr'),
  len: $('len'),
  read: $('read'),
  dump: $('dump'),
  log: $('log'),
  cmdForm: $('cmd-form'),
  cmd: $('cmd'),
  send: $('send'),
  clear: $('clear'),
};

let flasher = null;
let state = { connected: false, busy: false, chip: null, supported: [], map: null };
let files = []; // [{name, text, parsed?, error?}]
let working = false; // подключение или сеанс, начатый со страницы
let mapLoading = false;

// ---------------------------------------------------------------- надписи и состояние

for (const node of document.querySelectorAll('[data-t]')) {
  const v = T[node.dataset.t];
  if (typeof v === 'string') node.textContent = v;
}
el.connect.textContent = T.connect;
el.status.textContent = T.notConnected;

function render() {
  const on = state.connected;
  const free = on && !state.busy && !working;
  el.connect.textContent = on ? T.disconnect : T.connect;
  el.connect.disabled = working && !on;
  el.chip.textContent = on ? state.chip || T.chipNone : '—';
  const names = state.supported || [];
  if ([...el.chipSelect.options].map((o) => o.value).join() !== names.join()) {
    el.chipSelect.replaceChildren(...names.map((n) => new Option(n, n)));
  }
  if (state.chip && names.includes(state.chip)) el.chipSelect.value = state.chip;
  el.chipSelect.disabled = el.chipSet.disabled = !free || !names.length;
  const goodFiles = files.length && files.every((f) => f.parsed);
  el.flash.disabled = !free || !state.chip || !goodFiles;
  el.read.disabled = el.region.disabled = !free || !state.chip;
  el.cmd.disabled = el.send.disabled = !free;
  renderRegions();
}

function renderRegions() {
  const map = state.map || [];
  const key = map.map((r) => `${r.type}${r.start}`).join();
  if (el.region.dataset.key === key && el.region.options.length) return;
  el.region.dataset.key = key;
  const opts = [new Option(T.regionManual, '')];
  for (const r of map) {
    const o = new Option(r.type === 'ram' ? T.regionRam(r) : T.regionFlash(r), `${r.start}:${r.length}`);
    opts.push(o);
  }
  el.region.replaceChildren(...opts);
}

function setStatus(text, bad = false) {
  el.status.textContent = text;
  el.status.classList.toggle('bad', bad);
}

// ---------------------------------------------------------------- лог

function logLine(text, cls) {
  const stick = el.log.scrollTop + el.log.clientHeight >= el.log.scrollHeight - 4;
  const div = document.createElement('div');
  div.textContent = text;
  if (cls) div.className = cls;
  el.log.append(div);
  while (el.log.childElementCount > LOG_LINES) el.log.firstElementChild.remove();
  if (stick) el.log.scrollTop = el.log.scrollHeight;
}

el.clear.addEventListener('click', () => el.log.replaceChildren());

// ---------------------------------------------------------------- подключение

async function connect() {
  const serial = await choosePort();
  if (!serial) return;
  working = true;
  setStatus(T.connecting);
  render();
  const port = new WebSerialPort(serial);
  flasher = new Flasher(port, {
    onLine: (line, own) => logLine(line, own ? 'own' : ''),
    onState: (s) => {
      const wasOn = state.connected;
      state = s;
      if (wasOn && !s.connected) setStatus(T.lost, true);
      render();
      loadMapSoon();
    },
  });
  try {
    await flasher.connect();
    setStatus(T.connected);
  } catch (e) {
    setStatus(e.message, true);
    flasher = null;
  } finally {
    working = false;
    render();
    loadMapSoon();
  }
}

async function disconnect() {
  if (!flasher) return;
  await flasher.disconnect().catch(() => {});
  setStatus(T.notConnected);
  render();
}

el.connect.addEventListener('click', () => (state.connected ? disconnect() : connect()));

el.chipSet.addEventListener('click', () => command(`chip ${el.chipSelect.value}`));

// Карта памяти — для списка областей; без подключения к плате.
function loadMapSoon() {
  if (!flasher || mapLoading || !state.connected || state.busy || working || !state.chip || state.map) return;
  mapLoading = true;
  setTimeout(async () => {
    try {
      if (flasher && state.connected && !state.busy && !working && state.chip && !state.map) await flasher.memoryMap();
    } catch (e) {
      logLine(e.message, 'err');
    } finally {
      mapLoading = false;
      render();
    }
  }, 300);
}

// ---------------------------------------------------------------- команды

async function command(line) {
  if (!flasher || !line) return;
  logLine(`> ${line}`, 'cmd');
  try {
    await flasher.command(line);
  } catch (e) {
    logLine(e.message, 'err');
  }
}

el.cmdForm.addEventListener('submit', (ev) => {
  ev.preventDefault();
  const line = el.cmd.value.trim();
  el.cmd.value = '';
  command(line);
});

// ---------------------------------------------------------------- файлы и прошивка

el.files.addEventListener('change', async () => {
  files = [];
  for (const f of el.files.files) {
    const item = { name: f.name, text: await f.text() };
    try {
      item.parsed = parseFile(item.name, item.text);
    } catch (e) {
      item.error = e.message;
    }
    files.push(item);
  }
  renderFiles();
  render();
});

function renderFiles() {
  if (!files.length) {
    el.fileList.replaceChildren(Object.assign(document.createElement('li'), { textContent: T.noFiles }));
    return;
  }
  el.fileList.replaceChildren(
    ...files.map((f) => {
      const li = document.createElement('li');
      if (f.error) {
        li.textContent = f.error;
        li.className = 'bad';
      } else {
        const ranges = f.parsed.ranges.map((r) => `${hex32(r.start)}–${hex32(r.end - 1)}`).join(', ');
        li.textContent = `${f.name}: ${ranges}, ${f.parsed.size} Б`;
      }
      return li;
    }),
  );
}

function showResult(ok, reason, lines) {
  el.result.hidden = false;
  el.resultHead.textContent = ok ? T.resultOk : T.resultBad(reason);
  el.resultHead.className = 'result-head ' + (ok ? 'ok' : 'bad');
  el.resultLines.textContent = (lines || []).join('\n');
}

const PHASE = {
  connect: () => T.phaseConnect,
  erase: () => T.phaseErase,
  write: (p) => T.phaseWrite(p.sent, p.total),
  done: () => T.phaseDone,
};

el.flash.addEventListener('click', async () => {
  if (!flasher) return;
  working = true;
  el.result.hidden = true;
  el.progress.hidden = false;
  el.progress.value = 0;
  el.phase.textContent = '';
  render();
  try {
    const r = await flasher.flash(
      files.map((f) => ({ name: f.name, text: f.text })),
      {
        eraseAll: el.eraseAll.checked,
        onProgress: (p) => {
          el.progress.value = p.total ? p.sent / p.total : 0;
          el.phase.textContent = PHASE[p.phase] ? PHASE[p.phase](p) : '';
        },
      },
    );
    showResult(r.ok, r.reason, r.lines);
  } catch (e) {
    showResult(false, e.message, []);
  } finally {
    working = false;
    el.progress.hidden = true;
    el.phase.textContent = '';
    render();
  }
});

// ---------------------------------------------------------------- память

el.region.addEventListener('change', () => {
  if (!el.region.value) return;
  const [start, length] = el.region.value.split(':').map(Number);
  el.addr.value = hex32(start);
  el.len.value = String(length);
});

function parseAddr(text) {
  const t = text.trim().replace(/^0x/i, '');
  return /^[0-9a-f]{1,8}$/i.test(t) ? parseInt(t, 16) : NaN;
}

function parseLen(text) {
  const t = text.trim();
  const v = /^0x[0-9a-f]+$/i.test(t) ? parseInt(t, 16) : /^\d+$/.test(t) ? parseInt(t, 10) : NaN;
  return v > 0 ? v : NaN;
}

el.read.addEventListener('click', async () => {
  if (!flasher) return;
  const addr = parseAddr(el.addr.value);
  const len = parseLen(el.len.value);
  el.dump.hidden = false;
  if (Number.isNaN(addr)) return void (el.dump.textContent = T.badAddress);
  if (Number.isNaN(len)) return void (el.dump.textContent = T.badLength);
  working = true;
  el.dump.textContent = T.reading;
  render();
  try {
    const r = await flasher.read(addr, len);
    el.dump.textContent = r.ok ? hexDump(addr, r.bytes).join('\n') : r.reason;
  } catch (e) {
    el.dump.textContent = e.message;
  } finally {
    working = false;
    render();
  }
});

// ---------------------------------------------------------------- старт

if (!serialAvailable()) {
  el.noSerial.textContent = location.protocol === 'file:' ? T.noSerialFile : T.noSerial;
  el.noSerial.hidden = false;
  for (const id of ['sec-connect', 'sec-flash', 'sec-memory', 'sec-log']) $(id).hidden = true;
} else {
  renderFiles();
  render();
}
