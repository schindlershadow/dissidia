// The pre-shared key that signs a report (HMAC-SHA256 over "<time>\n<sha256 of the zip>", psp_report.cpp). It is committed on purpose:
// every build of the game has to be able to report, so it is not a secret. It only keeps scanners out; the service rate-limits and caps
// sizes (tools/report_service/report_service.py). The service accepts several keys, so a rotated key does not break old releases.
#pragma once
#define PSPRECOMP_REPORT_KEY "bdQPHYdsP9PdbmfTSUb2hXyYP21zGV-2N4XmUigPPF4"
