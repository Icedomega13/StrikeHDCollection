/*
 * HD graphics packs (docs/09-hd-packs.md).
 *
 * Replacement art is keyed by a hash of an 8x8 pattern's 32 bytes, so it
 * follows the graphics wherever the game loads them in VRAM, under any
 * palette. A pack folder holds, in any subfolders:
 *   <hash>.png        one pattern, 8*S x 8*S pixels (S = 1..4)
 *   spr_<id>.png/.txt a sheet of patterns (a whole sprite or any block),
 *                     with the .txt listing the pattern hashes per cell
 *   palettes.txt      "<hash> c0 .. c15": colours a <hash>.png was drawn
 *                     for, so palette fades and flashes still apply
 * The dumper writes exactly these files from what is on screen.
 */
#ifndef DS_HDPACK_H
#define DS_HDPACK_H
#include "hw/vdp_render.h"

int  hdpack_load(const char *dir);      /* adds a folder; returns patterns added */
void hdpack_unload(void);
int  hdpack_scale(void);                /* 0 = nothing loaded */
int  hdpack_count(void);
/* Per frame, before vdp_render: point hd->tile[] at art for the patterns
 * currently in VRAM, and set hd->scale. */
void hdpack_update(vdp_hd *hd);
/* After vdp_render: write patterns and sprites seen this frame that were not
 * dumped before into dir (ends in a separator). Returns files written. */
int  hdpack_dump(const char *dir, int width);

/* Dump the visible cells of a painted world plane as a context sheet. */
int  hdpack_dump_plane(const char *dir, const vdp_world_plane *wp);

/* Upscale everything in a dump folder 4x (Scale2x twice) into a pack
 * folder: a quick starter pack. Returns images written. */
int  hdpack_make_starter(const char *dump_dir, const char *pack_dir);

#endif
