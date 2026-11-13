// Экран страницы прошивки: кнопки, поля, терминал, ход записи; точка сборки (design §3, §6, §11;
// jaffa-flasher-page design §2, §5–§9).
// Кадров протокола не знает: разговор с программатором — lib/flasher.js, порт — lib/webserial.js.

import { Flasher, hexDump } from './lib/flasher.js';
import { serialAvailable, choosePort, WebSerialPort } from './lib/webserial.js';
import { parseFile, hex32, fileTags } from './lib/image.js';
import { SPEEDS, DATA_BITS, PARITY, STOP_BITS, DEFAULT_UART, baudCommand, terminalControls, LogView, stamp } from './lib/terminal.js';

// Спонсор — одна ссылка для шапки и подвала (jaffa-flasher-page design §2).
const SPONSOR_URL = 'https://waterius.ru/?utm_source=jaffa_flasher&utm_medium=web&utm_campaign=jaffa_flasher';
const MAKE_URL = 'https://github.com/waterius/jaffa_flasher#сборка-и-заливка-программатора';

// Все надписи — здесь, чтобы позже добавить английский (design §11).
const T = {
  sponsoredBy: 'Sponsored by',
  title: 'Прошивка через Jaffa Flasher',
  lead: 'Программатор Jaffa Flasher на МК ESP32-S3 для прошивки МК PY32.',
  noSerial: 'Нужен Chrome или Edge на компьютере.',
  noSerialFile: 'Нужен Chrome или Edge на компьютере, а страница — открытая по ссылке https:// или с localhost, не из файла.',
  secMake: 'Как сделать программатор',
  makeText: 'Нужны плата ESP32-S3 Super Mini и кабель USB. Прошивка программатора собирается в PlatformIO и заливается по USB самой платы, одной командой.',
  makeLink: 'Инструкция: сборка и заливка программатора',
  secWiring: 'Подключение',
  wiringHint: 'Плата с PY32 питается от 3,0–3,6 В. Сигнальные провода — через резисторы 100–330 Ом. 3V3 программатора подключайте, только если у платы нет своего питания.',
  secFlash: 'Прошивка',
  step1: 'Подключите Jaffa Flasher к USB',
  step2: 'Нажмите «Подключить»',
  step3: 'Выберите файлы',
  step4: 'Нажмите «Прошить»',
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
  chooseFiles: 'Выбрать файлы…',
  filesHint: 'Intel HEX: прошивка и, если нужно, файл для USER OTP. Адреса записи — из самих файлов. Файлы не покидают браузер.',
  noFiles: 'файлы не выбраны',
  eraseAll: 'Стереть всю основную flash перед записью (USER OTP не стирается)',
  verify: 'Проверить прошивку после записи — программатор сверяет каждую записанную страницу',
  flash: 'Прошить',
  phaseConnect: 'подключение к плате…',
  phaseErase: 'полное стирание…',
  phaseWrite: (sent, total) => `запись: ${sent} из ${total} Б`,
  phaseDone: 'стирание, запись и сверка страниц…',
  resultOk: 'Итог: ок',
  resultBad: (reason) => `Итог: ошибка — ${reason}`,
  secMemory: 'Память',
  clearScreen: 'Очистить экран',
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
  secLog: 'Терминал',
  logOn: 'Включить лог',
  logOff: 'Выключить лог',
  uartSpeed: 'Скорость',
  uartBits: 'Биты',
  uartParity: 'Чётность',
  uartStop: 'Стоп',
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
  dumpClear: $('dump-clear'),
  logToggle: $('log-toggle'),
  uartSpeed: $('uart-speed'),
  uartBits: $('uart-bits'),
  uartParity: $('uart-parity'),
  uartStop: $('uart-stop'),
};
const UART = [el.uartSpeed, el.uartBits, el.uartParity, el.uartStop];

let flasher = null;
let state = { connected: false, busy: false, chip: null, supported: [], regions: [], map: null };
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
for (const id of ['sponsor-top', 'sponsor-foot']) $(id).href = SPONSOR_URL;
$('make-link').href = MAKE_URL;

const options = (select, items) => select.replaceChildren(...items.map(([v, t]) => new Option(t, String(v))));
options(el.uartSpeed, SPEEDS.map((v) => [v, String(v)]));
options(el.uartBits, DATA_BITS.map((v) => [v, String(v)]));
options(el.uartParity, PARITY);
options(el.uartStop, STOP_BITS.map((v) => [v, String(v)]));
el.uartSpeed.value = String(DEFAULT_UART.speed);
el.uartBits.value = String(DEFAULT_UART.bits);
el.uartParity.value = DEFAULT_UART.parity;
el.uartStop.value = String(DEFAULT_UART.stop);

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
  const term = terminalControls({ connected: on, busy: state.busy, working });
  for (const s of UART) s.disabled = !term.uart;
  el.logToggle.disabled = !term.logToggle;
  el.logToggle.textContent = logView.on ? T.logOff : T.logOn;
  el.cmd.disabled = el.send.disabled = !term.command;
  renderRegions();
  renderFiles();
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
  div.textContent = stamp(new Date()) + text;
  if (cls) div.className = cls;
  el.log.append(div);
  while (el.log.childElementCount > LOG_LINES) el.log.firstElementChild.remove();
  if (stick) el.log.scrollTop = el.log.scrollHeight;
}

// Терминал: выключенный лог строки не выводит и не копит (jaffa-flasher-page design §8).
const logView = new LogView(logLine);

el.clear.addEventListener('click', () => el.log.replaceChildren());
el.logToggle.addEventListener('click', () => {
  logView.toggle();
  render();
});

// Формат лога: любой из четырёх списков — одна команда baud (jaffa-flasher-page design §7).
for (const s of UART) {
  s.addEventListener('change', () =>
    command(
      baudCommand({
        speed: el.uartSpeed.value,
        bits: el.uartBits.value,
        parity: el.uartParity.value,
        stop: el.uartStop.value,
      }),
    ),
  );
}

// ---------------------------------------------------------------- подключение

async function connect() {
  const serial = await choosePort();
  if (!serial) return;
  working = true;
  setStatus(T.connecting);
  render();
  const port = new WebSerialPort(serial);
  flasher = new Flasher(port, {
    onLine: (line, own) => logView.show(line, own ? 'own' : ''),
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
  logView.show(`> ${line}`, 'cmd');
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
  // Метки: неосновная область из ответа chip и пересечение с файлами выше (design §5).
  const tags = fileTags(files, state.regions);
  el.fileList.replaceChildren(
    ...files.map((f, i) => {
      const li = document.createElement('li');
      if (f.error) {
        li.textContent = f.error;
        li.className = 'bad';
        return li;
      }
      const name = Object.assign(document.createElement('span'), { className: 'fname', textContent: f.name });
      const ranges = f.parsed.ranges.map((r) => `${hex32(r.start)}–${hex32(r.end - 1)}`).join(', ');
      const where = Object.assign(document.createElement('span'), {
        className: 'franges',
        textContent: `${ranges}, ${f.parsed.size} Б`,
      });
      li.append(name, where);
      for (const t of tags[i]) {
        li.append(Object.assign(document.createElement('span'), { className: `tag ${t.kind}`, textContent: t.text }));
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

el.dumpClear.addEventListener('click', () => {
  el.dump.textContent = '';
  el.dump.hidden = true;
});

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
  for (const id of ['sec-flash', 'sec-memory', 'sec-log']) $(id).hidden = true;
} else {
  renderFiles();
  render();
}
