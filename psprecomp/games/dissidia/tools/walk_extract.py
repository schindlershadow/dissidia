import sys, re
log, out = sys.argv[1], sys.argv[2]
rows, base, state = [], None, 0
for l in open(log, errors='replace'):
    m = re.match(r'\[PADREC\] loaded (\d+)', l)
    if m and base is None:
        base = int(m[1]); state = int(0)
        # the pad state at the moment the save loaded carries over: start from what was held then
        continue
    m = re.match(r'\[PADREC\] (\d+) ([0-9A-Fa-f]+)', l)
    if m:
        poll, mask = int(m[1]), int(m[2], 16)
        if base is None:
            state = mask
        else:
            rows.append((max(poll - base, 0), mask))
    if l.startswith('[TEST] battle started'):
        break
if rows and rows[-1][1] != 0:
    rows.append((rows[-1][0] + 2, 0))
open(out, 'w').write(''.join('%d %X\n' % r for r in rows))
print(len(rows), 'rows after the save loaded (base poll %s), last rel poll %s' % (base, rows[-1][0] if rows else None))
