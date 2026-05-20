#!/usr/bin/env bash
# ============================================================================
#  ota_flash_wallbox.sh
#
#  One-shot wireless reflash of the wall-box on a Mac:
#    1. build the firmware (while you still have internet)
#    2. remember the WiFi network you're currently joined to
#    3. join the wall-box's "FC-Waengi-Wallbox" SoftAP
#    4. wait for the wall-box to be pingable
#    5. invoke espota.py directly with the prebuilt .bin (bypasses
#       PlatformIO so its Python-deps update check doesn't choke on
#       the no-internet AP)
#    6. bounce WiFi so macOS auto-rejoins the original SSID
#
#  Usage:
#    tools/ota_flash_wallbox.sh
#
#  Overrides:
#    WIFI_IF=en1 …              if your Mac's WiFi isn't en0
#    SKIP_BUILD=1 …             reuse the existing .pio build (no recompile)
# ============================================================================

set -euo pipefail

WALLBOX_SSID="${WALLBOX_SSID:-FC-Waengi-Wallbox}"
WALLBOX_PASS="${WALLBOX_PASS:-1967NeverGiveUp}"
WALLBOX_IP="${WALLBOX_IP:-192.168.4.1}"
WALLBOX_OTA_USER="${WALLBOX_OTA_USER:-ota}"
WALLBOX_OTA_AUTH="${WALLBOX_OTA_AUTH:-1967}"
WIFI_IF="${WIFI_IF:-en0}"
PIO_ENV="${PIO_ENV:-lilygo-t-display-s3}"
SKIP_BUILD="${SKIP_BUILD:-0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WALLBOX_DIR="$REPO_ROOT/alge-wallbox"
FIRMWARE_BIN="$WALLBOX_DIR/.pio/build/$PIO_ENV/firmware.bin"

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

# HTTP-push OTA (host → device only) means we don't need the
# Application Firewall to accept any incoming connections — curl just
# opens an outbound TCP to the wall-box on port 80. Skipping the
# firewall check entirely.

# Step 1 — build the firmware while we still have internet. PlatformIO's
# pre-build phase pings GitHub/registries for dependency / toolchain
# updates; once we jump to the wall-box AP that check times out and the
# upload never starts.
if [[ "$SKIP_BUILD" != "1" ]]; then
    echo "[ota] Building firmware (env: $PIO_ENV)..."
    (cd "$WALLBOX_DIR" && pio run -e "$PIO_ENV")
fi

if [[ ! -f "$FIRMWARE_BIN" ]]; then
    echo "[ota] ERROR: built firmware not found at $FIRMWARE_BIN" >&2
    exit 1
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "[ota] ERROR: curl not found — install it via Homebrew (already on macOS by default)" >&2
    exit 1
fi

PREV_SSID=""

restore_wifi() {
    local rc=$?
    # Always bounce regardless of PREV_SSID — when the script starts with
    # WiFi off / disconnected we still want to drop the wall-box AP and
    # let macOS pick the best preferred network on the way out. The off/on
    # toggle is the most reliable way to do that without us needing to
    # know the user's home/work SSID password.
    echo
    echo "[ota] Bouncing WiFi so macOS rejoins its preferred network..."
    networksetup -setairportpower "$WIFI_IF" off || true
    sleep 1
    networksetup -setairportpower "$WIFI_IF" on  || true
    if [[ -n "$PREV_SSID" && "$PREV_SSID" != "$WALLBOX_SSID" ]]; then
        echo "[ota] Should rejoin '$PREV_SSID' shortly."
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

# Read the WiFi interface's current IPv4. Use this as the ground truth
# of "are we on the wall-box AP" — macOS Sonoma/Sequoia restricts
# `networksetup -getairportnetwork` for privacy and it now returns
# "not associated" even when you're connected, unless the caller has
# Location Services. The IP doesn't lie.
current_ip() {
    ipconfig getifaddr "$WIFI_IF" 2>/dev/null || true
}

CURR_IP=$(current_ip)
ON_WALLBOX_AP=0
if [[ "$CURR_IP" == 192.168.4.* ]]; then
    ON_WALLBOX_AP=1
    echo "[ota] Already on wall-box AP (IP: $CURR_IP)."
fi

if [[ "$ON_WALLBOX_AP" != "1" ]]; then
    echo "[ota] Joining $WALLBOX_SSID ..."
    # Try up to 5 times — macOS only scans for new APs periodically, and a
    # wall-box that just powered on may not be in the scan cache yet. A
    # bare networksetup call fails with "Could not find network ..." in
    # that window; sleep + retry handles it.
    JOINED=0
    for attempt in 1 2 3 4 5; do
        if networksetup -setairportnetwork "$WIFI_IF" \
                "$WALLBOX_SSID" "$WALLBOX_PASS" 2>&1 | grep -qi "could not"; then
            echo "[ota]   attempt $attempt: AP not in scan cache yet, waiting..."
            sleep 5
            continue
        fi
        # Wait for DHCP — the join can return success while the lease is
        # still negotiating. Poll the interface IP up to 10 s.
        for _ in $(seq 1 10); do
            CURR_IP=$(current_ip)
            if [[ "$CURR_IP" == 192.168.4.* ]]; then
                JOINED=1
                break 2
            fi
            sleep 1
        done
        echo "[ota]   attempt $attempt: joined but no 192.168.4.x lease yet, retrying..."
    done

    if [[ "$JOINED" != "1" ]]; then
        cat >&2 <<EOF
[ota] ERROR: couldn't end up on the wall-box's 192.168.4.0/24 subnet.
[ota] Last $WIFI_IF IP: ${CURR_IP:-<none>}
[ota] Things to check:
[ota]   - Wall-box is powered on and running our firmware (LCD lit)
[ota]   - Wall-box is within range of this Mac
[ota]   - SSID '$WALLBOX_SSID' is visible in System Settings > WiFi
[ota]     (if not, the wall-box's SoftAP isn't broadcasting — reboot it
[ota]     by power-cycling, then re-run this script)
[ota]   - If the SSID IS visible, click-join it once manually so macOS
[ota]     caches the credentials, then re-run.
EOF
        exit 1
    fi
    echo "[ota] On wall-box AP. Local IP: $CURR_IP"

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
            exit 1
        fi
    done
fi

echo "[ota] Uploading $FIRMWARE_BIN to http://$WALLBOX_IP/update ..."
# --fail makes curl exit non-zero on HTTP >=400 so we don't claim success
# when the wall-box rejects the upload. --max-time 60s caps a hung box.
# The form name "firmware" matches WebServer's HTTPUpload conventions —
# the wallbox-side handle_upload() doesn't care about the field name,
# but tagging it consistently helps if you ever post manually.
http_status=$(curl --fail --max-time 60 \
    --user "$WALLBOX_OTA_USER:$WALLBOX_OTA_AUTH" \
    --progress-bar \
    --form "firmware=@$FIRMWARE_BIN" \
    --write-out '%{http_code}\n' \
    --output /dev/null \
    "http://$WALLBOX_IP/update" || echo "curl-failed")

if [[ "$http_status" == "200" ]]; then
    echo "[ota] Upload complete — wall-box rebooting into new firmware."
else
    echo "[ota] ERROR: upload failed (status: $http_status)" >&2
    exit 1
fi
