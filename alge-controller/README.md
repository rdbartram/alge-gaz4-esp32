# alge-controller

ESP32 firmware for the FC Wängi 1967 scoreboard handheld controller.

Touchscreen UI for full football match management. Sends state to the
`alge-wallbox` over ESP-NOW; the wall-box renders frames to the Alge GAZ4
flip-segment scoreboard at the Grosswies stadium.

**Target hardware**: M5Stack Core2 V1.1 (ESP32, 2.0" 320×240 capacitive
touchscreen, 500 mAh LiPo, USB-C, vibration motor).

---

## Build

```bash
pip3 install --user platformio
cd alge-controller
pio run
pio run -t upload
pio device monitor
```

Before the first build, generate the crest C arrays (one-off):

```bash
python3 tools/make_crest.py
```

## UI tour

| Screen          | Purpose                                                      |
|-----------------|--------------------------------------------------------------|
| Splash          | Crest + version, auto-advances after ~2s                     |
| Setup           | Pick match type, enter opponent name, MATCH STARTEN          |
| Match (live)    | Big score numerals, +/- tap, clock, PAUSE/RESUME, HALBZEIT   |
| Halftime        | End-of-1.HZ score + pause countdown, "2.HZ STARTEN"          |
| Ended           | Final score + Verlängerung/Penalty options, auto-blank in 5m |
| Penalty shootout| Per-kick ✓/✗ tracker for both teams                          |
| Settings        | Funk diagnostics, Wartung (blank/polarity/exercise), History |

Match-type presets are defined in `matchmodes.cpp` and cover the OFV
categories FC Wängi plays under (1.MS, 2./3.MS, Senioren 30+/40+,
B-/Ca-/D-/E-/F-/G-Junioren, Freundschaftsspiel).

## Vibration feedback

| Event                  | Pattern                                |
|------------------------|----------------------------------------|
| Score +1               | 50 ms buzz                             |
| Score −1 (correction)  | Two 30 ms buzzes, 100 ms apart         |
| Clock pause/resume     | 80 ms buzz                             |
| Halftime / match end   | Long 500 ms buzz                       |

## State persistence (NVS)

On every score/state change, the current match is written to NVS. If the
device reboots mid-match, the splash screen detects the persisted state and
the match resumes (always paused — the operator confirms before continuing).

History keeps the last 5 matches: timestamp, preset, final score, opponent.

## Pairing

If no peer is saved, the controller broadcasts `MSG_PAIRING` once per
second. The wall-box does the same. Once they see each other, both save
MAC addresses to NVS and stay paired across reboots.

To re-pair: Settings → "Neu koppeln". On the wall-box, hold IO14 during
boot to force pairing mode there too.

## Troubleshooting

- **Touch unresponsive**: ensure `M5.update()` is called every loop
  (it is — see `ui.cpp`). Re-flash if frozen.
- **"Funk" shows nothing**: not paired yet. Trigger re-pair from Settings.
- **Score won't budge past 9 on the board**: the GAZ4 only has 0-9 per
  digit. The controller shows the real score; the board wraps.

## WiFi AP + live score page + OTA

The controller hosts its own WiFi AP — **no internet needed at the stadium**.
SSID, password, OTA password, and hostname all live in `src/credentials.h`
(gitignored). Before your first build:

```bash
cp src/credentials.example.h src/credentials.h
# edit src/credentials.h to set a real password
```

Once flashed and connected:

- Open `http://192.168.4.1/` → live HTML score page, auto-refresh 2s
- `GET /score.json` → JSON for any external integration
- ArduinoOTA listens on port 3232 for wireless firmware updates:

```bash
pio run -t upload --upload-port alge-controller.local
# or
pio run -t upload --upload-port 192.168.4.1
```

## Simulating in Wokwi (no hardware needed)

```bash
pio run                                            # build firmware
# Then open this folder in VS Code with the Wokwi extension installed.
# (cmd-shift-P → "Wokwi: Start Simulator")
```

The Core2 is officially supported in Wokwi — display, capacitive touch,
A/B/C buttons, vibration motor, IMU, and battery are all modeled.
ESP-NOW pairing with a sim wall-box requires the Wokwi Gateway / IoT
plan; for free-tier testing, run each device in its own tab.

## Repository layout

```
alge-controller/
├── platformio.ini
└── src/
    ├── main.cpp           orchestration
    ├── config.h           brand + colours + timing
    ├── state.h / .cpp     match state machine + NVS persistence + history
    ├── matchmodes.h / .cpp OFV preset table
    ├── espnow_client.*    ESP-NOW pairing + state push
    ├── ui.h / ui.cpp      all screens + dispatcher
    ├── ui_helpers.*       widgets (buttons, big numerals, header)
    ├── crest.*            FC Wängi crest bitmap (auto-generated)
    └── shared/
        └── message.h      ESP-NOW message protocol (must match alge-wallbox)
```
