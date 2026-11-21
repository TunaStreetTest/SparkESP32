# AGENTS.md

Instructions for AI coding agents working in this repo: setting SparkESP32 up for
someone, or changing it. Humans: [README.md](README.md) is the friendlier version.

## Hard rules

1. **Never handle the Wi-Fi password.** Don't ask for it in chat, don't put it in a
   command line, a file, or an environment variable you set. The installer and
   `tools/provision.py` prompt for it on a hidden TTY prompt; let the human type it. If
   you have no TTY, set up USB-only and tell the human the exact command to run themselves.
2. **Run the installer on the Spark, with sudo, from the repo root.** It needs root for
   systemd/udev/ufw, and it runs the agent as the invoking (non-root) user.
3. **Flash split images only** (bootloader 0x0, partition table 0x8000, app 0x10000).
   The merged image overwrites the NVS partition and the board forgets its name and Wi-Fi.
   The installer and `make flash` already do this right.
4. **Don't change settings the human didn't ask for.** The installer only fills in unset
   values unless given a flag. Keep it that way.

## Setting it up for someone

Preconditions: you have a shell on the DGX Spark (or can ask the human to run commands
there), and the display is plugged into one of the Spark's USB-C ports with a data cable.

```bash
git clone https://github.com/twidtwid/SparkESP32.git && cd SparkESP32
lsusb | grep -i 303a:1001          # the display: "Espressif USB JTAG/serial debug unit"
```

Decide with the human (ask; don't guess):

- **Display name.** By default the title is the Spark's hostname, sent live by the agent;
  pass nothing. Only use `--name` if they want something else (up to 24 printable ASCII
  characters). `--name ""` returns to the hostname.
- **Wi-Fi or USB-only.** USB-only needs nothing else. For Wi-Fi you need the network
  name(s); the board is 2.4 GHz only.

Then run **one** of these. If you can't type into a TTY yourself, hand the human the
exact line:

```bash
# USB only, no questions
sudo ./install.sh --yes --no-wifi

# with Wi-Fi: the human types the password at the hidden prompt
sudo ./install.sh --ssids "HomeNet"

# fully unattended with Wi-Fi: the HUMAN exports the variable in their own shell first
sudo --preserve-env=WIFI_PSK ./install.sh --yes --ssids "HomeNet" --wifi-password-env WIFI_PSK
```

Useful flags: `--host <ip>` when mDNS (`<hostname>.local`) doesn't resolve on their LAN;
`--port /dev/ttyACMn` if several Espressif boards are attached; `--no-flash` to leave
the firmware alone; `--lan <cidr>` to override the subnet ufw allows. `./install.sh --help`
lists everything.

### Definition of done

All of these, checked by you, not assumed:

```bash
systemctl is-active sparkesp32                         # -> active
curl -fsS localhost:8787/stat                          # -> stat <cpu> <gpu> ... (8 fields)
curl -fsS localhost:8787/healthz | python3 -m json.tool   # "display": "connected on /dev/ttyACM0"
```

- The screen reads **ONLINE · via USB** (ask the human to look, or check
  `"display": "connected ..."` in `/healthz` and that `age_s` < 2).
- If Wi-Fi was set up: unplug from the Spark, power the board from any USB charger, and
  within ~20 s it reads **ONLINE · via WI-FI**. If it can't, `tools/devctl.py wifi` on a
  machine it's plugged into shows `last_err` (see docs/troubleshooting.md).
- Tell the human: the name shown, how it's connected, and the change command
  (`sudo ./install.sh --name ...`).

## Repo map

```
install.sh                 Spark-side installer (flash, provision, service, firewall, verify)
VERSION                    the one version: firmware (CMake reads it), agent, installer
agent/
  sparkesp32_agent.py      sampler + USB push + HTTP /stat,/healthz (stdlib + pyserial)
  sparkesp32.service       systemd unit template (@RUN_USER@)
  99-sparkesp32.rules      udev: ModemManager ignore for 303a:1001
  requirements.txt         pinned pyserial + esptool for /opt/sparkesp32/venv
tools/
  sparklink.py             serial plumbing: find by USB ID, handshake, run-until-terminator
  provision.py             set name/host/Wi-Fi/rotation over USB (never prints the password)
  devctl.py                bench/QA: status, demo, CRC-checked screenshots, raw console
firmware/                  ESP-IDF 5.5 project (target esp32c6)
  main/spark_data.c        the protocol parser both transports share
  main/net.c               Wi-Fi supervisor + 1 Hz HTTP poll
  main/ui.c                LVGL screen
  main/console_cmds.c      USB console commands (the agent's and the tools' interface)
  components/board/        AXP2101 PMIC, QSPI AMOLED panel, LVGL rotation (our code)
  components/xpowerslib/   vendored XPowersLib (MIT), unmodified
tests/                     host tests: agent + tools (Python), parser (C, stubbed esp_timer)
docs/                      protocol.md, troubleshooting.md
```

## Changing the code

- `make check` must pass. It needs only python3 and a C compiler, and covers py_compile,
  unit tests, the C parser tests, ruff (via `uvx` if not installed) and shellcheck.
- Firmware: `. ~/esp/esp-idf/export.sh` (ESP-IDF **v5.5.x**), then `make build`. Treat
  any new warning in `firmware/main` or `firmware/components/board` as an error.
- Hardware check after firmware changes: `make flash`, then
  `python3 tools/devctl.py status` (first data line must be `fw=sparkesp32 version=...`),
  then `python3 tools/devctl.py demoshot /tmp/s.png 70` and look at the PNG.
- **Protocol changes** touch four places at once: `firmware/main/spark_data.c`,
  `agent/sparkesp32_agent.py`, `tests/`, and `docs/protocol.md`. The console splits on
  spaces with a 32-token cap, so keep each sample to a few tokens.
- Console replies always end in a line starting with `OK` or `ERR` (or a `*-END` marker
  for multi-line output, which an `ERR ...` line can replace when the command can't run).
  `tools/sparklink.py` depends on that.
- Bump `VERSION` and `__version__` in the agent together (`make check` enforces it), and
  add a `CHANGELOG.md` entry.

## Releasing

Tag `vX.Y.Z` matching `VERSION`. `.github/workflows/release.yml` builds the firmware and
attaches `sparkesp32-firmware-X.Y.Z.tar.gz` (split images + `flash_args` + `SHA256SUMS`),
the merged image, and checksums. `install.sh` downloads that tarball when the repo has no
local build.
