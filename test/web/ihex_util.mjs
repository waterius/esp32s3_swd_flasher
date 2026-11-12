// Сборка строк Intel HEX для тестов: запись с верной суммой.
export function rec(type, offset, data = []) {
  const bytes = [data.length, (offset >> 8) & 0xff, offset & 0xff, type, ...data];
  const sum = (0x100 - (bytes.reduce((a, b) => a + b, 0) & 0xff)) & 0xff;
  return ':' + [...bytes, sum].map((b) => b.toString(16).padStart(2, '0').toUpperCase()).join('');
}

// Файл: база 04, данные кусками по 16 Б, конец.
export function hexFile(addr, bytes) {
  const lines = [];
  let base = -1;
  for (let i = 0; i < bytes.length; i += 16) {
    const a = addr + i;
    if (a >>> 16 !== base) {
      base = a >>> 16;
      lines.push(rec(0x04, 0, [base >> 8, base & 0xff]));
    }
    lines.push(rec(0x00, a & 0xffff, [...bytes.slice(i, i + 16)]));
  }
  lines.push(rec(0x01, 0));
  return lines.join('\r\n') + '\r\n';
}

export function fill(len, value) {
  return Array.from({ length: len }, (_, i) => (typeof value === 'function' ? value(i) : value));
}
