# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Upload Commands

**Compile:**
```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi \
  .
```

**Upload** (adjust port as needed — `/dev/cu.usbmodem101` is typical):
```bash
arduino-cli upload \
  -p /dev/cu.usbmodem101 \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi \
  .
```

**Serial monitor:**
```bash
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```

**Prerequisites:** `esp32:esp32` board package v3.3.6, `lvgl` library v8.3.9.

> If upload fails, hold **Boot + RST** buttons on the board to enter bootloader mode.

> Arduino CLI requires the folder name and `.ino` filename to match exactly (`ESP32_HelloWorld_AG/` → `ESP32_HelloWorld_AG.ino`). Do NOT use symlinks — Arduino merges all `.ino` files, causing redefinition errors.

## Architecture

### Layer Stack

```
ESP32_HelloWorld_AG.ino   ← Application: UI + BLE gamepad logic
         │
    BSP Layer
   esp_bsp.c/h            ← All-in-one init: QSPI display, backlight (PWM), touch (I2C)
   display.h              ← Pin defines, resolution (320×480), color format (RGB565 BE)
         │
   esp_lcd_axs15231b.c/h  ← AXS15231B QSPI panel driver (67-cmd init sequence)
   esp_lcd_touch.c/h      ← Capacitive touch driver (I2C)
         │
   lv_port.c/h            ← LVGL ↔ hardware bridge (FreeRTOS task, DMA flush, TE vsync)
   lv_conf.h              ← LVGL v8.3.9 config (fonts 8–48 all enabled, all standard widgets on)
```

### Key Conventions

- **All LVGL UI writes must be wrapped** with `bsp_display_lock(0)` / `bsp_display_unlock()`. The LVGL task runs on a separate FreeRTOS core.
- **BLE callbacks run in the BLE task context** — never call LVGL APIs from `notifyCallback` or `onDisconnect`. Use volatile flags/structs to pass data to the main loop.
- `processGamepad()` is called from `loop()` and handles the BLE state machine (connect, auto-rescan every 5 s).

### BLE HID Client (IINE L1161)

- ESP32-S3 acts as **BLE Central**; gamepad is Peripheral with HID Service UUID `0x1812`.
- **Security must be set before** `pClient->connect()` — use `ESP_LE_AUTH_BOND` (not SC).
- After connect, wait **1 s** before calling `getService()` to allow service discovery.
- Subscribe via both `registerForNotify()` **and** direct CCCD write (`0x2902 = {0x01,0x00}`).
- HID Report is 17 bytes: `[0]=Report ID, [1]=JoyLX, [2]=JoyLY, [4]=hat/dpad, [5]=buttons, [16]=0x01`.
- Idle filter: report is ignored if all bytes are `0x00`, `0x7F`, or `0x80`.

### Hardware

- **MCU:** ESP32-S3, Dual-Core LX7 @ 240 MHz, 16 MB Flash, 8 MB PSRAM (OPI)
- **Display:** 3.5" TFT, AXS15231B IC, QSPI (SPI2_HOST), 320×480, RGB565, backlight on GPIO1 (LEDC PWM)
- **Touch:** I2C on GPIO8 (SCL) / GPIO4 (SDA) — note GPIO8 is shared with DC signal
- **Core version:** `esp32:esp32` v3.3.6 (ESP-IDF 5.1). Libraries targeting Core v2.x (Bluepad32, ESP32-BLE-Gamepad) are **incompatible**.
