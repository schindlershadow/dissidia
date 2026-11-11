#!/usr/bin/env bash
# Builds and runs test_cloud_providers against a local fake WebDAV / Google server (no network, no accounts, no TLS).
# Needs python3. Run from anywhere: runtime/tests/run_cloud.sh
set -euo pipefail
cd "$(dirname "$0")/.."
SRV=""; T=$(mktemp -d); trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT
SRC="src/hle/psp_cloud_providers.cpp src/hle/psp_cloudsync.cpp src/hle/psp_http.cpp src/hle/psp_savemgr.cpp src/hle/psp_savedata_crypto.cpp"
g++ -O1 -std=c++17 -Wall -Werror -Isrc/hle -Itests tests/test_cloud_providers.cpp $SRC -o "$T/test" -lpthread
g++ -O1 -std=c++17 -Wall -Werror -Isrc/hle -Itests tests/test_cloud_service.cpp src/hle/psp_cloud_service.cpp $SRC -o "$T/test_svc" -lpthread
mkfifo "$T/hold"
python3 -I tests/cloud_fake_server.py < "$T/hold" > "$T/srv.out" 2> "$T/srv.err" &
SRV=$!
exec 9>"$T/hold"
for i in $(seq 50); do grep -q READY "$T/srv.out" 2>/dev/null && break; sleep 0.1; done
read -r _ PORT < "$T/srv.out"
"$T/test" "$PORT" || { echo "--- server stderr:"; cat "$T/srv.err"; exit 1; }
"$T/test_svc" "$PORT" || { echo "--- server stderr:"; cat "$T/srv.err"; exit 1; }
