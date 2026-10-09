"""Minimal client for the VICE text remote monitor (x64sc -remotemonitor)."""

import re
import socket
import subprocess
import time

PROMPT = re.compile(rb"\(C:\$[0-9a-f]{4}\) $")
BREAK_RE = re.compile(r"#\d+ \(Stop on\s+exec ([0-9a-f]{4})\)\s+(\d+)/\$[0-9a-f]+,\s+(\d+)/\$[0-9a-f]+\s*\n"
                      r"\.C:[0-9a-f]{4}.*?- A:([0-9a-f]{2}) X:([0-9a-f]{2}) Y:([0-9a-f]{2}) SP:([0-9a-f]{2}) \S+\s+(\d+)",
                      re.I)
MEM_RE = re.compile(r"^>C:([0-9a-f]{4})\s+((?:[0-9a-f]{2}\s+)+)", re.I | re.M)


def read_symbols(path):
    """Numeric symbols from a 64tass --dump-labels file."""
    syms = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r": ([A-Za-z_][\w.]*) = \$([0-9a-fA-F]+)\s*$", line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    return syms


class Vice:
    def __init__(self, vice_dir, crt, port=6510, ntsc=True, extra=()):
        args = [f"{vice_dir}\\x64sc.exe", "-default", "-ntsc" if ntsc else "-pal", "-sounddev", "dummy",
                "-remotemonitor", "-remotemonitoraddress", f"ip4://127.0.0.1:{port}", *extra, "-cartcrt", crt]
        self.proc = subprocess.Popen(args, cwd=vice_dir)
        deadline = time.time() + 15
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)

    def cmd(self, text, timeout=30, expect=None):
        """Send a command and return its output up to the next prompt (after `expect`, if given)."""
        self.sock.sendall((text + "\n").encode("ascii"))
        self.sock.settimeout(timeout)
        buf = b""
        while not PROMPT.search(buf) or (expect and not re.search(expect, buf)):
            data = self.sock.recv(65536)
            if not data:
                break
            buf += data
        return buf.decode("ascii", "replace")

    def go_until_break(self, timeout=30):
        """Continue execution; return dict with pc, raster line, cycle, registers and clock."""
        out = self.cmd("x", timeout)
        m = BREAK_RE.search(out)
        if not m:
            raise RuntimeError(f"unexpected monitor output: {out!r}")
        return {"pc": int(m.group(1), 16), "line": int(m.group(2)), "cycle": int(m.group(3)),
                "a": int(m.group(4), 16), "x": int(m.group(5), 16), "y": int(m.group(6), 16),
                "sp": int(m.group(7), 16), "clock": int(m.group(8))}

    def mem(self, start, end):
        """Read memory start..end inclusive as bytes."""
        out = self.cmd(f"m {start:04x} {end:04x}")
        data = {}
        for m in MEM_RE.finditer(out):
            addr = int(m.group(1), 16)
            for i, b in enumerate(m.group(2).split()):
                data[addr + i] = int(b, 16)
        return bytes(data.get(a, 0) for a in range(start, end + 1))

    def resume(self):
        self.sock.sendall(b"x\n")

    def enter(self):
        """Break into the monitor from running state (VICE first answers with a bare prompt)."""
        return self.cmd("r", expect=rb"STOPWATCH[^\n]*\n[^\n]*\n\(C:")

    def quit(self):
        try:
            self.sock.sendall(b"quit\n")
            self.sock.close()
        finally:
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
