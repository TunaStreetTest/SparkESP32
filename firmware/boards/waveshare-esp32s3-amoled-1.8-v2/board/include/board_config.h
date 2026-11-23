#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.8, hardware V2: a 368x448 color AMOLED (CO5300 on
// QSPI) behind an AXP2101 PMIC and a TCA9554 IO expander. Display only: the touch
// panel, IMU, RTC, audio codec and microSD are unused. The panel is taller than wide,
// so the board offers both shapes; the C6 AMOLED is the reference
// (see firmware/boards/README.md).

#define BOARD_ID            "waveshare-esp32s3-amoled-1.8-v2"
#define BOARD_UI_W          448     // landscape (rot cw, the default, or ccw); rot none is
#define BOARD_UI_H          368     // portrait, 368x448, with a layout of its own in ui.c
#define BOARD_DISPLAY_MONO  0
