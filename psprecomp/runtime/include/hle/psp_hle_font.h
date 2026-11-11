// Host TrueType files that stand in for the PSP firmware fonts (sceFont HLE).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Resolves the regular face (PSPRECOMP_FONT, else DejaVu Sans) and its
// "<name>-Bold.ttf" sibling (empty when absent). Returns false if no regular
// face is readable. Text drawn by the game through sceFont uses exactly these
// files, so UI the runtime draws itself can match it.
bool psp_font_host_files(std::string* regular, std::string* bold);

// HD text. The game draws its text lines (item names, descriptions, numbers, subtitles) with sceFontGetCharGlyphImage
// into small 4-bit buffers that it later uses as CLUT4 textures. Besides the 1x glyph the HLE draws every glyph at
// PSPRECOMP_TEXT_SCALE x (default 3, 1 = off) into a shadow canvas keyed by the buffer's content. When such a
// buffer is bound as a texture, psp_font_hd_strip() returns the canvas as 8-bit coverage, width*scale x height*scale
// (parts of the buffer the font did not draw, e.g. the game's own digit sprites, are bilinear-upscaled from the 1x
// buffer). Returns false when the texture is not one of those buffers (any content change invalidates the canvas).
int psp_font_hd_scale();
bool psp_font_hd_strip(const uint8_t* mem, size_t avail, int width, int height, int bufw,
                       std::vector<uint8_t>& cov, int& hw, int& hh);
