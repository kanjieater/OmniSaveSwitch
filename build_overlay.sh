#!/bin/bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
OVL="overlay/omnisave.ovl"
OVL_ENTRY="[omnisave.ovl]
custom_name=OmniSave
custom_version=
hide=false
launch_args=
priority=20
star=false
use_launch_args=false"

TESLA_PATH="overlay/include/tesla.hpp"
STB_PATH="overlay/include/stb_truetype.h"
LIBTESLA_COMMIT="f766e9b607a05e9756843cbd62b3bfb98be1646c"
TESLA_URL="https://raw.githubusercontent.com/WerWolv/libtesla/${LIBTESLA_COMMIT}/include/tesla.hpp"
STB_URL="https://raw.githubusercontent.com/WerWolv/libtesla/${LIBTESLA_COMMIT}/include/stb_truetype.h"

load_env() {
    local file="$1"
    if [ -f "$file" ]; then
        set -a
        # shellcheck disable=SC1090
        source "$file"
        set +a
    fi
}

load_env "${REPO_ROOT}/.env"
load_env "${OMNISAVE_ROOT:-/mnt/srv/omnisave}/.env"

# --- Fetch deps ---
mkdir -p overlay/include
echo "--- Fetching libtesla @ ${LIBTESLA_COMMIT} ---"
curl -fsSL "$TESLA_URL" -o "$TESLA_PATH"
curl -fsSL "$STB_URL" -o "$STB_PATH"

# --- Build ---
echo "--- 1. Building Docker environment ---"
docker build -t omnisave-overlay-builder ./overlay

echo "--- 2. Compiling overlay ---"
docker run --rm -v "$(pwd)/overlay:/src" omnisave-overlay-builder make

# --- Deploy ---
if [ -z "${OMNISAVE_SWITCHES:-}" ]; then
    echo "OMNISAVE_SWITCHES not set — skipping deploy."
    echo "Done: $OVL"
    exit 0
fi

IFS=',' read -ra SWITCH_IPS <<< "${OMNISAVE_SWITCHES}"
echo "--- 3. Deploying to ${#SWITCH_IPS[@]} switch(es) ---"

register_in_ultrahand() {
    local ip="$1"
    local ini_ftp="ftp://${ip}/config/ultrahand/overlays.ini"
    local tmp_ini
    tmp_ini="$(mktemp)"

    # Download current overlays.ini; start fresh if it doesn't exist
    curl -s -o "$tmp_ini" "$ini_ftp" 2>/dev/null || true

    if grep -qF "[omnisave.ovl]" "$tmp_ini" 2>/dev/null; then
        echo "  already registered in overlays.ini on ${ip}"
    else
        printf '\n%s\n' "$OVL_ENTRY" >> "$tmp_ini"
        if curl -s --ftp-create-dirs -T "$tmp_ini" "$ini_ftp"; then
            echo "  registered in overlays.ini on ${ip}"
        else
            echo "  WARN: could not update overlays.ini on ${ip}"
        fi
    fi

    rm -f "$tmp_ini"
}

for raw_ip in "${SWITCH_IPS[@]}"; do
    ip="${raw_ip// /}"
    [ -z "$ip" ] && continue
    echo "  deploying to ${ip}..."
    if curl -s --ftp-create-dirs \
            -T "$OVL" \
            "ftp://${ip}/switch/.overlays/omnisave.ovl"; then
        register_in_ultrahand "$ip"
    else
        echo "  FAIL ${ip}"
    fi
done

echo "--- Done ---"
