#!/usr/bin/env python3
"""Записывает байты, которые шлёт gdb из тулчейна PlatformIO, — фикстуры для test/test_gdb.

Заглушка на ПК отвечает gdb так, как ответит программатор по таблице design §9
(openspec/changes/swd-flash-py32f002b/design.md), с картой памяти py32f002b. gdb
подключается к ней по TCP: протокол тот же, что по USB-порту. Всё, что gdb прислал,
вместе с `+`, пишется в test/fixtures/gdb_<сценарий>.h массивом байтов.

Образ нейтральный: test/fixtures/src/blank.c — пустая программа для Cortex-M0+, и файлы
для USER OTP с узором. Чужие прошивки сюда не кладутся.

Запуск после смены тулчейна PlatformIO:
    python3 tools/gdb_record.py
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "test" / "fixtures"
SRC = FIX / "src"
BIN = Path.home() / ".platformio" / "packages" / "toolchain-gccarmnoneeabi" / "bin"
GCC, GDB = BIN / "arm-none-eabi-gcc", BIN / "arm-none-eabi-gdb"

# Карта памяти py32f002b — design §6, §9.
REGIONS = [  # (вид, начало, размер, блок)
    ("flash", 0x08000000, 0x6000, 0x80),
    ("flash", 0x1FFF0280, 0x80, 0x80),
    ("ram", 0x20000000, 0xC00, 0),
]
MEMMAP = (
    '<?xml version="1.0"?><!DOCTYPE memory-map PUBLIC "+//IDN gnu.org//DTD GDB Memory Map V1.0//EN"'
    ' "http://sourceware.org/gdb/gdb-memory-map.dtd"><memory-map>'
    + "".join(
        f'<memory type="flash" start="0x{s:x}" length="0x{n:x}"><property name="blocksize">0x{b:x}</property></memory>'
        if kind == "flash"
        else f'<memory type="ram" start="0x{s:x}" length="0x{n:x}"/>'
        for kind, s, n, b in REGIONS
    )
    + "</memory-map>"
)
REG_NAMES = [f"r{i}" for i in range(13)]
TDESC = (
    '<?xml version="1.0"?><!DOCTYPE target SYSTEM "gdb-target.dtd"><target><architecture>arm</architecture>'
    '<feature name="org.gnu.gdb.arm.m-profile">'
    + "".join(f'<reg name="{n}" bitsize="32"/>' for n in REG_NAMES)
    + '<reg name="sp" bitsize="32" type="data_ptr"/><reg name="lr" bitsize="32"/>'
    '<reg name="pc" bitsize="32" type="code_ptr"/><reg name="xpsr" bitsize="32" regnum="25"/>'
    "</feature></target>"
)


def checksum(data: bytes) -> int:
    return sum(data) & 0xFF


def frame(data: bytes) -> bytes:
    return b"$" + data + b"#" + b"%02x" % checksum(data)


def unescape(raw: bytes) -> bytes:
    out, i = bytearray(), 0
    while i < len(raw):
        if raw[i] == 0x7D:
            out.append(raw[i + 1] ^ 0x20)
            i += 2
        else:
            out.append(raw[i])
            i += 1
    return bytes(out)


class Stub:
    """Программатор по таблице design §9 с поддельной памятью чипа."""

    def __init__(self, fail_done=False, attach_fail=False, on_first_write=None):
        self.fail_done, self.attach_fail, self.on_first_write = fail_done, attach_fail, on_first_write
        self.mem: dict[int, int] = {}
        self.noack = self.session = self.ok_write = self.failed = False
        self.seen_write = False
        self.inbound = bytearray()

    def region(self, addr: int, length: int, kinds=("flash",)):
        for kind, s, n, _ in REGIONS:
            if kind in kinds and s <= addr and addr + length <= s + n:
                return True
        return False

    def read(self, addr: int) -> int:
        if addr in self.mem:
            return self.mem[addr]
        return 0x00 if 0x20000000 <= addr < 0x20000C00 else 0xFF

    def xfer(self, doc: str, args: str) -> bytes:
        off, ln = (int(x, 16) for x in args.split(","))
        chunk = doc[off : off + ln]
        return (b"l" if off + ln >= len(doc) else b"m") + chunk.encode()

    def handle(self, p: bytes) -> bytes | None:
        s = p.decode("latin1")
        if s.startswith("qSupported"):
            return b"PacketSize=1000;qXfer:memory-map:read+;qXfer:features:read+;QStartNoAckMode+"
        if s == "QStartNoAckMode":
            self.noack = True
            return b"OK"
        if s in ("!",) or s.startswith(("Hg", "Hc")) or s == "qSymbol::":
            return b"OK"
        if s == "?":
            return b"T05" if self.session else b"W00"
        if s.startswith("qRcmd,"):
            cmd = bytes.fromhex(s[6:]).decode()
            if cmd == "swdp_scan":
                if self.attach_fail:
                    text = "# чип не отвечает\n"
                else:
                    self.session = True
                    text = "# SWD: под сбросом, IDCODE 0x0bc11477, 3 мс\n1 py32f002b\n"
            else:
                text = "# неизвестная команда\n"
            return [b"O" + text.encode().hex().encode(), b"OK"]
        if s.startswith("vAttach;"):
            if self.attach_fail:
                return b"E01"
            self.session = True
            return b"T05"
        if s == "qC":
            return b"QC1"
        if s == "qfThreadInfo":
            return b"m1"
        if s == "qsThreadInfo":
            return b"l"
        if s == "qAttached":
            return b"1"
        if s.startswith("qXfer:features:read:target.xml:"):
            return self.xfer(TDESC, s.rsplit(":", 1)[1])
        if s.startswith("qXfer:memory-map:read::"):
            return self.xfer(MEMMAP, s.split("::")[1])
        if s.startswith(("g", "p", "P", "m", "vFlash")) and not self.session:
            return b"E01"
        if s == "g":
            return b"00000000" * 13 + b"000c0020" + b"ffffffff" + b"c1000008" + b"00000001"
        if s.startswith("p"):
            return b"00000000"
        if s.startswith("P"):
            return b"OK"
        if s.startswith("m"):
            addr, ln = (int(x, 16) for x in s[1:].split(","))
            return bytes(self.read(addr + i) for i in range(ln)).hex().encode()
        if s.startswith("vFlashErase:"):
            addr, ln = (int(x, 16) for x in s[12:].split(","))
            if not self.region(addr, ln):
                self.failed = True
                return b"E01"
            for a in range(addr, addr + ln):
                self.mem[a] = 0xFF
            return b"OK"
        if s.startswith("vFlashWrite:"):
            head, data = p[12:].split(b":", 1)
            addr, data = int(head, 16), unescape(data)
            if not self.seen_write:
                self.seen_write = True
                if self.on_first_write:
                    self.on_first_write()
                    return None
            if not self.region(addr, len(data)):
                self.failed = True
                return b"E01"
            for i, b in enumerate(data):
                self.mem[addr + i] = b
            return b"OK"
        if s == "vFlashDone":
            if self.fail_done:
                self.failed = True
                return b"E01"
            self.ok_write = True
            return b"OK"
        if s.startswith("vKill"):
            ok = self.ok_write and not self.failed
            self.session = self.ok_write = self.failed = False
            return b"OK" if ok else b"E01"
        if s == "D":
            self.session = self.ok_write = self.failed = False
            return b"OK"
        if s in ("c", "s") or s.startswith(("vCont;", "Z", "M", "X")):
            return b"E01"
        return b""

    def serve(self, conn: socket.socket):
        buf = b""
        while True:
            try:
                data = conn.recv(65536)
            except OSError:
                return
            if not data:
                return
            self.inbound += data
            buf += data
            while True:
                while buf[:1] in (b"+", b"-", b"\x03"):
                    buf = buf[1:]
                start = buf.find(b"$")
                if start < 0:
                    buf = b""
                    break
                end = buf.find(b"#", start)
                if end < 0 or len(buf) < end + 3:
                    buf = buf[start:]
                    break
                body = buf[start + 1 : end]
                buf = buf[end + 3 :]
                if not self.noack:
                    conn.sendall(b"+")
                replies = self.handle(body)
                if replies is None:
                    conn.close()
                    return
                for r in replies if isinstance(replies, list) else [replies]:
                    conn.sendall(frame(r))


def build():
    common = ["-mcpu=cortex-m0plus", "-mthumb", "-Os", "-nostdlib", "-T", str(SRC / "blank.ld")]
    subprocess.run([GCC, *common, str(SRC / "blank.c"), "-o", FIX / "blank.elf"], check=True)
    subprocess.run([GCC, *common, "-DWITH_TAIL", str(SRC / "blank.c"), "-o", FIX / "blank_tail.elf"], check=True)
    otp = bytes([0xFF] * 4 + [i & 0xFF for i in range(4, 128)])
    write_ihex(FIX / "otp.hex", 0x1FFF0280, otp)
    write_ihex(FIX / "otp_lock.hex", 0x1FFF0280, bytes([0x55, 0xAA]) + otp[2:])


def write_ihex(path: Path, addr: int, data: bytes):
    lines = []

    def rec(kind, off, payload):
        body = bytes([len(payload), (off >> 8) & 0xFF, off & 0xFF, kind]) + payload
        lines.append(":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper())

    rec(4, 0, bytes([(addr >> 24) & 0xFF, (addr >> 16) & 0xFF]))
    for i in range(0, len(data), 16):
        rec(0, (addr + i) & 0xFFFF, data[i : i + 16])
    rec(1, 0, b"")
    path.write_text("\n".join(lines) + "\n")


SCENARIOS = {
    # имя: (команды gdb, файл для gdb, параметры заглушки, что проверяет)
    "pio_upload": (["load", "compare-sections", "kill"], "blank.elf", {}, "заливка командой PlatformIO"),
    "otp_load": (["load otp.hex", "kill"], None, {}, "load файла для USER OTP"),
    "two_loads": (["load", "load otp.hex", "kill"], "blank.elf", {}, "прошивка и OTP за один сеанс"),
    "flash_fail": (["load", "compare-sections", "kill"], "blank.elf", {"fail_done": True}, "провал vFlashDone"),
    "attach_fail": (["load", "kill"], "blank.elf", {"attach_fail": True}, "attach не удался"),
    "load_failed": (["load", "kill"], "blank_tail.elf", {}, "секция вне карты памяти — Load failed"),
    "close_mid_write": (["load", "kill"], "blank.elf", {"close": True}, "gdb пропал посреди записи"),
}


def record(name: str) -> tuple[bytes, int, str]:
    cmds, elf, opts, _ = SCENARIOS[name]
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    args = [str(GDB), "-nx", "--batch", "-ex", f"target extended-remote localhost:{port}", "-ex", "monitor swdp_scan", "-ex", "attach 1"]
    for c in cmds:
        args += ["-ex", c]
    if elf:
        args.append(elf)
    gdb = subprocess.Popen(args, cwd=FIX, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    stub = Stub(fail_done=opts.get("fail_done", False), attach_fail=opts.get("attach_fail", False),
                on_first_write=(lambda: None) if opts.get("close") else None)
    conn, _ = srv.accept()
    t = threading.Thread(target=stub.serve, args=(conn,), daemon=True)
    t.start()
    try:
        out, _ = gdb.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        gdb.kill()
        out, _ = gdb.communicate()
    t.join(timeout=5)
    srv.close()
    return bytes(stub.inbound), gdb.returncode, out.decode(errors="replace")


def gdb_version() -> str:
    return subprocess.run([GDB, "--version"], capture_output=True, text=True).stdout.splitlines()[0]


def write_header(name: str, data: bytes, rc: int, out: str, version: str):
    cmds, elf, _, what = SCENARIOS[name]
    tail = [l for l in out.splitlines() if l.strip() and not l.startswith("Section ")][-3:]
    ident = "GDB_" + name.upper()
    body = ",\n".join(
        "    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) for i in range(0, len(data), 16)
    )
    text = (
        f"// Записано tools/gdb_record.py — не править руками.\n"
        f"// {version}\n"
        f"// Сценарий «{what}»: gdb {' '.join(['monitor swdp_scan', 'attach 1', *cmds])}"
        f"{' ' + elf if elf else ''}; код возврата gdb {rc}.\n"
        + "".join(f"// gdb: {l}\n" for l in tail)
        + "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n"
        f"static const uint8_t {ident}[] = {{\n{body}\n}};\n"
        f"static const size_t {ident}_LEN = sizeof({ident});\n"
        f"#define {ident}_RC {rc}\n"
    )
    (FIX / f"gdb_{name}.h").write_text(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="сценарии, по умолчанию все: " + ", ".join(SCENARIOS))
    a = ap.parse_args()
    build()
    version = gdb_version()
    for name in a.names or SCENARIOS:
        data, rc, out = record(name)
        write_header(name, data, rc, out, version)
        print(f"{name}: {len(data)} Б от gdb, код возврата {rc}")


if __name__ == "__main__":
    sys.exit(main())
