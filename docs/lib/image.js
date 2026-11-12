// Образ из нескольких файлов Intel HEX: противоречия между файлами, проверка по карте памяти
// программатора, блоки для vFlashErase и куски для vFlashWrite (design §3, §5). Порт не знает.

import { parseIhex } from './ihex.js';

/** Адрес для человека: 0x08000010. */
export function hex32(addr) {
  return '0x' + (addr >>> 0).toString(16).toUpperCase().padStart(8, '0');
}

/** Ошибка образа: текст для человека уже с именами файлов. */
export class ImageError extends Error {}

/**
 * Разобрать один файл. Возвращает {name, chunks, size, ranges: [{start, end}]} — end не входит.
 * Бросает ImageError с именем файла.
 */
export function parseFile(name, text) {
  let chunks;
  try {
    chunks = parseIhex(text);
  } catch (e) {
    throw new ImageError(`${name}: ${e.message}`);
  }
  let size = 0;
  for (const c of chunks) size += c.bytes.length;
  return { name, chunks, size, ranges: ranges(chunks) };
}

// Сплошные отрезки адресов файла, по возрастанию.
function ranges(chunks) {
  const sorted = chunks.map((c) => [c.addr, c.addr + c.bytes.length]).sort((a, b) => a[0] - b[0]);
  const out = [];
  for (const [s, e] of sorted) {
    const last = out[out.length - 1];
    if (last && s <= last.end) last.end = Math.max(last.end, e);
    else out.push({ start: s, end: e });
  }
  return out;
}

/**
 * Карта памяти gdb (qXfer:memory-map) → [{type: 'flash'|'ram', start, length, blocksize}].
 */
export function parseMemoryMap(xml) {
  const out = [];
  const re = /<memory\s+([^>]*?)\/?>(?:([\s\S]*?)<\/memory>)?/g;
  let m;
  while ((m = re.exec(xml))) {
    const attr = (name) => {
      const a = new RegExp(`${name}="([^"]*)"`).exec(m[1]);
      return a ? a[1] : null;
    };
    const region = {
      type: attr('type'),
      start: Number(attr('start')),
      length: Number(attr('length')),
      blocksize: 0,
    };
    const bs = m[2] && /<property\s+name="blocksize">([^<]*)<\/property>/.exec(m[2]);
    if (bs) region.blocksize = Number(bs[1].trim());
    if (region.type && Number.isFinite(region.start) && Number.isFinite(region.length)) out.push(region);
  }
  return out;
}

function flashRegionOf(map, addr) {
  return map.find((r) => r.type === 'flash' && r.blocksize > 0 && addr >= r.start && addr < r.start + r.length);
}

/**
 * Собрать образ из разобранных файлов (parseFile) под карту памяти.
 * Порядок отказов: адрес вне карты (первый такой байт файла), затем противоречие между файлами.
 * Возвращает {size, erase: [{addr, len}], runs: [{addr, bytes}]}. Бросает ImageError.
 */
export function buildImage(files, map) {
  // 1. Все байты — в областях записи карты.
  for (const f of files) {
    for (const r of f.ranges) {
      for (let a = r.start; a < r.end; ) {
        const reg = flashRegionOf(map, a);
        if (!reg) throw new ImageError(`${f.name}: адрес ${hex32(a)} вне карты памяти программатора`);
        a = Math.min(r.end, reg.start + reg.length);
      }
    }
  }
  // 2. Слить файлы; один адрес — одно значение.
  const bytes = new Map(); // адрес → [значение, имя файла]
  for (const f of files) {
    for (const c of f.chunks) {
      for (let i = 0; i < c.bytes.length; i++) {
        const a = c.addr + i;
        const v = c.bytes[i];
        const had = bytes.get(a);
        if (had && had[0] !== v) {
          const who = had[1] === f.name ? f.name : `${had[1]} и ${f.name}`;
          throw new ImageError(`${who}: разные байты по ${hex32(a)}`);
        }
        if (!had) bytes.set(a, [v, f.name]);
      }
    }
  }
  const addrs = [...bytes.keys()].sort((a, b) => a - b);
  // 3. Сплошные куски данных.
  const runs = [];
  for (let i = 0; i < addrs.length; ) {
    let j = i + 1;
    while (j < addrs.length && addrs[j] === addrs[j - 1] + 1) j++;
    const run = new Uint8Array(j - i);
    for (let k = i; k < j; k++) run[k - i] = bytes.get(addrs[k])[0];
    runs.push({ addr: addrs[i], bytes: run });
    i = j;
  }
  // 4. Блоки стирания, в которые попал хотя бы байт, — подряд идущие в одной области сливаются.
  const erase = [];
  for (const run of runs) {
    const reg = flashRegionOf(map, run.addr);
    const bs = reg.blocksize;
    const first = reg.start + Math.floor((run.addr - reg.start) / bs) * bs;
    const end = reg.start + Math.ceil((run.addr + run.bytes.length - reg.start) / bs) * bs;
    const last = erase[erase.length - 1];
    if (last && last.region === reg && first <= last.addr + last.len) {
      last.len = Math.max(last.len, end - last.addr);
    } else {
      erase.push({ addr: first, len: end - first, region: reg });
    }
  }
  return {
    size: addrs.length,
    erase: erase.map(({ addr, len }) => ({ addr, len })),
    runs,
  };
}

/**
 * Куски для vFlashWrite: заголовок «vFlashWrite:addr:» плюс экранированные данные не длиннее limit.
 * needsEscape(b) — байт экранируется и займёт 2 (rsp.needsEscape).
 * Возвращает [{addr, bytes}].
 */
export function writeChunks(runs, limit, needsEscape) {
  const out = [];
  for (const run of runs) {
    let pos = 0;
    while (pos < run.bytes.length) {
      const addr = run.addr + pos;
      const head = `vFlashWrite:${addr.toString(16)}:`.length;
      let room = limit - head;
      let end = pos;
      while (end < run.bytes.length) {
        const cost = needsEscape(run.bytes[end]) ? 2 : 1;
        if (cost > room) break;
        room -= cost;
        end++;
      }
      if (end === pos) throw new Error('кусок vFlashWrite не помещается в пакет');
      out.push({ addr, bytes: run.bytes.subarray(pos, end) });
      pos = end;
    }
  }
  return out;
}

/**
 * Метки выбранных файлов (jaffa-flasher-page design §5). files — [{name, parsed?}], regions — из
 * ответа chip: [{start, end, name, main}], end не входит. Возвращает на каждый файл
 * [{kind: 'region' | 'overlap', text}]: имя каждой неосновной области, которую файл задевает, и
 * «пересекается с <файл>» для каждого файла выше по списку с общими адресами. Байты не сравнивает:
 * противоречие байтов отвергает buildImage.
 */
export function fileTags(files, regions = []) {
  const touches = (ranges, start, end) => ranges.some((r) => r.start < end && start < r.end);
  return files.map((f, i) => {
    if (!f.parsed) return [];
    const tags = [];
    for (const reg of regions) {
      if (!reg.main && touches(f.parsed.ranges, reg.start, reg.end)) tags.push({ kind: 'region', text: reg.name });
    }
    for (const prev of files.slice(0, i)) {
      if (prev.parsed && prev.parsed.ranges.some((r) => touches(f.parsed.ranges, r.start, r.end))) {
        tags.push({ kind: 'overlap', text: `пересекается с ${prev.name}` });
      }
    }
    return tags;
  });
}
