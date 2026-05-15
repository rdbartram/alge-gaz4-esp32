# alge-gaz4-esp32

ESP32 firmware that resurrects a 34-year-old **Alge GAZ4** flip-segment
scoreboard whose original TIMY3 W controller died. The protocol was
reverse-engineered from scratch; what's below is sufficient to drive the
board without ever having seen the original handheld.

If you have a dead-controller Alge GAZ4 sitting in a clubhouse, this
repo is for you.

---

## How it works

```
       ┌──────────────────┐                ┌──────────────────┐
       │  alge-controller │                │   alge-wallbox   │
       │   M5Stack Core2  │                │ LilyGO T-Display │
       │  touch handheld  │   ESP-NOW      │     -S3 (sealed) │
       │                  │  ──────────►   │                  │
       │ • match clock    │  ch 1, ~50 m   │ • renders state  │
       │ • +/- scores     │   line-of-sight│ • builds GAZ4    │
       │ • halftime, ET,  │                │   17-byte frames │
       │   penalties      │                │ • UART 2400 8N1  │
       │ • undo, stoppage │                │ • polarity test  │
       └──────────────────┘                └────────┬─────────┘
                                                    │ GPIO 17 TX
                                                    ▼
                                           ┌──────────────────┐
                                           │     MAX3232      │
                                           │ 3.3V TTL ──► ±5V │
                                           └────────┬─────────┘
                                                    │ T1OUT + GND
                                                    │  2× banana plugs
                                                    │  (polarity unmarked
                                                    │   on this board)
                                                    ▼
                                  ~100 m of buried cable into the field
                                                    │
                                                    ▼
                                ┌────────────────────────────────────┐
                                │   Alge GAZ4 scoreboard (1991)      │
                                │                                    │
                                │    ┌──┐┌──┐  ┌──┐┌──┐              │
                                │    │M ││M │  │S ││S │   ← MM:SS    │
                                │    └──┘└──┘  └──┘└──┘              │
                                │       ┌──┐    ┌──┐                 │
                                │       │H │    │G │   ← HEIM : GAST │
                                │       └──┘    └──┘                 │
                                │                                    │
                                │  Bistable flip-segment, yellow on  │
                                │  black, 6 digits total             │
                                └────────────────────────────────────┘
```

Two ESP32s because the controller wants to be a portable handheld on
the touchline, and the wall-box has to plug into a fixed wall socket in
the clubhouse. They talk over **ESP-NOW** (peer-to-peer WiFi radio, no
router required).

---

## Bill of materials

| Item | Qty | ~Price (CHF) | Purpose |
|---|---:|---:|---|
| [M5Stack Core2 V1.1](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit-v1-1) | 1 | 50 | Controller — touchscreen handheld, ESP32, 320×240 IPS, 500 mAh battery |
| [LilyGO T-Display-S3](https://www.lilygo.cc/products/t-display-s3) | 1 | 25 | Wall-box — ESP32-S3 + 1.9" 170×320 IPS, USB-C |
| Mini MAX3232 module | 1 | 4 | RS232 level shifter (3.3 V TTL → ±5 V) |
| 4 mm panel-mount banana plugs | 2 | 6 | Mate with the scoreboard's wall socket (Schutzinger FK 1210 Ni or similar) |
| Plastic IP66 enclosure (~80×60×30 mm) | 1 | 12 | Houses the wall-box; transparent lid helps see the IPS display |
| M12 cable gland | 1 | 3 | USB-C entry into the sealed enclosure |
| Long USB-C cable | 1 | 8 | Wall-box power |
| Dupont jumper wires | ~6 | 2 | Internal wiring |
| **Total** | | **~CHF 110** | |

For development: a USB-RS232 adapter (FT232RNL ~ CHF 12) + banana test
leads let you bench-test the protocol before the wall-box is built.

---

## Wiring (wall-box)

```
LilyGO T-Display-S3              Mini MAX3232 (TTL side)
─────────────────                ───────────────────────
   3.3 V  ────────────────────► VCC
   GND    ────────────────────► GND (TTL)
   GPIO 17 (UART1 TX) ────────► TIN
                                  T1OUT  (RS232) ──► Banana plug A (DATA)
                                  GND    (RS232) ──► Banana plug B (GND)

USB-C  ───── via M12 gland ─────► T-Display USB-C  (power + flashing)

IO14  (on-board button)  ──► short = info-page / long = blank / very-long = factory-reset
```

The wall-box plugs into the unmarked banana sockets on the clubhouse
wall plate. **Polarity matters silently** — if the scoreboard doesn't
respond on first power-up, unplug, rotate the box 180°, plug back in.
The firmware ships a polarity-test mode that runs on first boot and
asks you to confirm with the on-board button.

---

## Alge GAZ4 protocol (reverse-engineered)

The board is a passive RS232 receiver. Frames are 17-byte ASCII strings.

| Setting | Value |
|---|---|
| Baud | **2400** |
| Data / parity / stop | 8 / None / 1 |
| Flow control | None |
| Levels | RS232 ±5 V (3.3 V TTL won't reliably drive it — use MAX3232) |
| Terminator | `\r` (0x0D). LF alone is **not** accepted |
| Address bytes | First 3 bytes must be the ASCII digits `"000"` for this board. Frames not starting with 3 ASCII digits are silently rejected |

### Frame layout

```
Byte idx:  0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16
Content:  '0' '0' '0' sp  H  sp  G  sp sp sp sp M₁ M₂ sp S₁ S₂ CR
```

- Byte 4: HOME score digit ('0'-'9', or any non-digit to blank)
- Byte 6: GAST score digit
- Bytes 11/12: clock minutes-tens / minutes-ones
- Bytes 14/15: clock seconds-tens / seconds-ones

### Worked examples

```python
b"000 5 2    10 23\r"   # scores 5-2, clock 10:23
b"000 8 8    88 88\r"   # test pattern (all 8s → polarity check)
b"000             \r"   # blank all six digits
```

### Gotchas worth knowing

1. **Bistable flips hold state** — once set, a digit stays until
   overwritten. The firmware refreshes at 1 Hz anyway because the
   34-year-old electromagnets sometimes miss a single pulse.
2. **Single-shot writes are unreliable** — score changes are sent as a
   4×100 ms burst; blank frames as 8×80 ms (empirically necessary).
3. **Reverse polarity = silent failure** — no error, just no response.
   Hence the polarity-test mode.
4. **Address `"000"` is hardcoded for this board** — addresses 001-020
   and A-J were tested and silently ignored.
5. **GAZ4 has an internal rotary mode switch (positions 0-15)**. The
   board must be in position 0 or 13 (data receiver). 14/15 turn it
   into a standalone clock with no RS232 parsing. If your board ever
   stops responding, the switch may have been bumped.

The official Alge manual (German, 1991) is at:
<https://alge-timing.com/downloads/userGuides/gaz4-be.pdf>

---

## Repository layout

```
.
├── alge-controller/   Firmware: M5Stack Core2 handheld
├── alge-wallbox/      Firmware: LilyGO T-Display-S3 wall-box
├── tools/             Crest generator, mockup renderer, frame-vector tests
├── mockups/           PNG renders of every UI screen + index.html
├── photos/            Photos of the actual scoreboard
└── README.md          You are here
```

Each firmware project has its own README:

- [`alge-controller/README.md`](alge-controller/README.md)
- [`alge-wallbox/README.md`](alge-wallbox/README.md)

---

## Quick start

```bash
# 1. PlatformIO — install the VS Code extension or the pio CLI directly.
pip3 install platformio

# 2. Copy credential templates and customise (gitignored).
cp alge-controller/src/credentials.example.h alge-controller/src/credentials.h
cp alge-wallbox/src/credentials.example.h    alge-wallbox/src/credentials.h
# edit both to set real WiFi + OTA passwords

# 3. Generate the FC Wängi crest bitmap (one-off).
python3 tools/make_crest.py

# 4. Build + flash each device (USB-C).
( cd alge-controller && pio run -t upload )
( cd alge-wallbox    && pio run -t upload )

# 5. Verify the GAZ4 frame builder still matches the reverse-engineered
#    Python reference.
python3 tools/verify_gaz4.py
```

On first boot the wall-box runs its polarity test (sends `88:88 — 8:8`
to the scoreboard repeatedly). Connect the bananas, look at the board,
press IO14 short if it looks right or long if it's reversed (→ rotate
the box 180°).

The two devices auto-pair over ESP-NOW on first boot.

---

## UI mockups

Every controller and wall-box screen is rendered as a PNG at the real
device dimensions:

```bash
python3 tools/make_mockups.py
open mockups/index.html
```

Useful for design review without flashing real hardware. Glyph metrics
differ slightly between PIL (mockups) and the on-device M5GFX/TFT_eSPI
fonts, so the last few pixels of fine-tuning are best done on real
hardware.

---

## License

MIT — use it, fork it, fix your own scoreboard.
