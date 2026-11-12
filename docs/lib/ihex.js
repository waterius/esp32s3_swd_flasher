// Синтаксис Intel HEX: записи 00 (данные), 01 (конец), 02 и 04 (база адреса); 03 и 05 (точка
// входа) пропускаются. Ошибки — с номером строки. Порт и оснастку не знает (design §3).

/** Ошибка разбора: текст для человека без имени файла, номер строки с 1. */
export class IhexError extends Error {
  constructor(message, line) {
    super(line ? `строка ${line}: ${message}` : message);
    this.line = line;
  }
}

/**
 * Разобрать текст файла Intel HEX.
 * Возвращает куски данных по абсолютным адресам: [{addr, bytes: Uint8Array}] в порядке файла.
 * Бросает IhexError.
 */
export function parseIhex(text) {
  const lines = text.split(/\r?\n/);
  const chunks = [];
  let base = 0;
  let endLine = 0;
  for (let i = 0; i < lines.length; i++) {
    const n = i + 1;
    const line = lines[i].trim();
    if (!line) continue;
    if (endLine) throw new IhexError(`данные после записи конца (строка ${endLine})`, n);
    if (line[0] !== ':') throw new IhexError('строка не начинается с «:»', n);
    const hex = line.slice(1);
    if (hex.length % 2 || /[^0-9a-fA-F]/.test(hex)) throw new IhexError('не hex', n);
    const rec = new Uint8Array(hex.length / 2);
    for (let k = 0; k < rec.length; k++) rec[k] = parseInt(hex.substr(2 * k, 2), 16);
    if (rec.length < 5) throw new IhexError('запись короче 5 байт', n);
    const len = rec[0];
    if (rec.length !== len + 5) throw new IhexError(`длина ${len} не совпадает с записью`, n);
    let sum = 0;
    for (const b of rec) sum = (sum + b) & 0xff;
    if (sum !== 0) throw new IhexError('неверная контрольная сумма', n);
    const offset = (rec[1] << 8) | rec[2];
    const type = rec[3];
    const data = rec.subarray(4, 4 + len);
    switch (type) {
      case 0x00:
        if (len) chunks.push({ addr: (base + offset) >>> 0, bytes: data.slice() });
        break;
      case 0x01:
        if (len) throw new IhexError('у записи конца есть данные', n);
        endLine = n;
        break;
      case 0x02:
        if (len !== 2) throw new IhexError('у записи 02 не 2 байта', n);
        base = ((data[0] << 8) | data[1]) * 16;
        break;
      case 0x04:
        if (len !== 2) throw new IhexError('у записи 04 не 2 байта', n);
        base = (((data[0] << 8) | data[1]) << 16) >>> 0;
        break;
      case 0x03:
      case 0x05:
        break; // точка входа: оснастке не нужна
      default:
        throw new IhexError(`неизвестный тип записи 0x${type.toString(16).padStart(2, '0').toUpperCase()}`, n);
    }
  }
  if (!endLine) throw new IhexError('нет записи конца (01)');
  return chunks;
}
