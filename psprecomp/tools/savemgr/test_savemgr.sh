#!/usr/bin/env bash
# Tests for savemgr on synthetic saves (no game data needed). Run from psprecomp/: tools/savemgr/test_savemgr.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
g++ -O2 -std=c++17 -Wall -Werror -Iruntime/src/hle tools/savemgr/savemgr.cpp runtime/src/hle/psp_savedata_crypto.cpp -o "$T/savemgr"
python3 -I - "$T" <<'PY'
import struct, sys, os
T = sys.argv[1]
def sfo(entries):   # entries: (key, fmt, max, value bytes)
    keys = b''; kofs = []
    for k, *_ in entries: kofs.append(len(keys)); keys += k.encode() + b'\0'
    kt = 20 + 16 * len(entries); dt = (kt + len(keys) + 3) & ~3
    idx = b''; data = b''
    for (k, fmt, mx, v), ko in zip(entries, kofs):
        idx += struct.pack('<HHIII', ko, fmt, len(v), mx, len(data)); data += v.ljust(mx, b'\0')
    out = b'\0PSF' + struct.pack('<IIII', 0x101, kt, dt, len(entries)) + idx + keys
    out = out.ljust(dt, b'\0') + data
    return out.ljust((len(out) + 15) & ~15, b'\0')
def save(root, name, title, detail, files):
    d = f'{T}/{root}/{name}'; os.makedirs(d)
    fl = b''
    for f in files: fl += f.encode().ljust(13, b'\0') + bytes(range(16)) + b'\0\0\0'
    open(f'{d}/PARAM.SFO', 'wb').write(sfo([
        ('CATEGORY', 0x204, 4, b'MS\0'), ('SAVEDATA_DETAIL', 0x204, 1024, detail.encode() + b'\0'),
        ('SAVEDATA_DIRECTORY', 0x204, 64, name.encode() + b'\0'), ('SAVEDATA_FILE_LIST', 4, 3168, fl),
        ('SAVEDATA_PARAMS', 4, 128, b'\x41' + bytes(127)), ('SAVEDATA_TITLE', 0x204, 128, b'GAME DATA\0'),
        ('TITLE', 0x204, 128, title.encode() + b'\0')]))
    for f in files: open(f'{d}/{f}', 'wb').write(os.urandom(300))
    open(f'{d}/stray.sync-conflict.BIN', 'wb').write(b'x')
    open(f'{d}/ICON0.PNG', 'wb').write(b'png')
save('eu', 'ULES01505GameData00', 'DISSIDIA 012 FINAL FANTASY', 'PLAYER: Ender', ['DISSIDIA.BIN'])
save('eu', 'ULES01505FCard00', 'DISSIDIA 012 FINAL FANTASY', 'PLAYER: Akan', ['FCARD.BIN'])
save('eu', 'ULJM05500', 'DISSIDIA FINAL FANTASY', 'other game', ['X.BIN'])
os.makedirs(f'{T}/work/disc0/PSP_GAME')
open(f'{T}/work/disc0/PSP_GAME/PARAM.SFO', 'wb').write(sfo([('DISC_ID', 0x204, 16, b'ULUS10566\0')]))
PY
S="$T/savemgr"
out=$("$S" list --from "$T/eu"); echo "$out"
echo "$out" | grep -q "ULES01505GameData00.*EU.*game data.*Ender"
echo "$out" | grep -q "ULES01505FCard00.*friend card"
! echo "$out" | grep -q ULJM05500            # the first Dissidia is not listed
# import into an install: the disc ID (US) is used
"$S" import --from "$T/eu" --workdir "$T/work" >/dev/null
M="$T/work/ms0/PSP/SAVEDATA"
test -f "$M/ULUS10566GameData00/DISSIDIA.BIN" && cmp "$M/ULUS10566GameData00/DISSIDIA.BIN" "$T/eu/ULES01505GameData00/DISSIDIA.BIN"
test -f "$M/ULUS10566FCard00/FCARD.BIN"
test ! -e "$M/ULUS10566GameData00/stray.sync-conflict.BIN"   # only the listed files travel
grep -aq ULUS10566GameData00 "$M/ULUS10566GameData00/PARAM.SFO"
! grep -aq ULES01505 "$M/ULUS10566GameData00/PARAM.SFO"
# an existing target is refused, then replaced with a backup under --force
! "$S" import --from "$T/eu" --workdir "$T/work" 2>/dev/null
"$S" import --from "$T/eu" --workdir "$T/work" --force >/dev/null
test -d "$M/ULUS10566GameData00.bak"
# a single save folder as the source; a chosen ID; export keeps the region unless told
"$S" export --from "$T/eu/ULES01505GameData00" --to-dir "$T/out" --id ULJM05681 >/dev/null
test -f "$T/out/ULJM05681GameData00/DISSIDIA.BIN"
"$S" export --from "$T/eu" --to-dir "$T/out2" GameData00 >/dev/null
test -f "$T/out2/ULES01505GameData00/PARAM.SFO" && cmp "$T/out2/ULES01505GameData00/PARAM.SFO" "$T/eu/ULES01505GameData00/PARAM.SFO"
test ! -e "$T/out2/ULES01505FCard00"
echo "savemgr tests passed"
