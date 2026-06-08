#!/bin/bash
# Legacy raw exefs deploy — prefer ./deploy.sh + exefs.nsp
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
TITLE_ID="0100000000000001"

if [ -f "${REPO_ROOT}/.env" ]; then
    set -a
    # shellcheck disable=SC1090
    source "${REPO_ROOT}/.env"
    set +a
fi

if [ -z "${OMNISAVE_SWITCHES:-}" ]; then
    echo "ERROR: Set OMNISAVE_SWITCHES in .env (see .env.example)"
    exit 1
fi

IFS=',' read -ra SWITCH_IPS <<< "${OMNISAVE_SWITCHES}"

echo "--- Building OmniSave Sysmodule ---"
./build.sh

touch boot2.flag

for IP in "${SWITCH_IPS[@]}"; do
    IP="${IP// /}"
    [ -z "$IP" ] && continue

    echo "--- Deploying to $IP (raw exefs) ---"
    curl --ftp-create-dirs -T sysmodule/toolbox.json "ftp://${IP}/atmosphere/contents/${TITLE_ID}/toolbox.json"
    curl --ftp-create-dirs -T sysmodule/OmniSave.nso "ftp://${IP}/atmosphere/contents/${TITLE_ID}/exefs/main"
    curl --ftp-create-dirs -T sysmodule/OmniSave.npdm "ftp://${IP}/atmosphere/contents/${TITLE_ID}/exefs/main.npdm"
    curl --ftp-create-dirs -T boot2.flag "ftp://${IP}/atmosphere/contents/${TITLE_ID}/flags/boot2.flag"
done

rm -f boot2.flag
