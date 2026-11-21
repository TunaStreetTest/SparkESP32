#!/usr/bin/env python3
"""Host tests for the agent and the serial tools. Stdlib only: pyserial is
stubbed when it isn't installed, so `make check` runs anywhere.

    python3 tests/test_agent.py
"""
import base64
import json
import os
import sys
import threading
import time
import types
import unittest
import urllib.error
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = [os.path.join(ROOT, "agent"), os.path.join(ROOT, "tools")]

try:
    import serial  # noqa: F401
except ImportError:                       # minimal stand-in so the modules import
    fake = types.ModuleType("serial")
    fake.Serial = object
    fake.tools = types.ModuleType("serial.tools")
    fake.tools.list_ports = types.ModuleType("serial.tools.list_ports")
    fake.tools.list_ports.comports = lambda: []
    sys.modules.update({"serial": fake, "serial.tools": fake.tools,
                        "serial.tools.list_ports": fake.tools.list_ports})

import devctl  # noqa: E402
import sparkesp32_agent as agent  # noqa: E402
import sparklink  # noqa: E402


# ------------------------------------------------------------------ agent

class FakeGpu:
    def __init__(self, values=(91, 60, 410), state="ok"):
        self.values, self.state = values, state

    def latest(self):
        return self.values


class SamplerTest(unittest.TestCase):
    def make(self, gpu, times, mem=(46328, 130661)):
        seq = iter(times)
        orig = agent.read_cpu_times, agent.read_mem_mb
        agent.read_cpu_times = lambda: next(seq)
        agent.read_mem_mb = lambda: mem
        self.addCleanup(lambda: (setattr(agent, "read_cpu_times", orig[0]),
                                 setattr(agent, "read_mem_mb", orig[1])))
        return agent.Sampler(gpu)

    def test_line_is_protocol_v1(self):
        # aggregate + 3 cores: (idle, total) pairs, 1 s apart
        s = self.make(FakeGpu(), [
            [(100, 200), (10, 50), (10, 50), (10, 50)],
            [(150, 300), (60, 100), (35, 100), (10, 100)],
        ])
        s.tick()
        line, at, sample = s.latest()
        self.assertEqual(line, "stat 50 91 46328 130661 60 410 0,50,100 " + agent.short_hostname())
        self.assertEqual(len(line.split()), 9)
        self.assertEqual(sample["power_w"], 41.0)
        self.assertGreater(at, 0)

    def test_unknown_gpu_fields_are_dashes(self):
        s = self.make(FakeGpu((None, None, None), "nvidia-smi unavailable"),
                      [[(0, 0), (0, 0)], [(0, 100), (50, 100)]])
        s.tick()
        self.assertEqual(s.latest()[0], "stat 100 - 46328 130661 - - 50 " + agent.short_hostname())

    def test_big_machine_line_fits_the_console_buffer(self):
        # 144 cores, every value 3 digits, a 63-char hostname: worst case
        n = 144
        s = self.make(FakeGpu((100, 150, 100000)),
                      [[(0, 0)] * (n + 1), [(0, 100)] * (n + 1)], mem=(2**31 - 1, 2**31 - 1))
        s.host = "h" * 63
        s.tick()
        line = s.latest()[0]
        self.assertEqual(len(line.split()[7].split(",")), agent.MAX_CORES)
        self.assertLess(len(line) + 1, 256)       # + "\n"; firmware console buffer is 256

    def test_core_count_change_compares_only_matching_cores(self):
        s = self.make(FakeGpu(), [[(0, 0), (0, 0), (0, 0)], [(0, 100), (0, 100)]])
        s.tick()
        self.assertEqual(s.latest()[0].split()[7], "100")

    def test_hostname_is_wire_safe(self):
        orig = agent.socket.gethostname
        self.addCleanup(setattr, agent.socket, "gethostname", orig)
        for raw, want in [("spark-1a2b", "spark-1a2b"), ("spark-1a2b.lan", "spark-1a2b"),
                          ("my_box", "my_box"), ("caf\u00e9", "-"), ("", "-"), ("x" * 64, "-")]:
            agent.socket.gethostname = lambda raw=raw: raw
            self.assertEqual(agent.short_hostname(), want, raw)

    def test_pct_clamps_and_handles_no_progress(self):
        self.assertEqual(agent.pct((5, 10), (5, 10)), 0)
        self.assertEqual(agent.pct((0, 0), (0, 100)), 100)
        self.assertEqual(agent.pct((0, 0), (100, 100)), 0)
        self.assertEqual(agent.pct((0, 0), (200, 100)), 0)    # bogus counters: clamp

    def test_gpu_reader_parses_nvidia_smi_fields(self):
        self.assertEqual(agent.GpuReader._num(" 42 "), 42)
        self.assertEqual(agent.GpuReader._num("10.6", 10), 106)
        self.assertIsNone(agent.GpuReader._num("[N/A]"))

    def test_gpu_reader_goes_stale(self):
        g = agent.GpuReader(threading.Event())
        g.values, g.at = (1, 2, 3), time.monotonic()
        self.assertEqual(g.latest(), (1, 2, 3))
        g.at -= agent.GPU_STALE_S + 1
        self.assertEqual(g.latest(), (None, None, None))


class HttpTest(unittest.TestCase):
    def setUp(self):
        from http.server import ThreadingHTTPServer
        self.sampler = agent.Sampler.__new__(agent.Sampler)
        self.sampler.lock = threading.Lock()
        self.sampler.line, self.sampler.at = "stat 1 2 3 4 5 6 7 h", time.monotonic()
        self.sampler.sample = {"cpu": 1}
        status = agent.DisplayStatus()
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), agent.make_handler(self.sampler, status))
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def get(self, path):
        try:
            with urllib.request.urlopen(self.base + path, timeout=3) as r:
                return r.status, r.read().decode(), r.headers
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.read().decode(), e.headers

    def test_stat(self):
        code, body, headers = self.get("/stat")
        self.assertEqual((code, body), (200, "stat 1 2 3 4 5 6 7 h\n"))
        self.assertEqual(headers["Content-Length"], str(len(body)))   # firmware requires it

    def test_stale_is_503(self):
        self.sampler.at -= agent.STALE_S + 1
        self.assertEqual(self.get("/stat")[0], 503)

    def test_healthz_json(self):
        code, body, _ = self.get("/healthz")
        h = json.loads(body)
        self.assertEqual((code, h["version"], h["display"], h["cpu"]),
                         (200, agent.__version__, "not connected", 1))

    def test_unknown_path_404(self):
        self.assertEqual(self.get("/nope")[0], 404)


# ------------------------------------------------------------------ serial tools

class FakeSerial:
    """Echoes what's written (like the REPL in dumb mode), then replies."""

    def __init__(self, replies):
        self.replies, self.out = replies, b""

    def reset_input_buffer(self):
        self.out = b""

    def write(self, data):
        cmd = data.decode().strip()
        self.out += b"spark>" + data.replace(b"\n", b"\r\n") + self.replies(cmd).encode()

    def flush(self):
        pass

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk

    def close(self):
        pass


class BootingBoard:
    """A display whose console ignores its first `deaf` writes, like one still booting."""

    def __init__(self, deaf, fw=b"fw=sparkesp32 version=0.1.0"):
        self.deaf, self.fw, self.out, self.writes = deaf, fw, b"", 0

    def reset_input_buffer(self):
        self.out = b""

    def write(self, data):
        self.writes += 1
        if self.writes > self.deaf:
            self.out += b"STATUS-BEGIN\r\n" + self.fw + b"\r\nSTATUS-END\r\n"

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        if not chunk:
            time.sleep(0.005)
        return chunk


class AgentHandshakeTest(unittest.TestCase):
    def test_asks_again_while_the_board_boots(self):
        board = BootingBoard(deaf=3)
        self.assertTrue(agent.handshake(board, timeout=2, every=0.05))
        self.assertEqual(board.writes, 4)

    def test_rejects_other_firmware(self):
        self.assertFalse(agent.handshake(BootingBoard(deaf=0, fw=b"fw=other"), timeout=0.3, every=0.05))

    def test_rejects_a_silent_device(self):
        self.assertFalse(agent.handshake(BootingBoard(deaf=10**6), timeout=0.3, every=0.05))


def link_with(replies):
    link = sparklink.Link.__new__(sparklink.Link)
    link.port, link.ser = "/dev/fake", FakeSerial(replies)
    return link


class LinkTest(unittest.TestCase):
    def test_end_patterns(self):
        self.assertEqual(sparklink.end_pattern("status"), "STATUS-END")
        self.assertEqual(sparklink.end_pattern("wifi"), "NET-END")
        self.assertEqual(sparklink.end_pattern("wifi scan"), "SCAN-END")
        self.assertEqual(sparklink.end_pattern('wifi ssids "a"'), r"^(OK|ERR)\b")

    def test_echo_and_console_footer_are_not_the_reply(self):
        link = link_with(lambda c: "ERR stat cpu\r\nCommand returned non-zero error code: 0x1 (ERROR)\r\n")
        self.assertEqual(link.cmd("stat 999 1 1 1 1 1 1"), ["ERR stat cpu"])
        with self.assertRaises(sparklink.LinkError) as e:
            link.check("stat 999 1 1 1 1 1 1")
        self.assertIn("ERR stat cpu", str(e.exception))

    def test_secret_command_keeps_only_the_verdict(self):
        link = link_with(lambda c: "I (123) wifi: hunter2-ish log noise\r\nOK wifi pass\r\n")
        lines = link.check('wifi pass "s3cret pass"', echo_safe=False)
        self.assertEqual(lines, ["OK wifi pass"])
        self.assertNotIn("s3cret", " ".join(lines))

    def test_handshake(self):
        good = link_with(lambda c: "STATUS-BEGIN\r\nfw=sparkesp32 version=0.1.0 idf=v5.5.6\r\nSTATUS-END\r\n")
        self.assertTrue(any(ln.startswith("fw=") for ln in good.handshake()))
        bad = link_with(lambda c: "STATUS-BEGIN\r\nfw=other\r\nSTATUS-END\r\n")
        with self.assertRaises(sparklink.LinkError) as e:
            bad.handshake(timeout=0.2, attempt=0.05)
        self.assertIn("not running SparkESP32", str(e.exception))

    def test_handshake_asks_again_while_the_board_boots(self):
        calls = []

        def booting(c):                    # the first two vanish, as they do mid-boot
            calls.append(c)
            return "" if len(calls) < 3 else "STATUS-BEGIN\r\nfw=sparkesp32 version=0.1.0\r\nSTATUS-END\r\n"
        self.assertTrue(link_with(booting).handshake(timeout=2, attempt=0.05))
        self.assertEqual(len(calls), 3)

    def test_handshake_gives_up_on_a_silent_port(self):
        with self.assertRaises(sparklink.LinkError) as e:
            link_with(lambda c: "").handshake(timeout=0.2, attempt=0.05)
        self.assertIn("didn't answer", str(e.exception))

    def test_board_log_lines_are_not_the_reply(self):
        link = link_with(lambda c: "configured=1 connected=0\r\n I (4378) net: joined 'x'\r\n"
                                   "E (5184) esp-tls: delayed connect error\r\nNET-END\r\n")
        self.assertEqual(link.cmd("wifi show"), ["configured=1 connected=0", "NET-END"])

    def test_unrecognized_command_is_an_error(self):
        link = link_with(lambda c: "Unrecognized command\r\n")
        self.assertEqual(link.cmd("bogus"), ["ERR unrecognized command"])

    def test_console_quote(self):
        self.assertEqual(sparklink.console_quote('a "b" \\c'), r'"a \"b\" \\c"')


class ShotTest(unittest.TestCase):
    @staticmethod
    def dump(w, h, strips, corrupt=False):
        out = [f"SHOT-BEGIN {w} {h} rgb565"]
        for (x1, y1, x2, y2) in strips:
            n = (x2 - x1 + 1) * (y2 - y1 + 1) * 2
            data = bytes((x1 + y1 + i) & 0xFF for i in range(n))
            crc = zlib.crc32(data) ^ (1 if corrupt else 0)
            out.append(f"AREA {x1} {y1} {x2} {y2} {n} {crc:08x}")
            out += [base64.b64encode(data[i:i + 72]).decode() for i in range(0, n, 72)]
        return ("\r\n".join(out + ["SHOT-END"]) + "\r\n").encode()

    def test_reassembles_strips(self):
        w, h, fb = devctl.parse_shot(self.dump(4, 4, [(0, 0, 3, 1), (0, 2, 3, 3)]))
        self.assertEqual((w, h, len(fb)), (4, 4, 32))
        self.assertEqual(fb[16], 2)            # first byte of the second strip (x1+y1 = 2)

    def test_rejects_bad_crc_and_gaps(self):
        with self.assertRaises(sparklink.LinkError):
            devctl.parse_shot(self.dump(4, 4, [(0, 0, 3, 3)], corrupt=True))
        with self.assertRaises(sparklink.LinkError):
            devctl.parse_shot(self.dump(4, 4, [(0, 0, 3, 1)]))


if __name__ == "__main__":
    unittest.main(verbosity=1)
