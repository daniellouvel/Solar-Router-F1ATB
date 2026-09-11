# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

F1ATB Solar Router: an Arduino/ESP32 firmware that manages photovoltaic self-consumption by
diverting surplus solar production to loads (water heater, heating, etc.) via SSR/triac
control, instead of exporting it to the grid. It measures power via a Linky TIC meter, analog
current/voltage probes (UxI/UxIx2/UxIx3), or external sources (Shelly EM, Enphase Envoy,
HomeWizard, MQTT), exposes a web UI/API, MQTT, Telnet, and OTA updates.

## Build / flash / monitor

This is a PlatformIO project (Arduino framework). All sketch sources live in `src/` — PlatformIO
concatenates the `.ino` files and generates prototypes the same way the Arduino IDE does, so
function order across `.ino` files generally doesn't matter, but keep new code in one of the
existing `.ino`/`.h` files rather than introducing new top-level files unless there's a real
reason to.

Two environments are defined in `platformio.ini`:
- `esp32s3_n16r8` — the target hardware (ESP32-S3, 16MB flash, 8MB octal PSRAM). Uses
  `partitions_s3_16mb.csv` (6MB per OTA app slot).
- `esp32c3_supermini` — an interim test board (ESP32-C3 SuperMini, 4MB flash, no display) used
  while the S3 hardware isn't available. Uses the root `partitions.csv`, which has only a
  **single** app partition (no A/B OTA) because the compiled firmware (~1.97MB) doesn't fit
  two 1900K OTA slots on 4MB of flash.

```
pio run -e esp32s3_n16r8                                   # build
pio run -e esp32c3_supermini -t upload --upload-port COM14 # build + flash
pio device monitor -p COM14 -b 115200                       # serial monitor
pio run -e esp32c3_supermini -t erase --upload-port COM14   # full flash erase
```

**Erase flash after any change to a `partitions*.csv` file** before reflashing. The LittleFS
config partition (`/parametres.json`, `/EnergieMinuit.eng`) lives at an offset computed from the
partition table; changing the table without erasing leaves stale data at the new offset, which
can be read back as corrupt config on next boot (this has caused real, hard-to-diagnose bugs —
see the out-of-bounds fix below, which was masked by exactly this).

There is no test suite and no linter in this project.

### ESP32-C3 SuperMini quirks (test board only, not the target hardware)

- Opening any serial connection to the C3 resets it (native USB `USB_UART_CHIP_RESET`) — expect
  a reboot every time a monitor/terminal attaches.
- First-batch ESP32-C3 SuperMini boards have a known antenna defect that prevents WiFi
  association at full TX power (scanning/RSSI work fine, association hangs indefinitely). Worked
  around in `src/Solar_Router_V17_26.ino` with `WiFi.setTxPower(WIFI_POWER_8_5dBm)` gated on
  `CONFIG_IDF_TARGET_ESP32C3`, so it doesn't affect the S3 or classic ESP32.

## Architecture

### Board/hardware selection is a runtime enum, not a compile-time target

The firmware supports many physical board variants (plain Wroom, Wroom + relays, several
SPI TFT + resistive/capacitive touch panel combos, WT32-ETH01/ESP32-ETH01 Ethernet) through a
single runtime byte, `ESP32_Type`, persisted in config and checked throughout the codebase
(`ESP32_Type == 10` for Ethernet, `4..9` and `101` for display variants, etc.). The display
variant additionally maps to `ScreenType` in `src/EcranLCD.h` (LovyanGFX panel/touch config).
Porting to a new chip (S2/S3/C3) is a matter of making the low-level bus code (SPI hosts, EMAC)
compile on that chip — the `ESP32_Type` runtime logic itself is chip-agnostic. Two examples
already handled: `HSPI_HOST`/`VSPI_HOST` only exist natively on the classic ESP32 (shimmed onto
`SPI2_HOST`/`SPI3_HOST` in `EcranLCD.h`); EMAC/`ETH_PHY_LAN8720` only exists on the classic
ESP32 (guarded behind `CONFIG_ETH_USE_ESP32_EMAC` in the main `.ino`).

### Power source abstraction

`Source_*.ino` files (Linky, UxI/UxIx2/UxIx3, ShellyEm, ShellyProEm, EnphaseEnvoy, HomeWizard,
SmartG, MQTT, Externe, NotDef) are alternative, mutually-exclusive ways of getting
instantaneous power/energy readings, selected by the `Source` string variable. Each implements
its own polling/parsing and feeds the same set of global power/energy variables consumed by the
routing logic in the main `.ino` (`Actions.cpp`/`.h` for load control) and by the display/web
layers.

### Config persistence

All user settings are one big JSON blob (`SerializeConfiguration()` / `DeserializeConfiguration()`
in `src/Stockage.ino`) written to `/parametres.json` on LittleFS. Adding a new setting means
adding it to both the serialize and deserialize functions (and the two lines around it are the
easiest place to see the naming convention). Daily/persistent energy counters are stored
separately in `/EnergieMinuit.eng`. The project vendors its own modified copy of `OneWire`
directly in `src/` (not the registry library) — `platformio.ini` sets `lib_ignore = OneWire` and
`-Isrc` so `DallasTemperature` (an external lib) resolves to the project's copy instead of
pulling in `paulstoffregen/OneWire`.

### Multi-router mesh

Up to `LES_ROUTEURS_MAX` (8) F1ATB routers can reference each other by IP (`RMS_IP[]`) and poll
each other's `/ajax_Noms` endpoint (`src/RMS_Externes.ino`) to share temperature/action state
across units. Index 0 is always "this router" (no network call); indices 1-7 are remote peers.

### Web/JS layer

The HTTP server (`src/Server.ino`) serves HTML/JS assembled from large C-string literals split
across `Page*.h` (HTML shells) and `JS_*.h` / `PageHtmlJS_*.h` (JS, one file per page/feature:
Accueil, Actions, Brute, Para, Connect, Couleurs, Export, Heure, OTA). There's no build step for
the frontend — these are static strings served directly, so edits are made in-place in the
relevant `.h` file and take effect on next flash.

### Command interface

Serial and Telnet share one line-based command parser (`LireSerial()` / `DecodeSerial()` in
`src/commonFx.ino`): lines are split on the first `:`, e.g. `ssid:xxx`, `password:xxx`,
`restart`, `ETH01`. This is the only way to configure WiFi credentials on a headless board (no
display) before it has network access, since the device otherwise falls back to a WiFi AP
(`RMS-ESP32-<chipid>`) whose 5-minute `WIFI_AP_STA` timeout auto-resets the board if left
unconfigured.
