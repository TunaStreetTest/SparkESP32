# Troubleshooting

Start with the state in the top-right corner of the screen, then the three commands that
explain almost everything:

```bash
systemctl status sparkesp32                       # on the Spark: is the agent running?
curl -s localhost:8787/healthz | python3 -m json.tool   # what it samples; is the display connected?
journalctl -u sparkesp32 -n 50 --no-pager         # agent log
```

To see the display's side of things, plug it into any machine with this repo and
`pip install pyserial`, stop the agent first if that machine is the Spark
(`sudo systemctl stop sparkesp32`), and run:

```bash
python3 tools/devctl.py status     # firmware version, settings, last sample
python3 tools/devctl.py wifi       # Wi-Fi link, poll counters, last_err
```

Start the agent again afterwards (`sudo systemctl start sparkesp32`): only one program
can talk to the display's serial port at a time.

## The screen is black

- Try another cable. Many USB-C cables are charge-only, and the board needs data to be
  flashed and fed (it powers up on either).
- `lsusb | grep 303a:1001` on the Spark. If it's missing, the board isn't enumerating:
  check the cable and the port, then `dmesg | tail`.
- A brand-new board runs Waveshare's demo firmware until you install. Run `sudo ./install.sh`.

## WAITING, with the board plugged into the Spark

Nothing has arrived over USB yet.

- `systemctl status sparkesp32`. If it isn't running, `journalctl -u sparkesp32` says why.
- `/healthz` → `"display"`:
  - `not connected`: the agent can't see a SparkESP32 on USB. Make sure no other
    program has the port open (`screen`, `minicom`, `devctl.py`, an IDE monitor)
    with `sudo fuser -v /dev/ttyACM*`. Then re-run `sudo ./install.sh`, which also
    reflashes if the board isn't running SparkESP32.
  - The log says `is an Espressif USB device but not a SparkESP32`: the board answered
    the handshake with different firmware. Re-run `sudo ./install.sh` to flash it.
- ModemManager probing the port garbles the console. The installer's udev rule stops that.
  Check it's present: `cat /etc/udev/rules.d/99-sparkesp32.rules`.

## OFFLINE

The link is up but samples stopped. The Spark is asleep or off, or the agent is down.

- Over USB: the agent stopped (`systemctl status sparkesp32`) or the Spark is off.
- Over Wi-Fi: run `devctl.py wifi` on a machine the board is plugged into and read `last_err`:

| `last_err` | Meaning | Fix |
|---|---|---|
| `connect ESP_ERR_HTTP_CONNECT` | can't reach the agent | Is `<host>.local` resolvable on your LAN? Try `--host <ip>`. Is ufw allowing your subnet (`sudo ufw status`)? Is the board on an isolated guest/IoT network? |
| `http 503` | agent is up but its sampler stalled | `journalctl -u sparkesp32`; `sudo systemctl restart sparkesp32` |
| `http 404` | something else answers on that port | check the URL: `devctl.py wifi` shows it; fix with `--host` |
| `bad stat line (too many fields: update the agent)` | the agent speaks an older protocol | `git pull && sudo ./install.sh` on the Spark |
| `bad stat line (<field>)` | a value out of range | open an issue with the `/stat` output |

From another machine on the same LAN, `curl http://<spark>.local:8787/stat` should
print one `stat ...` line.

## NO WI-FI

Wi-Fi is configured but the board can't join. `devctl.py wifi` shows `last_err` as
`join '<ssid>' reason <n>`:

| reason | Meaning | Usually |
|---|---|---|
| 201 | no AP found | wrong name, or the network is 5/6 GHz only (the ESP32-C6 is **2.4 GHz only**) |
| 15, 204 | 4-way handshake timeout | wrong password |
| 202 | authentication failed | wrong password, or WPA3-only network with incompatible settings |
| 210, 211 | no AP with compatible security | the network needs something other than WPA2/WPA3-Personal (e.g. 802.1X), or it's WEP/open-only with odd settings |
| 2, 200 | auth expired / beacon timeout | weak signal; move the board closer |

`python3 tools/devctl.py wifi scan` lists what the board can see, with signal strength
and security. To fix the settings, re-run `sudo ./install.sh --ssids "<name>"` (the
password is prompted). The board rotates through up to 4 SSIDs, moving to the next
after 3 failed joins.

## SET HOST

The board is on Wi-Fi but has no agent address. Run `sudo ./install.sh --host <spark-name>.local`,
or `--host <ip>`.

## GPU, temperature or power show `--`

The agent couldn't read them from `nvidia-smi`. Check `/healthz` → `"gpu_source"`:

- `nvidia-smi unavailable`: the NVIDIA driver/tools aren't installed or aren't on `PATH`
  for the service.
- `nvidia-smi exited (...)`: it crashed or the driver isn't loaded; `nvidia-smi` by hand
  will say why. The agent restarts it with backoff.

CPU, memory and the core heatmap don't depend on `nvidia-smi`.

## The image is sideways

`sudo ./install.sh --rotation none|ccw|cw`. The default `cw` is upright with the board's
USB-C port on the right.

## Flashing fails

- Hold the board's **BOOT** button while plugging it in (forces the ROM bootloader), then
  re-run `sudo ./install.sh --reflash`.
- `several Espressif USB devices attached`: pick one with `--port /dev/ttyACMn`.

## Starting over

```bash
sudo ./install.sh --uninstall         # service, udev rule, ufw rule, /opt/sparkesp32
python3 tools/devctl.py wifi clear    # optional: make the board forget its Wi-Fi
```

The board keeps its name and Wi-Fi settings across reinstalls and firmware updates.
Only a merged-image flash or `wifi clear` erases them.
