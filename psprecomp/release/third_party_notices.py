#!/usr/bin/env python3
"""Write THIRD-PARTY-NOTICES.txt for a release from toolchain/versions.json: every bundled component, its version, license and the
pinned source URL with sha256, and the LGPL written offer for FFmpeg.

  third_party_notices.py --versions toolchain/versions.json -o THIRD-PARTY-NOTICES.txt

Why FFmpeg needs this: libavcodec.a / libavutil.a (LGPL 2.1 or later, built by toolchain/build_bundle.py without any GPL or
non-free option) ship in the toolchain bundle and are linked statically into the game on the player's own machine. The exact
sources are the pinned tarball below; the build flags are in toolchain/build_bundle.py (the notice repeats them). Because the
player compiles and links the game from the shipped static libraries, replacing libavcodec.a with a modified build is possible
(the SDK link project takes it from toolchain/deps/lib).
"""
import argparse
import json

LICENSES = {
    "zlib": "zlib license", "freetype": "FreeType License (FTL) or GPL-2.0", "sdl2": "zlib license",
    "openssl": "Apache-2.0", "pspdecrypt": "GPL-3.0", "ffmpeg": "LGPL-2.1-or-later",
}
USES = {
    "zlib": "linked into the game; used by the toolchain's pspdecrypt",
    "freetype": "linked into the game (menu text)",
    "sdl2": "linked into the game and the setup wizard (window, input, audio)",
    "openssl": "libcrypto statically linked into pspdecrypt only (SHA-256, DES-CBC)",
    "pspdecrypt": "separate executable the installer runs to decrypt the game's EBOOT.BIN (not linked into anything)",
    "ffmpeg": "libavcodec/libavutil, ATRAC3, ATRAC3+ and H.264 decoders only, linked statically into the game",
}
FFMPEG_FLAGS = ("--disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-network "
                "--disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-x86asm "
                "--enable-avcodec --enable-avutil --enable-decoder=atrac3,atrac3p,h264 --enable-static --disable-shared")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--versions", required=True)
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    v = json.load(open(a.versions))
    out = ["THIRD-PARTY NOTICES", "", "This release bundles the components below. Their license texts are in this folder (licenses/) and in",
           "toolchain/licenses/. The program itself (psprecomp) is under the license in psprecomp-LICENSE.txt. The game's own files are",
           "never part of a release: they are read from your own disc image on your machine.", ""]
    for name, e in sorted(v["sources"].items()):
        out += ["%s %s" % (name, e["version"]), "  license: %s" % LICENSES.get(name, "see the source"),
                "  use:     %s" % USES.get(name, ""), "  source:  %s" % e["url"], "  sha256:  %s" % e["sha256"], ""]
    out += ["zig %s (MIT) - the C/C++ compiler the game is built with: https://ziglang.org/" % v["zig"]["version"],
            "cmake %s (BSD-3-Clause), ninja %s (Apache-2.0) - build tools" % (v["cmake"]["version"], v["ninja"]["version"]),
            "portable-file-dialogs (WTFPL) - system file dialogs of the wizard", "", ""]
    ff = v["sources"]["ffmpeg"]
    out += ["WRITTEN OFFER AND NOTES FOR FFmpeg (LGPL 2.1 or later)", "",
            "FFmpeg %s is used unmodified except for two build-time edits made by toolchain/build_bundle.py: HAVE_SYSCTL is set to 0 in" % ff["version"],
            "the generated config.h (the target C library's headers lack <sys/sysctl.h>), and on Windows the unsupported linker option",
            "--pic-executable is dropped from configure. No GPL or non-free component is enabled. Configure options:", "",
            "  " + FFMPEG_FLAGS, "",
            "The complete corresponding source is the tarball %s (sha256 %s), also available from https://ffmpeg.org/." % (ff["url"], ff["sha256"]),
            "The build script is toolchain/build_bundle.py in the source repository of this release. If you want the source from the",
            "distributor of this release instead, open an issue at the project's GitHub page and it will be provided for at least three",
            "years after the release. You may replace libavcodec.a and libavutil.a in toolchain/deps/lib with your own build and rebuild",
            "the game (run the wizard's Rebuild); the game is linked on your machine, so this needs no tools beyond the bundle.", ""]
    open(a.output, "w").write("\n".join(out))


if __name__ == "__main__":
    main()
