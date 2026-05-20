#!/usr/bin/env bash
# ============================================================================
#  ota_flash_wallbox.sh
#
#  One-shot wireless reflash of the wall-box on a Mac:
#    1. remember the WiFi network you're currently joined to
#    2. join the wall-box's "FC-Waengi-Wallbox" SoftAP
#    3. wait for the wall-box to be pingable
#    4. pio run -e lilygo-t-display-s3-ota -t upload
#    5. bounce WiFi so macOS auto-rejoins your normal SSID from its
#       preferred-networks list (keychain holds the password)
#
#  Usage:
#    tools/ota_flash_wallbox.sh
#
#  Override the WiFi interface if your Mac uses something other than en0:
#    WIFI_IF=en1 tools/ota_flash_wallbox.sh
# ============================================================================

set -euo pipefail

WALLBOX_SSID="${WALLBOX_SSID:-FC-Waengi-Wallbox}"
WALLBOX_PASS="${WALLBOX_PASS:-1967NeverGiveUp}"
WALLBOX_IP="${WALLBOX_IP:-192.168.4.1}"
WIFI_IF="${WIFI_IF:-en0}"
PIO_ENV="${PIO_ENV:-lilygo-t-display-s3-ota}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WALLBOX_DIR="$REPO_ROOT/alge-wallbox"

if [[ ! -f "$WALLBOX_DIR/platformio.ini" ]]; then
    echo "[ota] ERROR: no platformio.ini at $WALLBOX_DIR" >&2
    exit 1
fi
if ! command -v pio >/dev/null 2>&1; then
    echo "[ota] ERROR: pio not in PATH — install PlatformIO Core first" >&2
    exit 1
fi
if ! command -v networksetup >/dev/null 2>&1; then
    echo "[ota] ERROR: networksetup not found — this script is macOS-only" >&2
    exit 1
fi

PREV_SSID=""

restore_wifi() {
    local rc=$?
    if [[ -n "$PREV_SSID" && "$PREV_SSID" != "$WALLBOX_SSID" ]]; then
        echo
        echo "[ota] Restoring WiFi (was: $PREV_SSID)..."
        # Toggling power off+on is the most reliable way to get macOS to
        # re-evaluate its preferred-networks list — passing the SSID
        # directly via networksetup -setairportnetwork would require us to
        # also know the password, which lives in the Keychain. Bouncing
        # the radio lets macOS rejoin the highest-priority known network
        # without us touching credentials.
        networksetup -setairportpower "$WIFI_IF" off || true
        sleep 1
        networksetup -setairportpower "$WIFI_IF" on  || true
        echo "[ota] WiFi bounced — macOS should rejoin '$PREV_SSID' shortly."
    fi
    exit $rc
}
trap restore_wifi EXIT INT TERM

# `networksetup -getairportnetwork` output:
#   "Current Wi-Fi Network: <SSID>"   when joined
#   "You are not associated with an AirPort network."   when not
RAW=$(networksetup -getairportnetwork "$WIFI_IF" 2>/dev/null || true)
if [[ "$RAW" == *":"* && "$RAW" != *"not associated"* ]]; then
    PREV_SSID="${RAW#*: }"
fi
echo "[ota] Current SSID: ${PREV_SSID:-<none>}"

if [[ "$PREV_SSID" != "$WALLBOX_SSID" ]]; then
    echo "[ota] Joining $WALLBOX_SSID ..."
    networksetup -setairportnetwork "$WIFI_IF" "$WALLBOX_SSID" "$WALLBOX_PASS"

    echo -n "[ota] Waiting for $WALLBOX_IP to respond"
    for i in $(seq 1 30); do
        if ping -c 1 -W 1000 "$WALLBOX_IP" >/dev/null 2>&1; then
            echo " — up."
            break
        fi
        echo -n "."
        sleep 1
        if [[ $i == 30 ]]; then
            echo
            echo "[ota] ERROR: $WALLBOX_IP didn't respond after 30 s." >&2
            echo "[ota] Is the wall-box powered on and within range?" >&2
            exit 1
        fi
    done
fi

echo "[ota] Running OTA upload..."
cd "$WALLBOX_DIR"
pio run -e "$PIO_ENV" -t upload
echo "[ota] Upload complete."
