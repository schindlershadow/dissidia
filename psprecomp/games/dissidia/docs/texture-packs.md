# Texture packs

PPSSPP-style HD texture replacement: where the pack goes, how to dump and upscale, and the built-in upscaler.

## How it works

PPSSPP-style texture replacement, implemented in the generic runtime (`runtime/src/psp_tex_pack.cpp`,
hooked into `ge_texture_bind`); this game module only supplies the directory. Pack directory:
`PSPRECOMP_TEXPACK` (`run.sh --texpack DIR`), else `<mods>/textures` (`<mods>` as in "Modding: loose files").
Keep the pack outside `<mods>` when it is large: the pack is served by texture hash and is never a package
entry, and the loader ignores `<mods>/textures/` for the package index, but a separate folder keeps the mods
tree small. Dumps go to `PSPRECOMP_TEX_DUMP_DIR` (`run.sh --tex-dump DIR`, which also turns dumping on),
else `<pack>/dump`.

```
<mods>/textures/dump/<id>.png   dumped originals (PSPRECOMP_TEX_DUMP=1)
<mods>/textures/<id>.png        replacements, any size (2x/4x/8x ...), RGBA/RGB/gray/palette PNG, not interlaced
<mods>/textures/textures.txt    index: id WxH format clut-format first-seen-address times-seen
```

**Texture id.** 64-bit xxHash of the decoded source texels (unswizzled, packed to width x height, so it does
not depend on guest address, swizzle or row stride), the RGBA values of the palette entries the texels
actually reference (CLUT4/CLUT8 only: each palette variant of an image is its own id, unused palette
entries do not matter), the GE format and the size. It is computed only on a texture-cache miss and only
when dump, a non-empty pack or the upscaler is on, so the default cost is zero. Formats decoded by the
runtime (5650/5551/4444/8888/CLUT4/CLUT8) take part; DXT is not used by this game.

**Workflow.**
1. `PSPRECOMP_TEX_DUMP=1 ./run.sh ...`, play through the menus and a battle. Each distinct texture is written once
   (`PSPRECOMP_TEX_DUMP_MAX`, default 50000, caps the count; existing PNGs are never overwritten). Title + menus + one
   Quick Battle is about 300 distinct textures / 2.8 MB. About two thirds of them are 512x32 CLUT4 strips: those are
   the runtime-rendered text lines, they change with the text; skip them with `--min-size 64`.
2. Upscale: `python3 games/dissidia/tools/texpack.py <mods>/textures/dump --min-size 64 --cmd "realesrgan-ncnn-vulkan -i {in} -o {out} -s 4"`
   (`--list` prints the index with done/todo status, `--filter ">=32x32"`, `--format CLUT8`, `--limit`, `--dry-run`,
   `--scale-check 4`; finished ids are skipped, so it resumes). Any CLI works: Real-ESRGAN ncnn-vulkan
   (`-n realesrgan-x4plus-anime` for the UI art), `waifu2x-ncnn-vulkan -n 1 -s 2`, or ImageMagick
   `convert {in} -filter Lanczos -resize 400% {out}`. The tool installs and downloads nothing. If an upscaler
   drops the alpha channel it is restored from the original when Pillow is available.
3. Run normally: replacement is on whenever the directory has `<id>.png` files (`PSPRECOMP_TEX_REPLACE=0` turns it off; the in-game settings menu has an **HD Textures** On/Off row whenever `<workdir>/mods/textures` exists, saved as `hd_textures=` in `settings.ini`, applied at the next start). The pack is looked up next to the Memory Stick folder: `<parent of ms0>/mods/textures`, so `PSPRECOMP_MS0` pointing somewhere else moves the pack, the Japanese voice folders and `mods/` with it.
   The file list is read once at start (no per-bind stat); a replacement is decoded on the cache miss and uploaded
   at its own size. UVs are normalised, so TEXSCALE/TEXOFFSET, REPEAT/CLAMP and both draw paths
   (batched GPU-skinning path, the default on every platform, and `PSPRECOMP_GE_FAST=0` for the legacy renderer) work unchanged; render scale is independent.
   A replacement more than 2x the original is uploaded with generated mip levels, used when the game's
   TEXFILTER asks for mipmapped minification. The game's filter is kept; `PSPRECOMP_TEX_REPLACE_FILTER=linear|nearest`
   forces it on replaced textures. Render-target textures and movie frames never take part.

**Built-in upscaler (off by default).** `PSPRECOMP_TEX_UPSCALE=xbrz2|xbrz3|xbrz4` upscales textures that have no
replacement at decode time (results cached per id in memory, 192 MB budget; 4x = 2x applied twice). It is a
clean-room edge-directed pixel scaler (Scale2x/Scale3x rules with a colour-distance equality test, alpha
aware), not the GPL xBRZ code; the names only select the factor. It helps hard-edged UI art, not photographic
textures.

Notes: ids of textures with partly uninitialised guest memory can differ between runs; textures drawn at
runtime (text strips) produce a new id per distinct content. Stats are logged as `[TEXPACK]` lines.


## PPSSPP-style packs (Niosa's D012FFRT)

A pack folder that has a PPSSPP `textures.ini` is used as it is, no conversion: `[options] hash = xxh64 | xxh32` and `ignoreAddress`, and
`[hashes]` lines `<24 hex> = relative/file.png`. (`hash = quick` is not supported and turns this off.) The runtime computes PPSSPP's key at
texture-bind time from the same guest memory PPSSPP hashes (`runtime/src/psp_tex_pack.cpp`, `psp_texpack_ppsspp_lookup`):

- cache key = `(addr & 0x3FFFFFFF) << 32 | (TEXSIZE0 & 0xF0F)`; for palette formats XOR `XXH32(palette bytes, seed 0xC0108888) ^ CLUTFORMAT`
  (palette bytes = the last LOADCLUT's block count x 32 plus the CLUT base offset, at least 1 KiB window: `GeState::clut_load_blocks`);
- content hash over the raw, still swizzled bytes in guest memory: contiguous textures (`bufw <= width`) hash `bpp * (bufw*h + (w - bufw)) / 8` bytes
  with `XXH64(seed 0xBACD7814)` truncated to 32 bits; textures with gaps (`bufw > width`) hash each row and combine `(r*11) ^ rowhash`;
- file name key = `%016llx%08x` (cache key, content hash); with `ignoreAddress` the lookup uses the key with the address bits cleared.

From PPSSPP's `TextureCacheCommon` / `TextureReplacer` as of v1.17+ (read from GitHub, 2026-11-10). **Not implemented**: PPSSPP's `maxSeenV`
height trimming for 512-high textures, `[hashranges]`, `[reducehashranges]`, mip levels (`_1.png`), `.dds/.ktx2`. `PSPRECOMP_TEX_PPSSPP=0`
disables it; `PSPRECOMP_TEXPACK_LOG=1` logs the first 400 lookups as `[TEXPACK] ppsspp <key> ... HIT file | miss` and a hit counter, which is how to
check a pack against the game (a miss on a texture the pack is known to replace points at one of the unimplemented parts).
The hash functions are unit-tested (`test_tex_pack_ppsspp`); **no hit has been seen in the real game yet**.

`tools/fetch_textures.py <workdir>` downloads Niosa/D012FFRT from GitHub into `<workdir>/mods/textures` (the installer wizard does this when
"HD texture pack" is on). The pack has no stated license and is not bundled; it was made for the US release (ULUS10566).

## Pack entries without a file

A textures.ini may list textures the pack does not ship: Niosa's D012FFRT has about 220 of its ~1050 `[hashes]` lines pointing to a PNG that is
not in the download (names without `.png`, DLC/test art, `ui/selogo.png`, `ui/opencredits.png`...). The loader checks every entry once at start,
drops those and logs `[TEXPACK] N pack entries point to a file the pack does not contain (ignored)`; before, such a texture logged `HIT` and then
`bad replacement ...: unreadable (using original)`. Those textures stay original either way.

## Precedence and pixel-repeated pack entries

The runtime looks a texture up in the id-named files (`<id>.png`, see above) first and in a PPSSPP `textures.ini` pack second. About 45% of
Niosa's D012FFRT files (360 of 808) are only the original with every pixel repeated 2x or 4x, so they look as blocky as the PSP art (item icons,
`ui/equipmenticons.png`, many character textures). `tools/texpack_extract.py` + `tools/texpack_upscale.py` produce real upscales of the same
textures under our ids; because id files win, they replace those entries without editing the foreign pack. `PSPRECOMP_TEXPACK_LOG_MAX=N`
raises the number of PPSSPP lookups logged (default 400 with `PSPRECOMP_TEXPACK_LOG=1`).

## The extra textures repository

`tools/fetch_textures.py` (run by the wizard when "HD texture pack" is on) installs two things into `<workdir>/mods/textures`: Niosa's pack
(about 200 MB) and our own upscales from https://github.com/schindlershadow/dissidia-hd-textures (3,366 `<id>.png` files, about 1.1 GB, built with
`texpack_extract.py` and `texpack_upscale.py`; its README and `manifest.json` say what each file is). Total download about 1.3 GB, on disk about
1.4 GB. If the second download fails the first is still installed; `--no-ids` skips it, `--ids-zip FILE` uses a local archive.
