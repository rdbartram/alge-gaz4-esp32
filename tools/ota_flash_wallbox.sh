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
WALLBOX_OTA_PORT="${WALLBOX_OTA_PORT:-3232}"
WALLBOX_OTA_AUTH="${WALLBOX_OTA_AUTH:-1967}"
WIFI_IF="${WIFI_IF:-en0}"
PIO_ENV="${PIO_ENV:-lilygo-t-display-s3-ota}"
SKIP_BUILD="${SKIP_BUILD:-0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WALLBOX_DIR="$REPO_ROOT/alge-wallbox"
FIRMWARE_BIN="$WALLBOX_DIR/.pio/build/$PIO_ENV/firmware.bin"
ESPOTA="$HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py"

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

# Application Firewall sanity check. The OTA flow needs the wall-box to
# open an inbound TCP connection back to python3 on the Mac. If the AF
# is enabled and python3 isn't allow-listed, that connection is silently
# dropped and espota.py reports "No response from device". We don't
# touch the firewall automatically (that needs sudo + an opinion), but
# we *do* warn so the user knows where to look.
PFW="/usr/libexec/ApplicationFirewall/socketfilterfw"
if [[ -x "$PFW" ]]; then
    FW_STATE=$("$PFW" --getglobalstate 2>/dev/null || true)
    if [[ "$FW_STATE" == *"enabled"* || "$FW_STATE" == *"State = 1"* || "$FW_STATE" == *"State = 2"* ]]; then
        PY_PATH=$(command -v python3 || true)
        cat >&2 <<EOF
[ota] WARNING: macOS Application Firewall is ON.
[ota]   $FW_STATE
[ota] If the upload fails with "No response from device", the wall-box's
[ota] inbound TCP connection to python3 is being dropped. Either:
[ota]   1) Disable temporarily:   System Settings → Network → Firewall
[ota]   2) Or allow python3 once:
[ota]      sudo "$PFW" --add "$PY_PATH"
[ota]      sudo "$PFW" --unblockapp "$PY_PATH"
[ota] Continuing — if the first run is slow, the OS may also be popping
[ota] a "Allow incoming connections?" dialog; accept it.
EOF
    fi
fi

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
if [[ ! -f "$ESPOTA" ]]; then
    echo "[ota] ERROR: espota.py not found at $ESPOTA" >&2
    echo "[ota] Run a wired 'pio run -t upload' once so PlatformIO installs"  >&2
    echo "[ota] the arduino-espressif32 toolchain, then re-run this script." >&2
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

# The host_ip espota.py auto-detects is whichever interface socket()
# resolves first — frequently the wired Ethernet or a VPN, NOT the WiFi
# we just joined. The wall-box would then try to dial back to an IP it
# can't reach, hang, and time out with "No response from device".
# Read the actual WiFi IP and pass it explicitly.
HOST_IP=$(ipconfig getifaddr "$WIFI_IF" 2>/dev/null || true)
if [[ -z "$HOST_IP" ]]; then
    echo "[ota] ERROR: couldn't read $WIFI_IF IP — is the AP join still up?" >&2
    exit 1
fi
echo "[ota] Host IP on $WIFI_IF: $HOST_IP"

echo "[ota] Uploading $FIRMWARE_BIN to $WALLBOX_IP:$WALLBOX_OTA_PORT ..."
python3 "$ESPOTA" \
    --debug \
    --progress \
    --ip="$WALLBOX_IP" \
    --host_ip="$HOST_IP" \
    --port="$WALLBOX_OTA_PORT" \
    --auth="$WALLBOX_OTA_AUTH" \
    --file="$FIRMWARE_BIN"
echo "[ota] Upload complete — wall-box rebooting into new firmware."
