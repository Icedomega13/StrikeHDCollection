/*
 * Amiga assets from the player's own Desert Strike disk images (ADF), for
 * the ultimate edition (docs/13-amiga-ultimate.md). Nothing from the Amiga
 * version ships: the disks are read at startup from <settings>/amiga/.
 *
 * The disks use EA's own loader; data blocks are found by their Rob Northen
 * ProPack (RNC) headers and verified by CRC (stored blocks, "RNC\0", too).
 * The game's sound effects are one bank (a single-hunk program block: a
 * table of offset, period range and length, then the samples). Other blocks
 * of raw signed 8-bit audio are the songs' instruments; the ProTracker music is rebuilt from the song data in
 * the program blocks plus those instruments.
 */
#ifndef DS_AMIGA_H
#define DS_AMIGA_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char id[20];               /* "a" + 16 hex digits of a content hash: stable across copies */
    int8_t *pcm;               /* signed 8-bit */
    int len;
    char label[24];            /* "fx 3" (the game's effect bank) or "datitle:Radio1" (a song's instrument) */
    int rate;                  /* the game's own playback rate in Hz (effect bank), else 0 */
    int per_lo, per_hi;        /* effect bank: the two Paula periods the game picks from (rate = their mean) */
} amiga_sample;

typedef struct {
    char name[24];             /* the module's own title, e.g. "datitle", "death" */
    uint8_t *data;             /* a complete M.K. module */
    int len;
    int complete;              /* every instrument was found */
} amiga_module;

typedef struct {
    int w, h;
    uint32_t *rgba;            /* ARGB8888, w * h */
} amiga_picture;

typedef struct {
    char id[20];               /* "c" + 16 hex digits of a content hash */
    int w, h, nframes;
    uint8_t **frames;          /* palette indices, w * h each */
    int *delay;                /* per frame, in 1/60 s (ANIM reltime) */
    uint32_t pal[256];         /* ARGB */
} amiga_anim;

typedef struct {
    char owner[20];            /* the picture (amiga_anim id) its program draws it on */
    int index;                 /* in that program's bob table */
    int w, h;
    uint8_t *mask, *px;        /* w * h: 1 = drawn; palette index (the picture's palette) */
} amiga_bob;

typedef struct {
    char owner[20];            /* id of the first picture it holds */
    uint8_t *data;             /* the whole AmigaDOS hunk file */
    size_t n;
} amiga_program;

/* Unpack an RNC block at src (header included). Returns malloc'd data or
 * NULL; *out_len = unpacked size. CRCs are checked. */
uint8_t *rnc_unpack(const uint8_t *src, size_t avail, size_t *out_len);

/* Scan every .adf in dir for samples. Returns the number found. */
int amiga_load_dir(const char *dir);
int amiga_sample_count(void);
const amiga_sample *amiga_sample_at(int i);
int amiga_sample_find(const char *id);   /* -1 if absent */
int amiga_module_count(void);
const amiga_module *amiga_module_at(int i);
int amiga_module_find(const char *name); /* -1 if absent */
/* The title photographs (shown by the Amiga intro), in disk order. */
int amiga_picture_count(void);
/* Cutscene pictures (one frame) and animations, from every program block. */
int amiga_anim_count(void);
const amiga_anim *amiga_anim_at(int i);
int amiga_anim_find(const char *id);
int amiga_bob_count(void);
const amiga_bob *amiga_bob_at(int i);
int amiga_program_count(void);
const amiga_program *amiga_program_at(int i);
const amiga_program *amiga_program_find(const char *owner);

const amiga_picture *amiga_picture_at(int i);

#endif
