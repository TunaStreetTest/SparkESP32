# Changelog

All notable changes to this project are documented here.

## 0.1.0 — 2026-11-20

First public release.

- Firmware for the Waveshare ESP32-C6-Touch-AMOLED-2.16 shows CPU, GPU and
  unified-memory rings, a per-core heatmap, device temperature and power, and an
  ONLINE / OFFLINE / NO WI-FI / SET HOST / WAITING state.
- Two transports carry one line format (protocol v1, [docs/protocol.md](docs/protocol.md)):
  - USB serial when the board is plugged into the Spark.
  - A 1 Hz HTTP poll of the Spark's agent over a LAN Wi-Fi network, with WPA2/WPA3
    and up to 4 SSIDs tried in order.
- The display's title is the Spark's hostname, sent live by the agent, unless a name is set.
- The `sparkesp32` agent is a systemd service that samples `nvidia-smi` and `/proc`
  once a second. It pushes samples over USB to a device that passes the
  firmware handshake, and serves `/stat` and `/healthz`.
- `install.sh` runs on the Spark. It flashes the firmware (split images that
  keep the board's settings), configures name, host and Wi-Fi (password typed at
  a hidden prompt), installs the service, the udev rule and the ufw rule, and
  verifies the result end to end.
- Host tools: `tools/provision.py` for setup from any computer and
  `tools/devctl.py` for QA and CRC-checked screenshots.
- Tests: the firmware's protocol parser runs on the host, along with the agent,
  the serial tools and screenshot reassembly.
