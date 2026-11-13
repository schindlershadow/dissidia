#!/usr/bin/env python3
"""Convert an already generated dispatch.cpp (unordered_map lookups) to the flat-table form the emitter now writes
(crates/psp-emitter/src/dispatch.rs), without redoing the whole recompile. Idempotent. Usage: flatten_dispatch.py FILE..."""
import re, sys

HEAD = '''static constexpr uint32_t kFlatLo = 0x%08XU;
static constexpr uint32_t kFlatN = %uU;
static FuncPtr s_flat[kFlatN];
static std::unordered_map<uint32_t, FuncPtr> s_dispatch;

static inline void disp_set(uint32_t a, FuncPtr f) {
    const uint32_t i = (a - kFlatLo) >> 2;
    if (!(a & 3) && i < kFlatN) s_flat[i] = f; else s_dispatch[a] = f;
}

static inline FuncPtr disp_get(uint32_t a) {
    const uint32_t i = (a - kFlatLo) >> 2;
    if (!(a & 3) && i < kFlatN) { if (FuncPtr f = s_flat[i]) return f; }
    auto it = s_dispatch.find(a);
    return it == s_dispatch.end() ? nullptr : it->second;
}
'''

def conv(path):
    s = open(path).read()
    if 'disp_set' in s:
        print(path, 'already flat'); return
    addrs = [int(m.group(1), 16) for m in re.finditer(r's_dispatch\[0x([0-9A-Fa-f]{8})U\] =', s)]
    addrs = [a for a in addrs if a % 4 == 0 and 0x08800000 <= a < 0x0A000000]   # guest user RAM; stray addresses stay in the map
    lo, hi = (min(addrs), max(addrs)) if addrs else (0, 0)
    decl = 'static std::unordered_map<uint32_t, FuncPtr> s_dispatch;\n'
    assert decl in s, path
    s = s.replace(decl, HEAD % (lo, ((hi - lo) >> 2) + 1))
    s = re.sub(r's_dispatch\.reserve\(\d+\);', 's_dispatch.reserve(16);', s)
    s = re.sub(r's_dispatch\[(0x[0-9A-Fa-f]{8}U)\] = (\w+);', r'disp_set(\1, \2);', s)
    s = s.replace('s_dispatch[vaddr] = fn;', 'disp_set(vaddr, fn);')
    s = s.replace('    auto it = s_dispatch.find(vaddr);\n    if (it == s_dispatch.end()) { return psp_on_lookup_miss(vaddr); }\n    return it->second;\n',
                  '    if (FuncPtr f = disp_get(vaddr)) return f;\n    return psp_on_lookup_miss(vaddr);\n')
    s = s.replace('    auto it = s_dispatch.find(vaddr);\n    if (it == s_dispatch.end()) return nullptr;\n    return it->second;\n', '    return disp_get(vaddr);\n')
    open(path, 'w').write(s)
    print(path, 'flat: %d entries, range 0x%08X..0x%08X' % (len(addrs), lo, hi))

for f in sys.argv[1:]:
    conv(f)
