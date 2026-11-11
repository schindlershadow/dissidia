#!/usr/bin/env bash
# Builds and runs test_http against a local HTTP/HTTPS server. Needs mbedTLS (MBEDTLS_PREFIX=<install prefix>, default
# /usr), python3 and openssl. Run from runtime/: tests/run_http.sh
set -euo pipefail
cd "$(dirname "$0")/.."
PFX="${MBEDTLS_PREFIX:-/usr}"
SRV=""; T=$(mktemp -d); trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT
cmake -DPEM=assets/ca/cacert.pem -DOUT="$T/cacert.cpp" -P assets/embed_cacert.cmake
g++ -O1 -std=c++17 -Wall -Werror -DPSPRECOMP_HAVE_MBEDTLS -Isrc/hle -I"$PFX/include" tests/test_http.cpp src/hle/psp_http.cpp \
    "$T/cacert.cpp" -L"$PFX/lib" -lmbedtls -lmbedx509 -lmbedcrypto -o "$T/test_http" -lpthread
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 2 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" 2>/dev/null
mkfifo "$T/hold"
python3 -I tests/http_test_server.py "$T/cert.pem" "$T/key.pem" < "$T/hold" > "$T/srv.out" 2> "$T/srv.err" &
SRV=$!
exec 9>"$T/hold"                      # keeps the server's stdin open until we exit
for i in $(seq 50); do grep -q READY "$T/srv.out" 2>/dev/null && break; sleep 0.1; done
read -r _ HP SP < "$T/srv.out"
"$T/test_http" untrusted "$HP" "$SP"
PSPRECOMP_CA_FILE="$T/cert.pem" "$T/test_http" trusted "$HP" "$SP"
