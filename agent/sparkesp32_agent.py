#!/usr/bin/env python3
"""sparkesp32-agent: DGX Spark vitals for the SparkESP32 display.

Samples once a second:
    nvidia-smi     -> GPU %, temperature (C), power (W)   [one long-lived child]
    /proc/stat     -> CPU % (aggregate + per core)
    /proc/meminfo  -> unified memory used/total (decimal MB, matches NVIDIA's GB)

and publishes the same `stat` line (protocol v1, see docs/protocol.md) two ways:
    1. USB serial: pushed to the display while it's plugged in
    2. HTTP: GET /stat on :8787, polled by the display over Wi-Fi; GET /healthz is JSON

The serial side only talks to a device that matches the display's USB ID *and*
answers the `status` handshake with `fw=sparkesp32`, so other serial gadgets on
the host are never written to. Installed by install.sh as a systemd service.
"""
import argparse
import json
import logging
import os
import select
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # HTTP-only mode
    serial = None

__version__ = "1.0.1"
log = logging.getLogger("sparkesp32")

USB_VID, USB_PID = 0x303A, 0x1001     # Espressif USB-Serial/JTAG (ESP32-C6 native USB)
HANDSHAKE = b"fw=sparkesp32"
REJECT_S = 300.0                      # a port that fails the handshake is skipped this long
HANDSHAKE_S = 10.0                    # keep asking a just-plugged-in board this long: it's still
                                      # booting, then its console drops input for 0.5 s
STALE_S = 5.0                        # /stat answers 503 if the sampler is older than this
MAX_CORES = 32                        # what the display shows; also keeps a line < 256 bytes,
                                      # the firmware console's line buffer
GPU_STALE_S = 5.0                     # nvidia-smi values older than this are reported unknown
GPU_STALL_S = 10.0                    # it prints a line a second: this long silent, it's hung
NVSMI = ["nvidia-smi", "-i", "0", "--query-gpu=utilization.gpu,temperature.gpu,power.draw",
         "--format=csv,noheader,nounits", "-lms", "1000"]


class RateLimitedLog:
    """Log a recurring problem at most once per `every` seconds."""

    def __init__(self, every=60.0):
        self.every, self.last = every, {}

    def warn(self, key, msg, *args):
        now = time.monotonic()
        if now - self.last.get(key, -1e9) >= self.every:
            self.last[key] = now
            log.warning(msg, *args)


rl = RateLimitedLog()


# ---------------------------------------------------------------- sampling

def read_cpu_times():
    """[(idle, total)] for the aggregate line, then each core, from /proc/stat."""
    out = []
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu"):
                break
            v = [int(x) for x in line.split()[1:9]]   # user..steal; guest is inside user
            out.append((v[3] + v[4], sum(v)))         # idle + iowait, total
    return out


def pct(prev, cur):
    d_total = cur[1] - prev[1]
    d_idle = cur[0] - prev[0]
    if d_total <= 0:
        return 0
    return max(0, min(100, round(100 * (d_total - d_idle) / d_total)))


def read_mem_mb():
    """(used_mb, total_mb), decimal MB; used = MemTotal - MemAvailable."""
    kb = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":", 1)
            if k in ("MemTotal", "MemAvailable"):
                kb[k] = int(v.split()[0])
    total = kb.get("MemTotal", 0)
    used = total - kb.get("MemAvailable", total)
    return round(used * 1024 / 1e6), round(total * 1024 / 1e6)


class GpuReader(threading.Thread):
    """Keeps one `nvidia-smi -lms 1000` child running and remembers its latest line.

    Sampling never waits on nvidia-smi: fields older than GPU_STALE_S go out as
    unknown ("-"). The child's state, published in /healthz as gpu_source:
      starting     spawned, no line yet
      streaming    a line a second
      stalled      silent for GPU_STALL_S (a hung driver call): killed, then respawned
      exited       it quit (gpu_error says how); respawned after a backoff
      unavailable  it couldn't be started (gpu_error says why)
    """

    def __init__(self, stop):
        super().__init__(daemon=True, name="gpu")
        self.stop, self.state, self.error = stop, "starting", None
        self.reading = ((None, None, None), 0.0)   # (values, monotonic time); replaced whole
        self.first = threading.Event()             # set by the first reading

    @staticmethod
    def _num(s, scale=1.0):
        try:
            return round(float(s.strip()) * scale)
        except ValueError:      # "[N/A]" for fields the GPU doesn't report
            return None

    def run(self):
        backoff = 1.0
        while not self.stop.is_set():
            try:
                proc = subprocess.Popen(NVSMI, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            except OSError as e:
                self.state, self.error = "unavailable", str(e)
            else:
                with proc:                                 # closes its pipes on the way out
                    self.state = "starting"
                    if self._stream(proc):
                        backoff = 1.0
                    proc.kill()
                    code = proc.wait()
                    if self.state != "stalled":
                        err = proc.stderr.read().decode(errors="replace").strip().splitlines()
                        self.state, self.error = "exited", f"exit {code}: {err[-1] if err else 'no output'}"
            if self.stop.is_set():
                break
            rl.warn("gpu", "nvidia-smi %s (%s); GPU fields show -- until it's back", self.state, self.error)
            self.stop.wait(min(backoff, 60))
            backoff *= 2

    def _stream(self, proc):
        """Take lines until the child exits, goes quiet for GPU_STALL_S, or we stop.
        True if it gave at least one reading."""
        fd, buf, got, heard = proc.stdout.fileno(), b"", False, time.monotonic()
        while not self.stop.is_set():
            if not select.select([fd], [], [], 0.5)[0]:
                if time.monotonic() - heard > GPU_STALL_S:
                    self.state, self.error = "stalled", f"no output for {GPU_STALL_S:.0f}s"
                    return got
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                return got                        # it exited
            *lines, buf = (buf + chunk).split(b"\n")
            buf = buf[-4096:]
            for line in lines:
                heard = time.monotonic()
                f = line.decode(errors="replace").split(",")
                if len(f) == 3:
                    values = (self._num(f[0]), self._num(f[1]), self._num(f[2], 10))
                    self.reading = (values, time.monotonic())
                    self.state, self.error, got = "streaming", None, True
                    self.first.set()
        return got

    def latest(self):
        """(gpu_pct, temp_c, power_dw), each None when unknown or stale."""
        values, at = self.reading
        return values if time.monotonic() - at <= GPU_STALE_S else (None, None, None)


def fmt(v):
    return "-" if v is None else str(v)


def short_hostname():
    """This host's short name for the display title, or "-" if it isn't wire-safe."""
    name = socket.gethostname().split(".")[0]
    ok = 0 < len(name) <= 63 and all(c.isascii() and (c.isalnum() or c in "-_") for c in name)
    return name if ok else "-"


class Sampler:
    """The latest stat line, refreshed once per interval, and the display link's
    status, both read by the HTTP handler. Each is replaced whole, never mutated,
    so readers need no lock."""

    def __init__(self, gpu):
        self.gpu, self.host = gpu, short_hostname()
        self.snap = (None, 0.0, {})          # (line, monotonic time of it, /healthz fields)
        self.display = {"state": "searching"}    # the USB link, kept by serial_loop
        self._prev = read_cpu_times()

    def tick(self):
        cur = read_cpu_times()
        cpu = pct(self._prev[0], cur[0])
        # cores hot-plugged/offlined between reads -> only compare what lines up
        cores = [pct(p, c) for p, c in zip(self._prev[1:], cur[1:], strict=False)][:MAX_CORES]
        self._prev = cur
        memu, memt = read_mem_mb()
        gpu, temp, pow_dw = self.gpu.latest()
        line = (f"stat {cpu} {fmt(gpu)} {memu} {memt} {fmt(temp)} {fmt(pow_dw)} "
                f"{','.join(map(str, cores)) or '-'} {self.host}")
        sample = dict(host=self.host, cpu=cpu, gpu=gpu, mem_used_mb=memu, mem_total_mb=memt,
                      temp_c=temp, power_w=None if pow_dw is None else pow_dw / 10,
                      cores=cores, gpu_source=self.gpu.state, gpu_error=self.gpu.error)
        self.snap = (line, time.monotonic(), sample)

    def latest(self):
        """(line, at, sample); `at` is the monotonic time of that line."""
        return self.snap


# ---------------------------------------------------------------- HTTP

def make_handler(sampler):
    class Handler(BaseHTTPRequestHandler):
        timeout = 5                     # idle clients can't pin a thread forever
        server_version = f"sparkesp32-agent/{__version__}"

        def _send(self, code, body, ctype="text/plain; charset=utf-8"):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            line, at, sample = sampler.latest()
            age = time.monotonic() - at
            path = self.path.split("?", 1)[0]
            if path == "/stat":
                if line is None or age > STALE_S:
                    self._send(503, "stale\n")
                else:
                    self._send(200, line + "\n")
            elif path == "/healthz":
                body = dict(version=__version__, age_s=round(age, 2), display=sampler.display, **sample)
                self._send(200, json.dumps(body) + "\n", "application/json")
            else:
                self._send(404, "try /stat or /healthz\n")

        def log_message(self, *args):   # one request a second, forever: keep journald quiet
            pass

    return Handler


# ---------------------------------------------------------------- serial

def candidate_ports():
    return sorted(p.device for p in serial.tools.list_ports.comports()
                  if p.vid == USB_VID and p.pid == USB_PID)


def handshake(ser, timeout=HANDSHAKE_S, every=1.0):
    """True if the device on `ser` is a SparkESP32 (answers status with fw=sparkesp32).

    Asks again every `every` seconds: the port appears the moment the board is
    plugged in, before its console exists, and the console's terminal probe
    (IDF linenoiseProbe) then throws away everything typed in its first 0.5 s.
    """
    ser.reset_input_buffer()
    buf, deadline, ask_at = b"", time.monotonic() + timeout, 0.0
    while time.monotonic() < deadline:
        if time.monotonic() >= ask_at:
            ser.write(b"\nstatus\n")
            ask_at = time.monotonic() + every
        buf = (buf + ser.read(256))[-4096:]
        if HANDSHAKE in buf:
            return True
    return False


def serial_loop(sampler, explicit_port, stop):
    """Push each fresh sample to the display; survive unplug/replug.

    The link's state goes to /healthz as sampler.display = {"state": ..., ...}:
      searching    no usable port ("error" says why if opening one failed)
      handshaking  asking `status` on "port", for up to HANDSHAKE_S
      streaming    pushing stat lines to "port" ("answered_in_s": handshake time)
      rejected     "port" didn't answer as a SparkESP32: skipped until it's
                   replugged or REJECT_S passes ("retry_in_s")
    """
    ser, last_at, rejected = None, 0.0, {}
    while not stop.is_set():
        try:
            if ser is None:
                now = time.monotonic()
                # unplugging forgets a rejection, so a replug is always retried at once
                rejected = {p: t for p, t in rejected.items() if t > now and os.path.exists(p)}
                ports = [explicit_port] if explicit_port else candidate_ports()
                ports = [p for p in ports if p not in rejected]
                if not ports:
                    if rejected:
                        port, until = min(rejected.items(), key=lambda kv: kv[1])
                        sampler.display = {"state": "rejected", "port": port,
                                           "retry_in_s": round(until - now)}
                    else:
                        sampler.display = {"state": "searching"}
                    stop.wait(2)
                    continue
                port = ports[0]
                sampler.display = {"state": "handshaking", "port": port}
                ser = serial.Serial(port, 115200, timeout=0.2, write_timeout=1)
                t0 = time.monotonic()
                if not handshake(ser, HANDSHAKE_S):
                    log.warning("%s is an Espressif USB device but not a SparkESP32; ignoring it "
                                "for %d minutes or until it's replugged", port, REJECT_S // 60)
                    ser.close()
                    ser, rejected[port] = None, time.monotonic() + REJECT_S
                    continue
                took = round(time.monotonic() - t0, 1)
                log.info("display connected on %s (answered in %.1fs)", port, took)
                sampler.display = {"state": "streaming", "port": port, "answered_in_s": took}
            line, at, _ = sampler.latest()
            if line and at != last_at:
                ser.write((line + "\n").encode())
                last_at = at
            if ser.in_waiting:              # drain console echo/replies so the
                ser.read(ser.in_waiting)    # display's TX never backs up
            stop.wait(0.2)
        except Exception as e:              # unplug, permission, anything: retry, don't die
            if ser is not None:
                log.info("display disconnected (%s)", e)
                try:
                    ser.close()
                except Exception:
                    pass
            else:
                rl.warn("serial-open", "can't open the display's port: %s", e)
            ser = None
            sampler.display = {"state": "searching", "error": str(e)}
            stop.wait(2)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between samples")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="HTTP bind address (default all interfaces; ufw limits it to the LAN)")
    ap.add_argument("--http-port", type=int, default=8787)
    ap.add_argument("--port", help="serial device (default: autodetect the display)")
    ap.add_argument("--no-serial", action="store_true", help="HTTP only")
    ap.add_argument("--no-http", action="store_true", help="USB only")
    ap.add_argument("--once", action="store_true", help="print one stat line and exit")
    ap.add_argument("--version", action="version", version=__version__)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    gpu = GpuReader(stop)
    gpu.start()
    sampler = Sampler(gpu)
    gpu.first.wait(2)              # so the first line already has GPU values, not "-"
    sampler.tick()

    if args.once:
        print(sampler.latest()[0])
        stop.set()
        return 0

    if not args.no_http:
        httpd = ThreadingHTTPServer((args.bind, args.http_port), make_handler(sampler))
        httpd.daemon_threads = True
        threading.Thread(target=httpd.serve_forever, daemon=True, name="http").start()
        log.info("serving /stat on http://%s:%d", args.bind, args.http_port)
    if not args.no_serial:
        if serial is None:
            log.warning("pyserial missing: USB output disabled")
        else:
            threading.Thread(target=serial_loop, args=(sampler, args.port, stop),
                             daemon=True, name="serial").start()

    while not stop.wait(args.interval):
        try:
            sampler.tick()
        except Exception:
            log.exception("sample failed")
    log.info("stopping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
