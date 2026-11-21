# Contributing

Thanks for helping improve SparkESP32.

## Before opening an issue

- Search existing issues first.
- Use the bug-report template for reproducible problems and include the output
  it asks for. Most issues are answered by `curl localhost:8787/healthz` and
  `tools/devctl.py status`.
- Don't include Wi-Fi passwords, your network's hostnames or addresses, or
  other private data.

## Development setup

Requirements:

- python3 (3.10+) and a C compiler for `make check`. Nothing else is needed for
  the agent, tools and tests.
- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c6/get-started/)
  with the esp32c6 target, for firmware.
- The Waveshare ESP32-C6-Touch-AMOLED-2.16, for anything you want to see on the screen.

```sh
make check                      # always: unit tests, parser tests, ruff, shellcheck
. ~/esp/esp-idf/export.sh
make build                      # firmware
make flash PORT=/dev/ttyACM0    # split images: keeps the board's saved settings
python3 tools/devctl.py demoshot /tmp/s.png 70   # synthetic load + screenshot
```

Read [AGENTS.md](AGENTS.md) before changing the wire protocol, the console
replies, or the installer. It lists what has to change together.

## Pull requests

- Keep each pull request focused.
- Add or update tests for behavior changes. `tests/test_parser.c` covers the
  firmware parser, and `tests/test_agent.py` covers the agent and tools.
- Firmware changes: build with no new warnings in `firmware/main` or
  `firmware/components/board`, and check the result on hardware (include a
  `devctl.py shot` screenshot for UI changes).
- Update the README or `docs/` when user-visible behavior changes, and add a
  `CHANGELOG.md` entry.
- Run `make check` before submitting.
- Don't copy vendor sample code without a license that allows it. The board
  layer in `firmware/components/board` is original for that reason.

By participating, you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
