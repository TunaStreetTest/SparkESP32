# SparkESP32 developer targets. Firmware targets need ESP-IDF v5.5.x exported
# (. ~/esp/esp-idf/export.sh); `make check` needs only python3 (+ ruff/shellcheck
# if installed).
VERSION := $(shell head -n1 VERSION)
DIST    := dist
PORT    ?=
PY      ?= python3

.PHONY: build flash monitor dist check clean

build:
	cd firmware && idf.py build

# split images only: keeps the board's saved settings (see install.sh)
flash: build
	cd firmware && idf.py $(if $(PORT),-p $(PORT)) flash

monitor:
	cd firmware && idf.py $(if $(PORT),-p $(PORT)) monitor

# Release bundle, what install.sh and the GitHub release consume:
#   dist/sparkesp32-firmware-X/       split images + flash_args + SHA256SUMS
#   dist/sparkesp32-firmware-X.tar.gz that directory, tarred (release asset)
#   dist/sparkesp32-X-merged.bin      one image at 0x0 for web flashers; fresh boards
#                                     only, it erases saved settings
PKG := sparkesp32-firmware-$(VERSION)
SHA := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo shasum -a 256)

dist: build
	rm -rf $(DIST) && mkdir -p $(DIST)/$(PKG)
	cp firmware/build/bootloader/bootloader.bin firmware/build/partition_table/partition-table.bin \
	   firmware/build/sparkesp32.bin $(DIST)/$(PKG)/
	printf '%s\n' '--flash_mode dio --flash_freq 80m --flash_size 16MB' '0x0 bootloader.bin' \
	   '0x8000 partition-table.bin' '0x10000 sparkesp32.bin' > $(DIST)/$(PKG)/flash_args
	cd $(DIST)/$(PKG) && $(SHA) bootloader.bin partition-table.bin sparkesp32.bin > SHA256SUMS
	cd $(DIST)/$(PKG) && python -m esptool --chip esp32c6 merge_bin \
	   -o ../sparkesp32-$(VERSION)-merged.bin @flash_args
	tar -C $(DIST) -czf $(DIST)/$(PKG).tar.gz $(PKG)
	cd $(DIST) && $(SHA) $(PKG).tar.gz sparkesp32-$(VERSION)-merged.bin > SHA256SUMS
	@ls -l $(DIST)

check:
	$(PY) -m py_compile agent/sparkesp32_agent.py tools/sparklink.py tools/provision.py tools/devctl.py
	$(PY) tests/test_agent.py
	@mkdir -p tests/.build
	$(CC) -std=c11 -Wall -Wextra -Werror -Itests/stubs -Ifirmware/main \
	   tests/test_parser.c firmware/main/spark_data.c -o tests/.build/test_parser
	tests/.build/test_parser
	$(CC) -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
	   tests/test_states.c firmware/main/link.c firmware/main/status.c -o tests/.build/test_states
	tests/.build/test_states
	@grep -q '^__version__ = "$(VERSION)"$$' agent/sparkesp32_agent.py \
	   || { echo "agent __version__ != VERSION ($(VERSION))"; exit 1; }
	@if command -v ruff >/dev/null; then ruff check agent tools tests; \
	 elif command -v uvx >/dev/null; then uvx ruff check agent tools tests; \
	 else echo "(ruff not installed: skipped)"; fi
	@if command -v shellcheck >/dev/null; then shellcheck install.sh; else echo "(shellcheck not installed: skipped)"; fi
	@echo "check: ok"

clean:
	rm -rf firmware/build firmware/managed_components $(DIST)
