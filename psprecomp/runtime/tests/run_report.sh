#!/bin/bash
# Builds test_report, starts the real report_service.py on a test port (GitHub faked, nothing is created) and checks what it stores.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$HERE/..
OUT=${OUT:-$(mktemp -d)}
g++ -std=c++17 -O1 -I$RT/include -I$RT/src -I$RT/src/hle "$HERE/test_report.cpp" $RT/src/psp_report.cpp $RT/src/hle/psp_http.cpp -lz -lpthread -o $OUT/test_report
$OUT/test_report   # unit part
python3 -I "$HERE/report_service_test.py" "$OUT/test_report" "$OUT"
