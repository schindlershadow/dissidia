#!/usr/bin/env python3
"""Pack android/shaders/*.glsl into the shader pack the game compiles at start ("Preparing graphics"): see runtime/src/vk/psp_vk_glsl.h.

  pack_shaders.py [--dir android/shaders] -o shaders.pack      (the APK carries it as assets/shaders.pack)

File names are <hash>.<vert|frag|comp>.glsl (what the device recorded in vk_cache/src, android/sync_shader_list.py); the text is the exact
Vulkan GLSL the runtime hands to glslang. Format: "PSHD", u32 count, then per shader u8 stage ('v' 'f' 'c'), u32 length, the text (little endian).
"""
import argparse, os, struct, sys

STAGES = {"vert": b"v", "frag": b"f", "comp": b"c"}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", default=os.path.join(here, "shaders"))
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    items = []
    for n in sorted(os.listdir(a.dir)):
        parts = n.split(".")
        if len(parts) != 3 or parts[2] != "glsl" or parts[1] not in STAGES:
            continue
        with open(os.path.join(a.dir, n), "rb") as f:
            items.append((STAGES[parts[1]], f.read()))
    with open(a.output, "wb") as f:
        f.write(b"PSHD" + struct.pack("<I", len(items)))
        for st, text in items:
            f.write(st + struct.pack("<I", len(text)) + text)
    print("shader pack: %d shaders, %.1f KB -> %s" % (len(items), os.path.getsize(a.output) / 1024, a.output))


if __name__ == "__main__":
    main()
