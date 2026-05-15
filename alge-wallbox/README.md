# alge-wallbox

ESP32-S3 firmware for the FC Wängi 1967 scoreboard wall-box.

Receives ESP-NOW state from the `alge-controller` handheld and drives the
34-year-old Alge GAZ4 flip-segment scoreboard at the Grosswies stadium over
2400 baud RS232 (via a MAX3232 level shifter and two banana plugs).

**Target hardware**: LilyGO T-Display-S3 (ESP32-S3, 1.9" 170×320 ST7789 IPS).

---

## Wiring

```
LilyGO T-Display-S3              Mini MAX3232 (TTL side)
─────────────────                ───────────────────────
+5V        ────────────────────► VCC
GND        ────────────────────► GND  (TTL)
GPIO 17    ────────────────────► TIN  (T1IN)
                                 T1OUT (RS232 side) ──► Banana plug A (TX, "yellow")
                                 GND  (RS232 side)  ──► Banana plug B (GND, "black")

USB-C   (via M12 cable gland)  ► power + flashing
IO14    on-board button        ► info-page / blank / factory-reset
```

Bananas plug into the unmarked sockets on the clubhouse wall plate. If the
scoreboard doesn't respond on first power-up, the polarity is reversed:
unplug, rotate the wall-box 180°, plug back in. The firmware's polarity-test
mode (auto-run on first boot, before any match) helps confirm this.

## Build

PlatformIO is required.

```bash
# install platformio if you don't have it
pip3 install --user platformio

# build
cd alge-wallbox
pio run

# flash + monitor
pio run -t upload
pio device monitor
```

Filesystem upload is not used — the crest is compiled into firmware via
`tools/make_crest.py` (run from the repo root before the first build, after
each crest change):

```bash
python3 tools/make_crest.py
```

## IO14 button shortcuts

| Press                  | Behaviour                                          |
|------------------------|----------------------------------------------------|
| Short (<1s)            | Cycle info pages — or "polarity OK" if in test     |
| Long (2-4s)            | Emergency-blank the scoreboard — or "polarity bad" |
| Very long (>10s)       | Factory reset (clears NVS + reboots into pairing)  |

## What the display shows

`config.h` defines colour palette; `display.cpp` defines the rendering logic.
States cycle through: BOOT → PAIRING → POLARITY_TEST (first boot only) →
PAIRED_IDLE → MATCH_LIVE. Special states: BLANK_BURST, SEGMENT_EXERCISE,
CONNECTION_LOST, ERROR.

The footer always shows polarity status, link state (Funk), and a TX-blink
indicator that pulses while frames are going to the scoreboard.

## GAZ4 timing (from `HANDOFF.md`)

| Event              | Frame repeats | Gap (ms) |
|--------------------|---------------|----------|
| Refresh while live | 1             | —        |
| Score change       | 4             | 100      |
| State transition   | 2             | 200      |
| Blank              | 8             | 80       |
| Segment exercise   | continuous    | 100 (10Hz) |

## Troubleshooting

- **Display blank, no boot text**: TFT pin assignments wrong. Re-check
  `platformio.ini` build flags against the T-Display-S3 schematic.
- **No ESP-NOW pairing**: ensure both devices are within ~10m and on the
  same WiFi channel (we use ESP-NOW so this is auto, but `WiFi.mode(WIFI_STA)`
  must run before `esp_now_init`).
- **Scoreboard ignores frames**: check polarity. The polarity test pattern
  is `000 8 8    88 88\r` — all 8s should appear. If nothing, swap polarity
  (physically rotate the wall-box). If still nothing, the GAZ4's internal
  mode switch may have been bumped (see HANDOFF.md section 3).
- **Some digits stuck on the wrong segment**: trigger the segment-exercise
  command from the controller's Settings screen (Wartung → Segment-Übung).
  Runs 30s at 10 Hz on digit position 11 (top-left clock).

## Wireless reflash (sealed enclosure → no USB-C access)

The wall-box hosts its own WiFi AP so you can OTA it without unmounting.
SSID/password/hostname live in `src/credentials.h` (gitignored). Before
your first build:

```bash
cp src/credentials.example.h src/credentials.h
# edit src/credentials.h to set a real password
```

Then:

```bash
pio run -t upload --upload-port alge-wallbox.local
```

ESP-NOW continues to work while the AP is up — both are locked to WiFi
channel 1. The AP costs ~30 mA extra; the wall-box is USB-powered so this
is irrelevant.

## Simulating in Wokwi (no hardware needed)

```bash
# Build the SIM env, which uses an SPI ST7789 instead of 8-bit parallel.
pio run -e wokwi-s3

# Then open this folder in VS Code with the Wokwi extension installed.
# (cmd-shift-P → "Wokwi: Start Simulator")
```

The diagram.json wires an SPI ST7789 mock display + an IO14 pushbutton to
an ESP32-S3-DevKitC-1. The 170×320 logical resolution is preserved in the
firmware, even though the simulated panel renders at 240×240. ESP-NOW
between two Wokwi tabs requires the Wokwi Gateway / IoT plan; for
free-tier testing, run each device in its own tab to verify UI behaviour
in isolation.

## Repository layout

```
alge-wallbox/
├── platformio.ini
└── src/
    ├── main.cpp          orchestration + GAZ4 TX scheduler
    ├── config.h          pins, colours, timing, brand strings
    ├── state.h / .cpp    local mirror of scoreboard state
    ├── gaz4.h / .cpp     frame builder + UART transmitter
    ├── espnow_server.*   ESP-NOW recv + pairing + heartbeat
    ├── display.*         T-Display rendering
    ├── button.*          IO14 short/long/very-long press
    ├── polarity.*        polarity flag NVS storage
    ├── maintenance.*     blank, polarity-test, segment-exercise, raw frame
    ├── crest.*           FC Wängi crest bitmap (auto-generated)
    └── shared/
        └── message.h     ESP-NOW message protocol (must match alge-controller)
```
