#!/usr/bin/env bash
# ============================================================================
#  usb_flash_wallbox.sh
#
#  Cable-flash both the wall-box firmware AND the bundled controller .bin
#  into the wall-box's LittleFS partition over USB. Much faster + more
#  reliable than the WiFi-OTA path:
#    - firmware:    `pio run -t upload`        (~5-10 s over USB at 921 KB/s)
#    - controller:  `pio run -t uploadfs`      (~5-10 s — LittleFS image)
#
#  Use this when the wall-box is in front of you and you have a cable.
#  For wireless updates without a cable, use ota_flash_wallbox.sh.
#
#  Usage:
#    tools/usb_flash_wallbox.sh
#
#  Overrides:
#    SKIP_BUILD=1 …       reuse existing .pio build for both projects
#    SKIP_BUILD_CTRL=1 …  reuse existing controller .bin only
#    PIO_ENV=…            override wall-box pio env name
#    CONTROLLER_ENV=…     override controller pio env name
# ============================================================================

set -euo pipefail

SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_BUILD_CTRL="${SKIP_BUILD_CTRL:-0}"
PIO_ENV="${PIO_ENV:-lilygo-t-display-s3}"
CONTROLLER_ENV="${CONTROLLER_ENV:-m5stack-core2}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WALLBOX_DIR="$REPO_ROOT/alge-wallbox"
CONTROLLER_DIR="$REPO_ROOT/alge-controller"
CONTROLLER_BIN="$CONTROLLER_DIR/.pio/build/$CONTROLLER_ENV/firmware.bin"
WALLBOX_DATA_DIR="$WALLBOX_DIR/data"
WALLBOX_DATA_BIN="$WALLBOX_DATA_DIR/controller.bin"

if [[ ! -f "$WALLBOX_DIR/platformio.ini" ]]; then
    echo "[usb] ERROR: no platformio.ini at $WALLBOX_DIR" >&2
    exit 1
fi
if ! command -v pio >/dev/null 2>&1; then
    echo "[usb] ERROR: pio not in PATH — install PlatformIO Core first" >&2
    exit 1
fi

# Build both projects (unless skipped). The wall-box firmware is what
# gets flashed via -t upload; the controller's firmware.bin gets copied
# into alge-wallbox/data/controller.bin which uploadfs then writes
# verbatim into the LittleFS partition.
if [[ "$SKIP_BUILD" != "1" ]]; then
    echo "[usb] Building wall-box firmware..."
    (cd "$WALLBOX_DIR" && pio run -e "$PIO_ENV")
fi
if [[ "$SKIP_BUILD" != "1" && "$SKIP_BUILD_CTRL" != "1" ]]; then
    echo "[usb] Building controller firmware..."
    (cd "$CONTROLLER_DIR" && pio run -e "$CONTROLLER_ENV")
fi

if [[ ! -f "$CONTROLLER_BIN" ]]; then
    echo "[usb] ERROR: controller firmware not found at $CONTROLLER_BIN" >&2
    exit 1
fi

# Stage the controller binary in alge-wallbox/data/ for uploadfs to pick
# up. PIO bundles whatever's in this folder into a LittleFS image and
# writes it to the spiffs/littlefs partition.
mkdir -p "$WALLBOX_DATA_DIR"
cp -f "$CONTROLLER_BIN" "$WALLBOX_DATA_BIN"
echo "[usb] Staged $(wc -c < "$WALLBOX_DATA_BIN" | tr -d ' ') bytes -> $WALLBOX_DATA_BIN"

# 1) Firmware upload (app partition).
echo "[usb] Uploading wall-box firmware..."
(cd "$WALLBOX_DIR" && pio run -e "$PIO_ENV" -t upload)

# 2) Filesystem upload (LittleFS data partition) — writes data/ verbatim.
echo "[usb] Uploading LittleFS image with controller.bin..."
(cd "$WALLBOX_DIR" && pio run -e "$PIO_ENV" -t uploadfs)

echo "[usb] Done. Wall-box should reboot and report:"
echo "[usb]   controller.bin: <N> bytes, md5=<…>"
echo "[usb]   HTTP server listening on :80"
echo "[usb] Paired controllers will be nudged on their next heartbeat."
