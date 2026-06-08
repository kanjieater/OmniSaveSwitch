#!/bin/bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
TITLE_ID="420000000000000C"
NSP="sysmodule/exefs.nsp"

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

if [ -z "${OMNISAVE_SWITCHES:-}" ]; then
    echo "ERROR: OMNISAVE_SWITCHES is not set."
    echo "Add it to ${REPO_ROOT}/.env (see .env.example)."
    exit 1
fi

IFS=',' read -ra SWITCH_IPS <<< "${OMNISAVE_SWITCHES}"

deploy_to_switch() {
    local ip="$1"
    local base="ftp://${ip}/atmosphere/contents/${TITLE_ID}"

    curl -s -Q "DELE ${base}/exefs/main" "ftp://${ip}/" 2>/dev/null || true
    curl -s -Q "DELE ${base}/exefs/main.npdm" "ftp://${ip}/" 2>/dev/null || true
    curl -s -Q "RMD ${base}/exefs" "ftp://${ip}/" 2>/dev/null || true

    if curl -s --ftp-create-dirs -T sysmodule/toolbox.json "${base}/toolbox.json" 2>/dev/null \
    && curl -s --ftp-create-dirs -T "$NSP" "${base}/exefs.nsp" 2>/dev/null \
    && printf '' | curl -s --ftp-create-dirs -T - "${base}/flags/boot2.flag" 2>/dev/null; then
        echo "  ✅ ${ip}"
    else
        echo "  ❌ ${ip} (upload failed)"
    fi
}

echo "🔨 Building OmniSave..."
./build.sh

if [ ! -f "$NSP" ]; then
    echo "❌ $NSP was not produced."
    exit 1
fi

SIZE=$(stat -c%s "$NSP")
if [ "$SIZE" -lt 4096 ]; then
    echo "❌ $NSP is only ${SIZE} bytes."
    exit 1
fi

echo "🚀 Deploying to ${#SWITCH_IPS[@]} switch(es)..."
for ip in "${SWITCH_IPS[@]}"; do
    ip="${ip// /}"
    [ -z "$ip" ] && continue
    deploy_to_switch "$ip"
done

echo "🔁 Cold boot each Switch to load the new sysmodule."
