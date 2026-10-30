/*
 * Desert Strike HD: the PC host. Opens a window, finds and verifies the ROM,
 * then runs the recompiled game one frame at a time: input in, picture and
 * sound out, paced to the Genesis frame rate (59.92 Hz). Esc / the pad's
 * Back button opens the HD pause menu (menu.c).
 *
 * usage: DesertStrikeHD [rom] [test options]
 * Test options (automated checks, run as fast as possible):
 *   --frames N              stop after N loop ticks
 *   --inputs file           scripted pad input ("<frame> <UDLRABCS>")
 *   --shot-every N dir      save the game image every N frames
 *   --make-hd-pack          build the starter HD pack from the dump first
 *   --extract [campaign]    dump all graphics: intros, then every campaign's map (headless)
 *   --key T:NAME            press key NAME at tick T (repeatable)
 *   --screen-shot T:path    save the whole window at tick T (repeatable)
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include "runtime/cpu.h"
#include "game.h"
#include "hw/genesis.h"
#include "hw/sound.h"
#include "hw/vdp_render.h"
#include "hdpack.h"
#include "util/sha1.h"
#include "input.h"
#include "settings.h"
#include "menu.h"
#include "ui.h"
#include "rewind.h"
#include "stats.h"
#include "amiga.h"
#include "amiga_vm.h"
#include "sfxmix.h"
#include "modplay.h"
#include "crt.h"
#include "gpumon.h"

static void music_stop(void);
/* Amiga extras: moments the Genesis has no sound for (docs/13-amiga-ultimate.md).
 * Ids sit after the game's effect ids in amiga_sfx.ini and the Sound Lab. */
enum { EV_OBJECTIVE = 1001, EV_MISSION_START, EV_LOW_FUEL, EV_LIFE_LOST, EV_RESCUE_FAR, EV_RESCUE_NEAR };
#define SFX_ROTOR 1010                      /* the helicopter's rotor loop (amiga_sfx.ini id) */
static void rotor_frame(void);
static void scene_frame(void);
static void scene_step(void);
static void draw_scene_swap(void);
static void amiga_event(int ev);

#define APP_TITLE   "Strike Collection HD"
#define FRAME_NS    16688154ull            /* 896040 / 53693175 s */
#define AUDIO_RATE  53267
#define THUMB_W     160
#define THUMB_H     112

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex, *upscaled;
static SDL_AudioStream *aud;
static char *pref;                          /* settings/save folder, ends in a separator */
static char settings_path[1024], controls_path[1024], stats_path[1024];
static const game_module *game;

static uint32_t fb[VDP_MAX_W * VDP_H];
static uint64_t fb_picture;                 /* counts the pictures rendered into fb */
static int fb_w = 320;
/* HD packs: frame composed at hd.scale x into hdfb when a pack is active */
static vdp_hd hd;
static uint64_t render_ns, render_frames;

/* Worker threads for the HD compose (vdp_render_opts.parallel). */
#define POOL_MAX 16
static struct {
    int n;                                   /* workers (the caller also works) */
    SDL_Thread *th[POOL_MAX];
    SDL_Semaphore *go[POOL_MAX], *done;
    void (*fn)(void *, int, int);
    void *ctx;
    int lines;
} pool;

static int SDLCALL pool_worker(void *arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        SDL_WaitSemaphore(pool.go[k]);
        if (!pool.fn) break;
        int parts = pool.n + 1;
        pool.fn(pool.ctx, pool.lines * (k + 1) / parts, pool.lines * (k + 2) / parts);
        SDL_SignalSemaphore(pool.done);
    }
    return 0;
}

static void pool_run(void (*fn)(void *, int, int), void *ctx, int lines)
{
    if (!pool.n) { fn(ctx, 0, lines); return; }
    pool.fn = fn; pool.ctx = ctx; pool.lines = lines;
    for (int k = 0; k < pool.n; k++) SDL_SignalSemaphore(pool.go[k]);
    fn(ctx, 0, lines / (pool.n + 1));
    for (int k = 0; k < pool.n; k++) SDL_WaitSemaphore(pool.done);
}

static void pool_start(void)
{
    int cores = SDL_GetNumLogicalCPUCores();
    int n = cores - 1;
    if (n > POOL_MAX) n = POOL_MAX;
    if (n > 7) n = 7;
    pool.done = SDL_CreateSemaphore(0);
    for (int k = 0; k < n; k++) {
        pool.go[k] = SDL_CreateSemaphore(0);
        pool.th[k] = SDL_CreateThread(pool_worker, "hd", (void *)(intptr_t)k);
        if (!pool.th[k]) break;
        pool.n = k + 1;
    }
}

static void pool_stop(void)
{
    pool.fn = NULL;
    for (int k = 0; k < pool.n; k++) SDL_SignalSemaphore(pool.go[k]);
    for (int k = 0; k < pool.n; k++) SDL_WaitThread(pool.th[k], NULL);
    pool.n = 0;
}   /* DS_PROFILE=1 prints the average at exit */
static uint32_t *hdfb;
static int hd_on;                            /* last frame was composed in HD */
static SDL_Texture *hdtex;
static char dump_dir[1100];
static vdp_render_opts ropts = { 0, 0, 1 };
static unsigned pad_now;
static int use_script;
static int running = 1, user_paused, focused = 1;

static char osd[128];
static uint64_t osd_until;

static void say(const char *msg)
{
    SDL_strlcpy(osd, msg, sizeof osd);
    osd_until = SDL_GetTicksNS() + 2000000000ull;
    SDL_Log("%s", msg);
}

/* ---------------- emulation callbacks ---------------- */
extern const unsigned char app_icon_png[];
extern const size_t app_icon_png_size;
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
static SDL_Texture *icon_tex;

/* The app icon (res/icon_256.png, embedded): window icon + launcher logo. */
static void load_icon(void)
{
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(app_icon_png, (int)app_icon_png_size, &w, &h, &n, 4);
    if (!px) return;
    SDL_Surface *sf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px, w * 4);
    if (sf) {
        SDL_SetWindowIcon(win, sf);
        icon_tex = SDL_CreateTextureFromSurface(ren, sf);
        if (icon_tex) SDL_SetTextureScaleMode(icon_tex, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(sf);
    }
    stbi_image_free(px);
}

static int hud_ok;
static vdp_world_plane ws_planes[2];          /* painted planes of the last frame */
static int ws_planes_ok;                          /* playfield on screen (set at vblank) */
static int extracting;                      /* graphics extraction running */
static int att_on;                          /* the emulation is running the launcher background */
static int sandbox;                         /* hidden frames (minimap capture): no host effects */
static unsigned sandbox_pad;                /* buttons held during hidden frames */

static void on_vblank(void)
{
    if (sandbox) return;
    if (att_on) {                            /* launcher background: plain 4:3 */
        static const vdp_render_opts plain = { 0, 0, 0 };
        vdp_render(fb, VDP_MAX_W, &plain);
        fb_picture++;
        fb_w = vdp_display_width();
        return;
    }
    /* Widescreen only applies to the scrolling playfield; other screens
     * are pillarboxed so the picture size never jumps. */
    /* playfield test, confirmed by the module's map check (ws_world returns
     * 0 when the planes don't hold the map, e.g. during a cutscene) */
    vdp_world_plane *wp = ws_planes;
    int play = game && game->ws_active && game->ws_active(), map_fail = 0;
    if (play && game->ws_world && !game->ws_world(wp, ropts.ext_left, ropts.ext_right)) { play = 0; map_fail = 1; }
    /* Anything that isn't the live battlefield (menus, maps, cutscenes) is
     * shown 4:3. After a frame where the planes held something other than
     * the map (a cutscene drawn over it), wait half a second of clean
     * playfield before widening again, so a cutscene never flickers wide.
     * Coming back from a screen that simply isn't the playfield (the pause
     * map and status screens reset the scroll) needs no wait (issue #7). */
    static int settle, plain;
    if (map_fail) { settle = 30; plain = 0; }
    else if (!play) { settle = ++plain >= 20 ? 6 : SDL_max(settle, 6); }   /* a while off the playfield: a short wait (a respawn flashes the map for 2 frames) */
    else if (settle > 0) { settle--; play = 0; }
    if (play) plain = 0;
    if (SDL_getenv("DS_WS_LOG")) { static int was = -1, wf = -1; int st = play * 4 + map_fail * 2 + (game && game->ws_active && game->ws_active()); if (st != was || (g_frame & 7) == 0) SDL_Log("ws play %d mapfail %d active %d settle %d flight %llu frame %llu", play, map_fail, st & 1, settle, (unsigned long long)g_ws_flight_frame, (unsigned long long)g_frame); was = st; (void)wf; }
    int active = ropts.ext_left && play;
    hud_ok = play;
    ropts.world[0] = ropts.world[1] = NULL;
    /* Sprite X as the game's full signed position at every widescreen width
     * (issue #11): read as 9 bits, a big sprite just off the left edge wraps
     * to x + 512 and shows as a sliver in the right-hand extension. */
    ropts.wide_sprites = active;
    ws_planes_ok = active && game->ws_world;
    if (active && game->ws_world && ropts.ext_left > 56) {
        ropts.world[0] = &wp[0];
        ropts.world[1] = &wp[1];
        ropts.wide_sprites = 1;
    }
    ropts.hd = NULL;
    hd_on = 0;
    if (g_set.hd_graphics && hdpack_scale() && hdfb) {
        hdpack_update(&hd);
        hd.fb = hdfb;
        hd.pitch = VDP_MAX_W * 4;
        ropts.hd = &hd;
        ropts.parallel = pool_run;
        if (!pool.done) pool_start();
        hd_on = 1;
    }
    uint64_t t0 = SDL_GetTicksNS();
    vdp_render(fb, VDP_MAX_W, &ropts);
    fb_picture++;
    render_ns += SDL_GetTicksNS() - t0; render_frames++;
    fb_w = ropts.ext_left + vdp_display_width() + ropts.ext_right;
    if (g_set.hd_dump && game) {
        int n = hdpack_dump(dump_dir, fb_w);
        static int total;
        if (n) { total += n; char m[96]; SDL_snprintf(m, sizeof m, "HD dump: %d new graphics", total); say(m); }
    }
    if (ropts.ext_left && !active && hd_on) {
        int S = hd.scale;
        for (int y = 0; y < VDP_H * S; y++) {
            uint32_t *row = hdfb + (size_t)y * hd.pitch;
            for (int x = 0; x < ropts.ext_left * S; x++) row[x] = 0xFF000000u;
            for (int x = (fb_w - ropts.ext_right) * S; x < fb_w * S; x++) row[x] = 0xFF000000u;
        }
    }
    if (ropts.ext_left && !active) {
        for (int y = 0; y < VDP_H; y++) {
            uint32_t *row = fb + y * VDP_MAX_W;
            for (int x = 0; x < ropts.ext_left; x++) row[x] = 0xFF000000u;
            for (int x = fb_w - ropts.ext_right; x < fb_w; x++) row[x] = 0xFF000000u;
        }
    }
}

static void on_frame(void)
{
    if (sandbox) { g_pad_buttons[0] = (uint16_t)sandbox_pad; cpu_frame_yield(); return; }
    if (att_on) g_pad_buttons[0] = 0;
    else if (!use_script) g_pad_buttons[0] = (uint16_t)pad_now;
    if (!att_on && !extracting && game && game->cheats && hud_ok &&
        (g_cheat.god || g_cheat.fuel || g_cheat.ammo || g_cheat.lives))
        game->cheats(g_cheat.god, g_cheat.fuel, g_cheat.ammo, g_cheat.lives);
    cpu_frame_yield();
}

static jmp_buf sandbox_jmp;
static void on_fatal(void)
{
    if (sandbox) longjmp(sandbox_jmp, 1);    /* hidden frames: abandon and restore */
    char msg[256];
    SDL_snprintf(msg, sizeof msg, "The game hit code that has not been recompiled (PC %06X, frame %llu).\n"
                 "Please report this along with what you were doing.", cpu_cur_pc, (unsigned long long)g_frame);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_TITLE, msg, win);
}

/* ---------------- ROM library ----------------
 * Every supported game is found by SHA-1 in <settings>/roms and next to the
 * exe (roms/, rom/, the exe folder, ../rom/). A ROM picked with "Add ROM" is
 * verified and copied to <settings>/roms/<id>.bin. */
#define MAX_GAMES 8
static char rom_path_of[MAX_GAMES][1024];

static int game_index(const game_module *g)
{
    for (int i = 0; i < g_game_count; i++) if (g_games[i] == g) return i;
    return -1;
}

static const game_module *identify_file(const char *path)
{
    size_t n = 0;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) return 0;
    if (info.size < 0x20000 || info.size > 0x400000) return 0;
    void *d = SDL_LoadFile(path, &n);
    if (!d) return 0;
    char h[41];
    sha1_hex(d, n, h);
    SDL_free(d);
    return game_find_sha1(h);
}

static SDL_EnumerationResult SDLCALL scan_cb(void *ud, const char *dir, const char *name)
{
    (void)ud;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    const game_module *g = identify_file(path);
    int i = g ? game_index(g) : -1;
    if (i >= 0 && !rom_path_of[i][0]) SDL_strlcpy(rom_path_of[i], path, sizeof rom_path_of[i]);
    return SDL_ENUM_CONTINUE;
}

static void scan_roms(void)
{
    memset(rom_path_of, 0, sizeof rom_path_of);
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%sroms", pref);
    SDL_EnumerateDirectory(dir, scan_cb, 0);
    const char *base = SDL_GetBasePath();
    const char *places[] = { "%sroms", "%srom", "%s", "%s../rom" };
    for (int i = 0; base && i < 4; i++) {
        SDL_snprintf(dir, sizeof dir, places[i], base);
        SDL_EnumerateDirectory(dir, scan_cb, 0);
    }
}

/* Copy a verified ROM into <pref>/roms/<id>.bin so it is found next time. */
static void remember_rom(const char *src, const game_module *g)
{
    char dir[1024], dst[1024];
    SDL_snprintf(dir, sizeof dir, "%sroms", pref);
    SDL_CreateDirectory(dir);
    SDL_snprintf(dst, sizeof dst, "%s/%s.bin", dir, g->id);
    if (SDL_strcmp(src, dst)) SDL_CopyFile(src, dst);
}

static volatile int dialog_state;            /* 0 waiting, 1 picked, 2 cancelled */
static char dialog_path[1024];

static void SDLCALL dialog_cb(void *ud, const char *const *files, int filter)
{
    (void)ud; (void)filter;
    if (files && files[0]) { SDL_strlcpy(dialog_path, files[0], sizeof dialog_path); dialog_state = 1; }
    else dialog_state = 2;
}

/* File picker for a ROM. Returns the game it turned out to be, or NULL. */
static const game_module *add_rom_dialog(void)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Genesis / Mega Drive ROM", "bin;md;gen;smd" }, { "All files", "*" },
    };
    dialog_state = 0;
    SDL_ShowOpenFileDialog(dialog_cb, 0, win, filters, 2, 0, false);
    while (dialog_state == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) { running = 0; return 0; }
        SDL_Delay(10);
    }
    if (dialog_state == 2) return 0;
    const game_module *g = identify_file(dialog_path);
    if (!g) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, APP_TITLE,
            "That file is not a supported dump.\n\nSupported: the USA/Europe releases of Desert Strike,\n"
            "Jungle Strike and Urban Strike for the Sega Genesis / Mega Drive.", win);
        return 0;
    }
    remember_rom(dialog_path, g);
    scan_roms();
    return g;
}

/* ---------------- save states ----------------
 * File: "DSHDST02" | game id[8] | int64 time (ns) | thumbnail 160x112 XRGB | state.
 * Version-1 files ("DSHDSTATE" + id, 16-byte header, no thumbnail) still load. */
#define ST_MAGIC   "DSHDST02"
#define ST_HDR     (8 + 8 + 8 + THUMB_W * THUMB_H * 4)

static int watch_reset = 1;                 /* re-baseline the status watcher (after loads, rewind) */
#define RESUME_SLOT MENU_SLOTS              /* written when the game is left mid-play; not in the slot grid */
static int resume_worthy;                   /* the playfield was reached this session */
#define THREAT_MAX 48
static struct { int n; int16_t x[THREAT_MAX], y[THREAT_MAX]; } threats;
static int load_fresh;                      /* a state was loaded: the next level seen is not a new mission */
static SDL_Texture *slot_tex[MENU_SLOTS];
static int slot_known[MENU_SLOTS], slot_has[MENU_SLOTS];
static char slot_when[MENU_SLOTS][32];

static void state_path(char *out, size_t n, int slot)
{
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%sstates", pref);
    SDL_CreateDirectory(dir);
    if (slot == RESUME_SLOT) SDL_snprintf(out, n, "%s/%s_resume.state", dir, game->id);
    else if (slot == 0) SDL_snprintf(out, n, "%s/%s_quick.state", dir, game->id);
    else if (slot >= MENU_SAVE_SLOTS) SDL_snprintf(out, n, "%s/%s_auto%d.state", dir, game->id, slot - MENU_SAVE_SLOTS + 1);
    else SDL_snprintf(out, n, "%s/%s_slot%d.state", dir, game->id, slot);
}

static void make_thumb(uint32_t *out)
{
    for (int y = 0; y < THUMB_H; y++)
        for (int x = 0; x < THUMB_W; x++) {
            int sx = x * fb_w / THUMB_W, sy = y * 2;
            uint32_t a = fb[sy * VDP_MAX_W + sx], b = fb[sy * VDP_MAX_W + sx + 1];
            uint32_t c = fb[(sy + 1) * VDP_MAX_W + sx], d = fb[(sy + 1) * VDP_MAX_W + sx + 1];
            uint32_t r = (((a >> 16) & 255) + ((b >> 16) & 255) + ((c >> 16) & 255) + ((d >> 16) & 255)) / 4;
            uint32_t g = (((a >> 8) & 255) + ((b >> 8) & 255) + ((c >> 8) & 255) + ((d >> 8) & 255)) / 4;
            uint32_t bl = ((a & 255) + (b & 255) + (c & 255) + (d & 255)) / 4;
            out[y * THUMB_W + x] = 0xFF000000u | r << 16 | g << 8 | bl;
        }
}

static int write_state(int slot)
{
    size_t n = cpu_state_size();
    uint8_t *b = SDL_calloc(1, ST_HDR + n);
    memcpy(b, ST_MAGIC, 8);
    SDL_strlcpy((char *)b + 8, game->id, 8);
    SDL_Time t = 0;
    SDL_GetCurrentTime(&t);
    memcpy(b + 16, &t, 8);
    make_thumb((uint32_t *)(b + 24));
    cpu_state_save(b + ST_HDR);
    char path[1024];
    state_path(path, sizeof path, slot);
    int ok = SDL_SaveFile(path, b, ST_HDR + n);
    SDL_free(b);
    if (slot < MENU_SLOTS) slot_known[slot] = 0;
    return ok;
}

static void save_slot(int slot)
{
    int ok = write_state(slot);
    char m[64];
    if (slot == 0) SDL_snprintf(m, sizeof m, ok ? "Quick saved" : "Could not save state");
    else SDL_snprintf(m, sizeof m, ok ? "Saved to slot %d" : "Could not save state", slot);
    say(m);
}

static int load_slot(int slot)
{
    char path[1024];
    state_path(path, sizeof path, slot);
    size_t n = 0;
    uint8_t *b = SDL_LoadFile(path, &n);
    if (!b) { say("That slot is empty"); return 0; }
    const uint8_t *st = 0;
    if (n == ST_HDR + cpu_state_size() && !memcmp(b, ST_MAGIC, 8) && !SDL_strcmp((char *)b + 8, game->id))
        st = b + ST_HDR;
    else if (n == 16 + cpu_state_size() && !memcmp(b, "DSHDSTATE", 9) && !SDL_strcmp((char *)b + 9, game->id))
        st = b + 16;
    int ok = st && cpu_state_load(st);
    SDL_free(b);
    if (ok) {
        if (aud) SDL_ClearAudioStream(aud);
        music_stop();
        rw_clear();
        watch_reset = 1;
        load_fresh = 1;                          /* the level it lands in is not a new mission */
        say(slot == RESUME_SLOT ? "Welcome back" : slot ? "State loaded" : "Quick state loaded");
    } else {
        say("That state is from another version");
    }
    return ok;
}

static int slot_info(int slot, char *when, size_t wn, SDL_Texture **thumb)
{
    if (!slot_known[slot]) {
        slot_known[slot] = 1;
        slot_has[slot] = 0;
        slot_when[slot][0] = 0;
        if (slot_tex[slot]) { SDL_DestroyTexture(slot_tex[slot]); slot_tex[slot] = 0; }
        char path[1024];
        state_path(path, sizeof path, slot);
        size_t n = 0;
        uint8_t *b = SDL_LoadFile(path, &n);
        if (b) {
            slot_has[slot] = 1;
            SDL_Time t = 0;
            if (n >= ST_HDR && !memcmp(b, ST_MAGIC, 8)) {
                memcpy(&t, b + 16, 8);
                slot_tex[slot] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, THUMB_W, THUMB_H);
                if (slot_tex[slot]) {
                    SDL_UpdateTexture(slot_tex[slot], 0, b + 24, THUMB_W * 4);
                    SDL_SetTextureScaleMode(slot_tex[slot], SDL_SCALEMODE_LINEAR);
                }
            } else {
                SDL_PathInfo pi;
                if (SDL_GetPathInfo(path, &pi)) t = pi.modify_time;
            }
            SDL_DateTime dt;
            if (t && SDL_TimeToDateTime(t, &dt, true))
                SDL_snprintf(slot_when[slot], sizeof slot_when[slot], "%04d-%02d-%02d %02d:%02d",
                             dt.year, dt.month, dt.day, dt.hour, dt.minute);
            SDL_free(b);
        }
    }
    SDL_strlcpy(when, slot_when[slot], wn);
    *thumb = slot_tex[slot];
    return slot_has[slot];
}

/* ---------------- display modes / settings ---------------- */
static SDL_DisplayMode modes[64];
static char mode_names[64][48];
static int nmodes;

static void load_modes(void)
{
    nmodes = 0;
    int n = 0;
    SDL_DisplayMode **m = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(win), &n);
    for (int i = 0; m && i < n && nmodes < 64; i++) {
        modes[nmodes] = *m[i];
        SDL_snprintf(mode_names[nmodes], sizeof mode_names[0], "%d x %d  %.0f Hz", m[i]->w, m[i]->h, m[i]->refresh_rate);
        nmodes++;
    }
    SDL_free(m);
}

static int mode_count(void) { return nmodes; }
static const char *mode_name(int i) { return i >= 0 && i < nmodes ? mode_names[i] : "Desktop"; }

static int applied_mode = -2, applied_fs = -2;

static void apply_window_mode(void)
{
    if (applied_mode == g_set.window_mode && applied_fs == g_set.fullscreen_mode) return;
    applied_mode = g_set.window_mode;
    applied_fs = g_set.fullscreen_mode;
    switch (g_set.window_mode) {
    case WIN_WINDOWED:
        SDL_SetWindowFullscreen(win, false);
        break;
    case WIN_BORDERLESS:
        SDL_SetWindowFullscreenMode(win, 0);
        SDL_SetWindowFullscreen(win, true);
        break;
    case WIN_EXCLUSIVE:
        if (g_set.fullscreen_mode >= 0 && g_set.fullscreen_mode < nmodes)
            SDL_SetWindowFullscreenMode(win, &modes[g_set.fullscreen_mode]);
        else {
            const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(win));
            if (dm) SDL_SetWindowFullscreenMode(win, dm);
        }
        SDL_SetWindowFullscreen(win, true);
        break;
    }
}

static void apply_audio(void)
{
    snd_set_levels(g_set.fm_level, g_set.psg_level);
    if (aud) SDL_SetAudioStreamGain(aud, (!focused && g_set.mute_unfocused) ? 0.0f : g_set.volume / 100.0f);
}

static int out_w = 1280, out_h = 720;   /* last render output size */

/* Extra width per side. "Match window" fills the window's shape (at the
 * chosen pixel aspect). Beyond 56 px the game module paints the planes
 * from map data (ws_world). */
static void apply_ws(void)
{
    int ext = ws_ext_px[g_set.widescreen];
    if (g_set.widescreen == WS_AUTO && out_h > 0) {
        double a = (double)out_w / out_h;
        double fw = g_set.aspect == ASPECT_SQUARE ? a * VDP_H : a * 240.0;   /* 4:3 shape: 320 px = 240 * 4/3 */
        ext = g_set.aspect == ASPECT_STRETCH ? 0 : ((int)((fw - 320) / 2) + 4) / 8 * 8;
        if (ext < 0) ext = 0;
    }
    int max_ext = game ? game->ws_max_ext : 0;
    if (ext > 56 && !(game && game->ws_world)) ext = 56;
    if (ext > max_ext) ext = max_ext;
    if ((VDP_MAX_W - 320) / 2 < ext) ext = (VDP_MAX_W - 320) / 2 / 8 * 8;
    g_ws_ext = ext;
    g_ws_win = ext < 56 ? ext : 56;
    ropts.ext_left = ropts.ext_right = ext;
}

/* Edition presets: picking one sets the per-feature options; changing one
 * of those by hand makes the edition Custom. Features so far: sound effects,
 * music and the intro. */
static void apply_edition(void)
{
    static int prev_edition = -1, prev_sfx = -1, prev_music = -1, prev_intro = -1, prev_extras = -1, prev_cuts = -1;
    if (prev_edition >= 0 && g_set.edition != prev_edition) {
        if (g_set.edition == EDITION_GENESIS) g_set.amiga_sfx = g_set.amiga_music = g_set.amiga_intro = g_set.amiga_extras = g_set.amiga_cutscenes = 0;
        else if (g_set.edition == EDITION_AMIGA) g_set.amiga_sfx = g_set.amiga_music = g_set.amiga_intro = g_set.amiga_extras = g_set.amiga_cutscenes = 1;
        /* Remaster: the Amiga effects were the clear favourite, and its title
         * and interlude music and cinematic intro were rated above the Genesis's. */
        else if (g_set.edition == EDITION_REMASTER) g_set.amiga_sfx = g_set.amiga_music = g_set.amiga_intro = g_set.amiga_extras = g_set.amiga_cutscenes = 1;
    } else if ((prev_sfx >= 0 && g_set.amiga_sfx != prev_sfx) || (prev_music >= 0 && g_set.amiga_music != prev_music) ||
               (prev_intro >= 0 && g_set.amiga_intro != prev_intro) || (prev_extras >= 0 && g_set.amiga_extras != prev_extras) ||
               (prev_cuts >= 0 && g_set.amiga_cutscenes != prev_cuts)) {
        g_set.edition = EDITION_CUSTOM;
    }
    prev_intro = g_set.amiga_intro;
    prev_extras = g_set.amiga_extras;
    prev_cuts = g_set.amiga_cutscenes;
    if (prev_music >= 0 && !g_set.amiga_music) mod_stop();
    mod_set_volume(g_set.amiga_music_vol / 100.0f);
    prev_edition = g_set.edition;
    prev_sfx = g_set.amiga_sfx;
    prev_music = g_set.amiga_music;
}

static const SDL_DisplayMode *display_mode(void);

static void apply_settings(void)
{
    apply_edition();
    apply_window_mode();
    SDL_SetRenderVSync(ren, g_set.vsync ? 1 : 0);
    {   /* what the display really does (tearing, judder reports) */
        int vs = -9;
        SDL_GetRenderVSync(ren, &vs);
        const SDL_DisplayMode *dm = display_mode();
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        SDL_Log("video: %s, display %dx%d @ %.2f Hz, window %dx%d (mode %d), renderer %s, vsync asked %d got %d",
                SDL_GetCurrentVideoDriver(), dm->w, dm->h, dm->refresh_rate,
                ww, wh, g_set.window_mode, SDL_GetRendererName(ren), g_set.vsync, vs);
    }
    ropts.sprite_limits = g_set.sprite_flicker;
    g_frame_insns = g_set.authentic_speed ? GEN_FRAME_INSNS_AUTHENTIC : GEN_FRAME_INSNS;
    apply_ws();
    apply_audio();
    settings_save(settings_path);
}

static void save_controls(void) { input_save(controls_path); }

static void restart_game(void)
{
    game_boot(game);
    rw_clear();
    if (aud) SDL_ClearAudioStream(aud);
    say("Game restarted");
}

static void quit_game(void) { running = 0; }

static void open_settings_folder(void)
{
    char url[1200] = "file:///";
    size_t k = strlen(url);
    for (const char *p = pref; *p && k < sizeof url - 1; p++) url[k++] = *p == '\\' ? '/' : *p;
    url[k] = 0;
    if (url[8] == '/') memmove(url + 7, url + 8, strlen(url + 8) + 1);   /* unix: file:///home/... */
    SDL_OpenURL(url);
}

static void toggle_fullscreen(void)
{
    static int last_fs = WIN_BORDERLESS;
    if (g_set.window_mode == WIN_WINDOWED) g_set.window_mode = last_fs;
    else { last_fs = g_set.window_mode; g_set.window_mode = WIN_WINDOWED; }
    apply_settings();
}

/* ---------------- video ---------------- */
static double fps_game, fps_disp;
static int sync_on;                         /* frame pacing: by the display now (see pacing_update) */

/* Show FPS: Diagnostics. Measures over the last second, taken together each
 * second (diag_second) so a log line describes one stretch of time. */
typedef struct {
    double sum_ms, max_ms;
    int n, late, early;                     /* refreshes missed; presents that didn't wait for one */
} present_stat;
typedef struct {
    float min_ms, max_ms, ratio;
    int n, clears, starved;                 /* queue thrown away (too full); queue nearly empty */
} audio_stat;
static present_stat pstat_now, pstat;
static audio_stat astat_now = { 0, 0, 1, 0, 0, 0 }, astat;
static gpu_stat gstat;                      /* graphics card */
static int compositing;                     /* an X11 compositing manager is running (-1 unknown) */

/* The display's refresh rate (0 if unknown) and whether it suits the game. */
static const SDL_DisplayMode no_mode;
static const SDL_DisplayMode *display_mode(void)
{
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    return dm ? dm : &no_mode;
}
static float display_hz(void) { return display_mode()->refresh_rate; }
static int near_60(float hz) { return hz > 59.0f && hz < 61.0f; }

/* X11: a compositing manager owns the _NET_WM_CM_S<screen> selection (the
 * standard way to ask). One that copies a fullscreen window on its own clock,
 * picom with NVIDIA, tears it. */
static int find_compositing(void)
{
#ifdef __linux__
    static SDL_SharedObject *x11;
    static int tried;
    static unsigned long (*intern)(void *, const char *, int);
    static unsigned long (*owner)(void *, unsigned long);
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11")) return 0;
    void *dpy = SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    if (!tried++ && (x11 = SDL_LoadObject("libX11.so.6"))) {
        *(void **)&intern = (void *)SDL_LoadFunction(x11, "XInternAtom");
        *(void **)&owner = (void *)SDL_LoadFunction(x11, "XGetSelectionOwner");
    }
    if (!dpy || !intern || !owner) return -1;
    char name[32];
    SDL_snprintf(name, sizeof name, "_NET_WM_CM_S%d", (int)SDL_GetNumberProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_X11_SCREEN_NUMBER, 0));
    return owner(dpy, intern(dpy, name, 0)) != 0;
#else
    return 0;
#endif
}

/* Hooks for more measures: lines in the Diagnostics box, fields in the log line. */
static void box_line(SDL_FColor c, const char *fmt, ...);

/* The CRT filter's lines: what runs, its GPU time a frame and each pass's. */
static void diag_overlay_extra(SDL_FColor ok, SDL_FColor warn, SDL_FColor bad)
{
    if (!g_set.crt) return;
    float ms = crt_gpu_ms();
    char passes[400];
    if (ms < 0.0f) {
        box_line(warn, "CRT filter not running: %s", crt_why_not()[0] ? crt_why_not() : "no game picture yet");
        return;
    }
    box_line(ms > 14.0f ? bad : ms > 10.0f ? warn : ok, "CRT %s   GPU %.1f ms a frame", crt_describe(), ms);
    if (crt_pass_times(passes, sizeof passes)) box_line(ok, "Passes: %s", passes);
    if (ms > 14.0f)
        box_line(warn, "The CRT filter takes %.0f of the 16.7 ms a frame has: try a lower resolution or an RGB preset.", ms);
}

static void diag_extra(char *buf, size_t n)
{
    float ms = g_set.crt ? crt_gpu_ms() : -1.0f;
    char passes[400];
    if (!g_set.crt) SDL_snprintf(buf, n, "crt=off");
    else if (ms < 0.0f) SDL_snprintf(buf, n, "crt=not-running");
    else {
        crt_pass_times(passes, sizeof passes);
        SDL_snprintf(buf, n, "crt_gpu_ms=%.2f crt=\"%s\" passes_ms=\"%s\"", ms, crt_describe(), passes);
    }
}

static SDL_FRect game_dst;                   /* where the game image was drawn */

/*
 * Status HUD: armour, fuel, load and lives in the game image's top-left
 * corner, in the pause menu's style. Sized from the image height so it
 * keeps its proportion in every window and widescreen setting.
 */
static float hud_box[4];                     /* where the status HUD was drawn (the minimap avoids it) */
static uint64_t hud_box_frame = (uint64_t)-1;
static void draw_hud(void)
{
    game_hud h;
    if (!g_set.status_hud || !game || !game->hud || !game->ws_active || menu_is_open()) return;
    if (!hud_ok || !game->hud(&h)) return;
    float x0, y0, x1, y1;
    ui_to_virtual(game_dst.x, game_dst.y, &x0, &y0);
    ui_to_virtual(game_dst.x + game_dst.w, game_dst.y + game_dst.h, &x1, &y1);
    float u = (y1 - y0) / 720.0f;                /* 1 = image fills a 720-high canvas */
    if (u < 0.55f) u = 0.55f;
    ui_font f = u < 1.25f ? UI_SMALL : UI_BODY;   /* text already scales with the window */
    u *= (g_set.hud_scale > 0 ? g_set.hud_scale : 100) / 100.0f;
    float lh = ui_line_h(f) * 0.92f;
    float lab = ui_text_w(f, "ARMOR") + 10 * u;
    float barw = 72 * u, valw = ui_text_w(f, "0000");
    float pad = 7 * u, w = pad * 2 + lab + barw + 8 * u + valw, rows = 5;
    float hgt = pad * 2 + lh * rows;
    float x = g_set.hud_pos & 1 ? x1 - 14 * u - w : x0 + 14 * u, y = g_set.hud_pos & 2 ? y1 - 12 * u - hgt : y0 + 12 * u;
    hud_box[0] = x; hud_box[1] = y; hud_box[2] = w; hud_box[3] = hgt; hud_box_frame = g_frame;
    SDL_FColor edge = ui_rgba(196, 164, 98, 200), fill = ui_rgba(14, 17, 12, 170);
    SDL_FColor label = ui_rgba(196, 164, 98, 255), text = ui_rgba(232, 226, 210, 255);
    SDL_FColor green = ui_rgba(118, 150, 64, 255), amber = ui_rgba(242, 178, 58, 255), red = ui_rgba(214, 74, 52, 255);
    SDL_FColor track = ui_rgba(40, 46, 32, 220);
    int blink = (SDL_GetTicks() / 300) & 1;
    ui_panel(x, y, w, hgt, 8 * u, fill, edge);
    float cx = x + pad, cy = y + pad;
    char v[32];
    const char *names[2] = { "ARMOR", "FUEL" };
    int val[2] = { h.armor, h.fuel }, max[2] = { h.armor_max, h.fuel_max };
    for (int r = 0; r < 2; r++) {
        float frac = max[r] > 0 ? (float)val[r] / (float)max[r] : 0;
        if (frac > 1) frac = 1;
        SDL_FColor c = frac > 0.5f ? green : frac > 0.25f ? amber : red;
        ui_text(f, cx, cy, label, names[r]);
        float by = cy + lh * 0.22f, bh = lh * 0.5f;
        ui_rect(cx + lab, by, barw, bh, track);
        if (!(frac <= 0.25f && blink)) ui_rect(cx + lab, by, barw * frac, bh, c);
        ui_frame(cx + lab, by, barw, bh, 1, ui_rgba(196, 164, 98, 110));
        SDL_snprintf(v, sizeof v, "%d", val[r]);
        ui_text_right(f, x + w - pad, cy, frac <= 0.25f ? red : text, v);
        cy += lh;
    }
    ui_text(f, cx, cy, label, "LOAD");
    if (h.load_max > 0 && h.load_max <= 12) {
        float pw = (barw - (h.load_max - 1) * 3 * u) / h.load_max, by = cy + lh * 0.22f, bh = lh * 0.5f;
        for (int i = 0; i < h.load_max; i++)
            ui_rect(cx + lab + i * (pw + 3 * u), by, pw, bh, i < h.load ? (h.load >= h.load_max ? amber : green) : track);
    }
    SDL_snprintf(v, sizeof v, h.load_max > 0 ? "%d/%d" : "%d", h.load, h.load_max);
    ui_text_right(f, x + w - pad, cy, text, v);
    cy += lh;
    ui_text(f, cx, cy, label, "LIVES");
    SDL_snprintf(v, sizeof v, "%d", h.lives);
    ui_text_right(f, x + w - pad, cy, h.lives <= 1 ? amber : text, v);
    cy += lh;
    /* ammo: guns, hydras, hellfires across one row */
    static const char *const an[3] = { "G", "HY", "HF" };
    float colw = (w - pad * 2) / 3;
    for (int i = 0; i < 3; i++) {
        float ax = cx + i * colw;
        float lw = ui_text(f, ax, cy, label, an[i]);
        if (h.ammo[i] < 0) SDL_strlcpy(v, "-", sizeof v);
        else SDL_snprintf(v, sizeof v, "%d", h.ammo[i]);
        ui_text(f, ax + lw + 3 * u, cy, h.ammo[i] == 0 ? red : text, v);
    }
}

/* ---------------- minimap (docs/11-minimap.md) ----------------
 * The terrain is the pause map's own picture. When a level is first seen on
 * the playfield, the game is run a few hidden frames from a saved state with
 * Start pressed, the pause map is rendered without its icons and cropped to
 * the playable world, and the state is restored; the picture is cached in
 * <settings>/minimap/. The markers are live from the game (game->minimap). */
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

static struct {
    const game_module *game;
    uint32_t level;               /* level the terrain belongs to */
    int state;                    /* 0 not yet, 1 terrain ready, 2 capture failed */
    int tries;
    uint64_t retry_frame;
    SDL_Texture *tex;
    int r[4];                     /* crop on the pause screen, 1x px */
    uint64_t name_until;          /* show the selected entry's name until (ns) */
} mm;
static game_minimap mmd;
static int mmd_ok;
static uint32_t mmfb[VDP_MAX_W * VDP_H];

/* The playable world on the pause map, in 1x screen pixels. */
static int mm_rect(const game_minimap *d, int r[4], int screen_w)
{
    const game_mm_view *v = game->mm_view;
    int s = d->shift ? d->shift : v->shift, round = (1 << s) - 1;
    int x0 = v->origin_x + (d->x0 >> s), y0 = v->origin_y + (d->y0 >> s);
    int x1 = v->origin_x + ((d->x1 + round) >> s), y1 = v->origin_y + ((d->y1 + round) >> s);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > screen_w) x1 = screen_w;
    if (y1 > VDP_H) y1 = VDP_H;
    r[0] = x0; r[1] = y0; r[2] = x1 - x0; r[3] = y1 - y0;
    return r[2] >= 16 && r[3] >= 16;
}

static void drain_sound(void)
{
    static int16_t b[4096 * 2];
    while (snd_read(b, 4096) > 0) {}
}

/* Hidden frames: open the pause map and grab its picture into out (r[2] x r[3]). */
static int mm_capture(uint32_t *out, int r[4])
{
    const game_mm_view *v = game->mm_view;
    size_t n = cpu_state_size();
    uint8_t *st = SDL_malloc(n);
    if (!st) return 0;
    cpu_state_save(st);
    uint32_t sa = v->sentinel & 0xFFFF;
    g_ram[sa] = (uint8_t)~(v->sentinel_val >> 8);
    g_ram[(sa + 1) & 0xFFFF] = (uint8_t)~v->sentinel_val;
    if (v->page) {
        g_ram[v->page & 0xFFFF] = (uint8_t)(v->page_map >> 8);
        g_ram[(v->page + 1) & 0xFFFF] = (uint8_t)v->page_map;
    }
    static volatile int ok;
    int stable = 0, have_prev = 0;
    static uint32_t prev[VDP_MAX_W * VDP_H];
    ok = 0;
    sandbox = 1;
    if (setjmp(sandbox_jmp)) { SDL_Log("minimap: the game stopped during the hidden frames"); goto restore; }
    for (int k = 0; k < 240 && !ok; k++) {
        sandbox_pad = k < 3 ? GB_START : 0;
        cpu_run_frame();
        drain_sound();
        if ((g_ram[sa] << 8 | g_ram[(sa + 1) & 0xFFFF]) != v->sentinel_val) continue;
        /* hide the icons: end the sprite list at an off-screen first entry */
        uint32_t sat = (uint32_t)(g_vreg[5] & 0x7F) << 9;
        for (int i = 0; i < 8; i++) g_vram[(sat + i) & 0xFFFF] = 0;
        static const vdp_render_opts plain = { 0 };
        vdp_render(mmfb, VDP_MAX_W, &plain);
        int rr[4];
        if (!mm_rect(&mmd, rr, vdp_display_width())) break;
        int lit = 0, same = have_prev && rr[2] == r[2] && rr[3] == r[3];
        for (int y = 0; y < rr[3]; y++)
            for (int x = 0; x < rr[2]; x++) {
                uint32_t c = mmfb[(rr[1] + y) * VDP_MAX_W + rr[0] + x];
                if ((c & 0xF0F0F0) != 0) lit++;
                if (same && prev[y * rr[2] + x] != c) same = 0;
                prev[y * rr[2] + x] = c;
            }
        /* the pause screen fades in: wait for the palette to hold too */
        static uint16_t prev_cram[64];
        if (memcmp(prev_cram, g_cram, sizeof prev_cram)) { same = 0; memcpy(prev_cram, g_cram, sizeof prev_cram); }
        memcpy(r, rr, sizeof rr);
        have_prev = 1;
        stable = same && lit > rr[2] * rr[3] / 4 ? stable + 1 : 0;
        if (stable >= 12) {
            for (int y = 0; y < r[3]; y++) memcpy(out + y * r[2], mmfb + (r[1] + y) * VDP_MAX_W + r[0], (size_t)r[2] * 4);
            ok = 1;
        }
    }
restore:
    sandbox = 0;
    sandbox_pad = 0;
    cpu_state_load(st);
    SDL_free(st);
    drain_sound();
    return ok;
}

static void mm_make_texture(const uint32_t *px, int w, int h)
{
    if (mm.tex) SDL_DestroyTexture(mm.tex);
    mm.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!mm.tex) return;
    SDL_UpdateTexture(mm.tex, 0, px, w * 4);
    SDL_SetTextureScaleMode(mm.tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(mm.tex, SDL_BLENDMODE_BLEND);
}

/* At the frame boundary, before running frames: refresh the data and get the
 * terrain for a newly seen level. */
static void mm_update(void)
{
    mmd_ok = 0;
    if (!game || !game->minimap || !game->mm_view || att_on || extracting || sandbox) return;
    if (!hud_ok || !game->minimap(&mmd)) return;
    mmd_ok = 1;                                  /* also feeds autosave and stats */
    if (!g_set.minimap) return;
    if (mm.game != game || mm.level != mmd.level) {
        if (mm.tex) SDL_DestroyTexture(mm.tex);
        memset(&mm, 0, sizeof mm);
        mm.game = game;
        mm.level = mmd.level;
    }
    if (mm.state == 1 || (mm.state == 2 && (mm.tries >= 3 || g_frame < mm.retry_frame))) return;
    int r[4];
    if (!mm_rect(&mmd, r, 320)) return;
    char dir[1100], path[1200];
    SDL_snprintf(dir, sizeof dir, "%sminimap", pref);
    SDL_snprintf(path, sizeof path, "%s/%s_%06X.png", dir, game->id, (unsigned)mmd.level);
    int w = 0, h = 0, comp;
    size_t flen = 0;
    void *file = SDL_LoadFile(path, &flen);
    unsigned char *img = file ? stbi_load_from_memory(file, (int)flen, &w, &h, &comp, 4) : NULL;
    SDL_free(file);
    if (img && w == r[2] && h == r[3]) {
        for (int i = 0; i < w * h; i++) {           /* RGBA bytes -> XRGB */
            unsigned char *p = img + i * 4;
            ((uint32_t *)img)[i] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
        mm_make_texture((uint32_t *)img, w, h);
        stbi_image_free(img);
        memcpy(mm.r, r, sizeof r);
        mm.state = 1;
        return;
    }
    if (img) stbi_image_free(img);
    static uint32_t px[VDP_MAX_W * VDP_H];
    uint64_t t0 = SDL_GetTicksNS();
    int ok = mm_capture(px, r);
    SDL_Log("minimap: %s level %06X terrain %s (%dx%d, %.0f ms)", game->id, (unsigned)mmd.level, ok ? "captured" : "not found",
            r[2], r[3], (SDL_GetTicksNS() - t0) / 1e6);
    if (!ok) { mm.state = 2; mm.tries++; mm.retry_frame = g_frame + 600; return; }
    mm_make_texture(px, r[2], r[3]);
    memcpy(mm.r, r, sizeof r);
    mm.state = 1;
    SDL_CreateDirectory(dir);
    static uint8_t rgb[VDP_MAX_W * VDP_H * 3];
    for (int i = 0; i < r[2] * r[3]; i++) {
        rgb[i * 3] = (uint8_t)(px[i] >> 16); rgb[i * 3 + 1] = (uint8_t)(px[i] >> 8); rgb[i * 3 + 2] = (uint8_t)px[i];
    }
    stbi_write_png(path, r[2], r[3], 3, rgb, r[2] * 3);
}

/* Bumpers / Q E: step through the pause map's list. */
static void mm_step(int dir)
{
    if (!g_set.minimap || !mmd_ok || !game->minimap_select || mmd.count <= 0) return;
    int i = (mmd.sel + dir + mmd.count) % mmd.count;
    game->minimap_select(i);
    mmd.sel = i;
    mm.name_until = SDL_GetTicksNS() + 2500000000ull;
}

static void mm_marker(float x, float y, float s, SDL_FColor c)   /* diamond with a dark edge */
{
    SDL_FColor k = ui_rgba(8, 10, 6, 230);
    float e = s + 1.4f;
    ui_triangle(x - e, y, x, y - e, x + e, y, k);
    ui_triangle(x - e, y, x, y + e, x + e, y, k);
    ui_triangle(x - s, y, x, y - s, x + s, y, c);
    ui_triangle(x - s, y, x, y + s, x + s, y, c);
}

static void draw_minimap(void)
{
    if (!g_set.minimap || !mmd_ok || !hud_ok || menu_is_open() || !game || !game->mm_view) return;
    const game_mm_view *v = game->mm_view;
    int r[4];
    if (mm.state == 1) memcpy(r, mm.r, sizeof r);
    else if (!mm_rect(&mmd, r, 320)) return;
    float x0, y0, x1, y1;
    ui_to_virtual(game_dst.x, game_dst.y, &x0, &y0);
    ui_to_virtual(game_dst.x + game_dst.w, game_dst.y + game_dst.h, &x1, &y1);
    float u = (y1 - y0) / 720.0f;
    if (u < 0.55f) u = 0.55f;
    ui_font f = u < 1.25f ? UI_SMALL : UI_BODY;
    static const float frac[3] = { 0.24f, 0.32f, 0.42f };   /* the old Small / Medium / Large */
    float mh = (y1 - y0) * (g_set.minimap_size <= 2 ? frac[g_set.minimap_size] : g_set.minimap_size / 100.0f);   /* map height (0-2: the old sizes, before migration) */
    float k = mh / r[3], mw = r[2] * k;
    float maxw = (x1 - x0) * 0.5f;
    if (mw > maxw) { k = maxw / r[2]; mw = maxw; mh = r[3] * k; }
    float pad = 6 * u, lh = ui_line_h(f) * 0.92f;
    float pw = mw + pad * 2, ph = mh + pad * 2 + lh;
    int right = g_set.minimap_pos == 1 || g_set.minimap_pos == 2, top = g_set.minimap_pos >= 2;
    float px = right ? x1 - 14 * u - pw : x0 + 14 * u;
    float py = top ? y0 + 12 * u : y1 - 12 * u - ph;
    if (hud_box_frame == g_frame && g_set.status_hud &&             /* the HUD is in this corner: stack below / above it */
        ((g_set.hud_pos & 1) != 0) == right && ((g_set.hud_pos & 2) == 0) == top)
        py = top ? hud_box[1] + hud_box[3] + 8 * u : hud_box[1] - 8 * u - ph;
    SDL_FColor edge = ui_rgba(196, 164, 98, 200), fill = ui_rgba(14, 17, 12, 170);
    ui_panel(px, py, pw, ph, 8 * u, fill, edge);
    float mx = px + pad, my = py + pad;
    if (mm.state == 1 && mm.tex) {
        SDL_SetTextureAlphaMod(mm.tex, 205);
        ui_image(mm.tex, mx, my, mw, mh);
    } else {
        ui_rect(mx, my, mw, mh, ui_rgba(30, 36, 24, 150));
    }
    ui_frame(mx, my, mw, mh, 1, ui_rgba(196, 164, 98, 110));
    /* world -> panel */
    int sh = mmd.shift ? mmd.shift : v->shift;
    #define MMX(wx) (mx + ((float)(v->origin_x + ((wx) >> sh) - r[0]) + 0.5f) * k)
    #define MMY(wy) (my + ((float)(v->origin_y + ((wy) >> sh) - r[1]) + 0.5f) * k)
    int blink = (SDL_GetTicks() / 250) & 1;
    float ms = SDL_max(2.2f * u, k * 1.6f);
    for (int i = 0; i < mmd.n; i++) {
        const game_mm_mark *m = &mmd.m[i];
        float ax = MMX(m->x), ay = MMY(m->y);
        if (ax < mx || ay < my || ax > mx + mw || ay > my + mh) continue;
        int sel = m->obj == mmd.sel;
        SDL_FColor c = m->main ? (sel ? ui_rgba(255, 214, 72, 255) : ui_rgba(220, 96, 52, 255))
                               : ui_rgba(120, 220, 230, 255);
        if (sel && blink) c.a = 0.35f;                      /* the pause map blinks the selection */
        mm_marker(ax, ay, sel ? ms * 1.25f : ms, c);
    }
    for (int i = 0; i < threats.n; i++) {               /* nearby threats: small red diamonds */
        float tx = MMX(threats.x[i]), ty = MMY(threats.y[i]), s = ms * 0.85f;
        if (tx < mx || ty < my || tx > mx + mw || ty > my + mh) continue;
        ui_triangle(tx - s - 1, ty, tx, ty - s - 1, tx + s + 1, ty, ui_rgba(8, 10, 6, 230));
        ui_triangle(tx - s - 1, ty, tx, ty + s + 1, tx + s + 1, ty, ui_rgba(8, 10, 6, 230));
        ui_triangle(tx - s, ty, tx, ty - s, tx + s, ty, ui_rgba(255, 60, 48, 255));
        ui_triangle(tx - s, ty, tx, ty + s, tx + s, ty, ui_rgba(255, 60, 48, 255));
    }
    if (mmd.have_home) {
        float hx = MMX(mmd.hx), hy = MMY(mmd.hy), s = ms * 0.9f;
        ui_rect(hx - s - 1, hy - s - 1, s * 2 + 2, s * 2 + 2, ui_rgba(8, 10, 6, 230));
        ui_rect(hx - s, hy - s, s * 2, s * 2, ui_rgba(90, 150, 230, 255));
    }
    if (mmd.have_player) {
        float ax = MMX(mmd.px), ay = MMY(mmd.py), s = ms * 1.1f;
        ui_rect(ax - s - 1.5f, ay - s - 1.5f, s * 2 + 3, s * 2 + 3, ui_rgba(8, 10, 6, 240));
        ui_rect(ax - s, ay - s, s * 2, s * 2, ui_rgba(250, 250, 240, 255));
    }
    #undef MMX
    #undef MMY
    /* selected entry */
    char name[64];
    int st = game->minimap_name ? game->minimap_name(mmd.sel, name, sizeof name) : -1;
    if (st >= 0) {
        int fresh = SDL_GetTicksNS() < mm.name_until;
        SDL_FColor c = fresh ? ui_rgba(255, 214, 72, 255) : ui_rgba(196, 164, 98, 255);
        char line[80];
        SDL_snprintf(line, sizeof line, "%s%s", name, st == 2 ? "  (?)" : "");
        float ah = lh * 0.28f, ay = my + mh + pad * 0.5f + lh * 0.5f;    /* step arrows */
        SDL_FColor ac = ui_rgba(196, 164, 98, 200);
        ui_triangle(mx + ah * 0.4f, ay, mx + ah * 1.6f, ay - ah, mx + ah * 1.6f, ay + ah, ac);
        ui_triangle(mx + mw - ah * 0.4f, ay, mx + mw - ah * 1.6f, ay - ah, mx + mw - ah * 1.6f, ay + ah, ac);
        float tw = ui_text_w(f, line);
        float tx = tw > mw - ah * 4 ? mx + ah * 2 : mx + (mw - tw) / 2;
        ui_text(f, tx, my + mh + pad * 0.5f, c, line);
    }
}

/* ---------------- status watcher: rumble, stats, autosave ----------------
 * After the frames of each display update, compare the player status (the
 * HUD values) and the objectives (the minimap data) with the last update. */
static struct {
    int ok;
    game_hud h;
    uint64_t last_frame;
    uint32_t level;                 /* level seen last */
    int level_frames;               /* frames flown on it */
    int level_save;                 /* mission-start autosave pending */
    int cnt[32], zero[32];          /* objective markers; frames at zero */
    uint64_t stats_saved;
} wt;

static void rumble(float lo, float hi, int ms)
{
    if (g_set.rumble <= 0 || menu_is_open()) return;
    /* Issue #5: short weak pulses barely spin up a pad's motors, so the
     * strengths follow a square-root curve and pulses last at least 70 ms. */
    float k = g_set.rumble / 100.0f;
    input_rumble(SDL_sqrtf(lo) * k, SDL_sqrtf(hi) * k, ms < 70 ? 70 : ms);
}

static void fire_rumble(int weapon)              /* 0 gun, 1 rockets, 2 missiles */
{
    static const float lo[3] = { 0.05f, 0.30f, 0.65f }, hi[3] = { 0.30f, 0.55f, 0.90f };
    static const int ms[3] = { 70, 110, 220 };
    if (weapon >= 0 && weapon < 3) rumble(lo[weapon], hi[weapon], ms[weapon]);
}

static void autosave(const char *why)
{
    if (!g_set.autosave || !game) return;
    char a[1024], b[1024];
    for (int k = MENU_SLOTS - 1; k > MENU_SAVE_SLOTS; k--) {   /* Auto 1 -> 2 -> 3 */
        state_path(a, sizeof a, k - 1);
        state_path(b, sizeof b, k);
        SDL_RemovePath(b);
        SDL_RenamePath(a, b);
        slot_known[k] = 0;
    }
    int ok = write_state(MENU_SAVE_SLOTS);
    char m[96];
    SDL_snprintf(m, sizeof m, ok ? "Autosaved: %s" : "Autosave failed (%s)", why);
    say(m);
}

static void watch_objectives(game_stats *s, int df)
{
    if (!mmd_ok) return;
    int n = mmd.count < 32 ? mmd.count : 32, cnt[32] = { 0 };
    for (int i = 0; i < mmd.n; i++) if (mmd.m[i].main && mmd.m[i].obj < 32) cnt[mmd.m[i].obj]++;
    if (mmd.level != wt.level) {                 /* a new level: a mission starts */
        wt.level = mmd.level;
        wt.level_frames = 0;
        wt.level_save = !watch_reset && !load_fresh;   /* not when it came from a loaded state */
        load_fresh = 0;
        if (wt.level_save) s->missions++;
        memcpy(wt.cnt, cnt, sizeof cnt);
        memset(wt.zero, 0, sizeof wt.zero);
    }
    if (mmd.campaign > s->best_campaign) s->best_campaign = mmd.campaign;
    wt.level_frames += df;
    if (wt.level_save && wt.level_frames >= 120) { wt.level_save = 0; autosave("mission start"); amiga_event(EV_MISSION_START); }
    for (int i = 0; i < n; i++) {
        if (cnt[i] > 0) { wt.cnt[i] = cnt[i]; wt.zero[i] = 0; continue; }
        if (wt.cnt[i] <= 0) continue;
        char name[64];
        if (game->minimap_name(i, name, sizeof name) != 0) { wt.cnt[i] = 0; continue; }
        if ((wt.zero[i] += df) < 45) continue;   /* gone for good, not a flicker */
        wt.cnt[i] = 0;
        s->objectives++;
        amiga_event(EV_OBJECTIVE);
        char why[96];
        SDL_snprintf(why, sizeof why, "%s complete", name);
        autosave(why);
    }
}

/* Rescue calls (Amiga extras): the people waiting to be picked up shout as
 * the helicopter comes near, as on the Amiga. They are the units of the
 * pause map's rescue objectives (MIAs, pilots, hostages, inspectors ...). */
/* Threats (Settings > Picture > Minimap Threats): enemy units of the pause
 * map's enemy entries (AAA, Rapier, VDA ...) within reach of the helicopter,
 * drawn on the minimap. Pick-ups, rescues, landing zones and the numbered
 * objectives are not threats. */

static void watch_threats(void)
{
    static uint32_t level;
    static signed char is_enemy[32];
    static game_minimap units;
    if (g_frame % 6) return;
    threats.n = 0;
    if (!g_set.minimap || !g_set.minimap_threats || !mmd_ok || !mmd.have_player || !game->minimap_name || !game->minimap_units) return;
    if (mmd.level != level) { level = mmd.level; memset(is_enemy, -1, sizeof is_enemy); }
    static const char *const not_enemy[] = { "FUEL", "AMMO", "ARMOR", "ARMOUR", "REPAIR", "LANDING", "ZONE", "MIA", "PILOT", "HOSTAGE",
                                            "PRISONER", "RESCUE", "CAPTIVE", "INSPECT", "SCIENTIST", "AGENT", "CREW", "BASE", "FRIGATE", "SHIP" };
    for (int o = 0; o < mmd.count && o < 32; o++) {
        if (is_enemy[o] < 0) {
            char name[64];
            if (game->minimap_name(o, name, sizeof name) != 0) continue;
            is_enemy[o] = !(name[0] >= '0' && name[0] <= '9');               /* "1.RADAR SITES": an objective */
            for (size_t w = 0; w < sizeof not_enemy / sizeof not_enemy[0]; w++) if (SDL_strstr(name, not_enemy[w])) is_enemy[o] = 0;
        }
        if (is_enemy[o] != 1 || !game->minimap_units(o, &units)) continue;
        for (int i = 0; i < units.n && threats.n < THREAT_MAX; i++) {
            long dx = units.m[i].x - mmd.px, dy = units.m[i].y - mmd.py;
            long r = SDL_getenv("DS_THREAT_R") ? SDL_atoi(SDL_getenv("DS_THREAT_R")) : 420;   /* about a screen and a half (env: tests) */
            if (dx * dx + dy * dy > r * r) continue;
            threats.x[threats.n] = units.m[i].x; threats.y[threats.n] = units.m[i].y; threats.n++;
        }
    }
}

static void watch_rescue(void)
{
    static uint32_t level;
    static signed char is_rescue[32];
    static game_minimap units;
    if (!mmd_ok || !mmd.have_player || !g_set.amiga_extras || !game->minimap_name || !game->minimap_units || g_frame % 10) return;
    if (mmd.level != level) { level = mmd.level; memset(is_rescue, -1, sizeof is_rescue); }
    static const char *const words[] = { "MIA", "PILOT", "HOSTAGE", "PRISONER", "RESCUE", "CAPTIVE", "INSPECT", "SCIENTIST", "AGENT", "CREW" };
    long best = -1;
    for (int o = 0; o < mmd.count && o < 32; o++) {
        if (is_rescue[o] < 0) {
            char name[64];
            if (game->minimap_name(o, name, sizeof name) != 0) continue;    /* not available yet: ask again later */
            is_rescue[o] = 0;
            for (size_t w = 0; w < sizeof words / sizeof words[0]; w++) if (SDL_strstr(name, words[w])) is_rescue[o] = 1;
            if (SDL_getenv("DS_SFX_DEBUG")) SDL_Log("list %d \"%s\" rescue=%d", o, name, is_rescue[o]);
        }
        if (is_rescue[o] != 1 || !game->minimap_units(o, &units)) continue;
        for (int i = 0; i < units.n; i++) {
            long dx = units.m[i].x - mmd.px, dy = units.m[i].y - mmd.py, d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < best) best = d2;
        }
    }
    if (best < 0) return;
    if (SDL_getenv("DS_SFX_DEBUG") && g_frame % 300 == 0) SDL_Log("nearest rescue %.0f px", SDL_sqrt((double)best));
    long nearr = 90, farr = SDL_getenv("DS_RESCUE_FAR") ? SDL_atoi(SDL_getenv("DS_RESCUE_FAR")) : 220;   /* env: tests */
    if (best < nearr * nearr) amiga_event(EV_RESCUE_NEAR);
    else if (best < farr * farr) amiga_event(EV_RESCUE_FAR);
}

static void watch_frame(void)
{
    if (!game || att_on || extracting || sandbox || !game->hud) return;
    game_hud h;
    if (!hud_ok || !game->hud(&h)) { wt.ok = 0; return; }
    resume_worthy = 1;
    game_stats *s = stats_for(game->id);
    int df = (int)(g_frame - wt.last_frame);
    wt.last_frame = g_frame;
    if (watch_reset || !wt.ok || df <= 0 || df > 30) {
        wt.h = h;
        wt.ok = 1;
        if (watch_reset && mmd_ok) {             /* baseline: no autosave for this level */
            load_fresh = 0;
            wt.level = mmd.level;
            wt.level_save = 0;
            for (int i = 0; i < 32; i++) wt.cnt[i] = wt.zero[i] = 0;
            for (int i = 0; i < mmd.n; i++) if (mmd.m[i].main && mmd.m[i].obj < 32) wt.cnt[mmd.m[i].obj]++;
        }
        watch_reset = 0;
        return;
    }
    s->play_seconds += df / 60.0;
    if (h.lives < wt.h.lives) {
        s->lives_lost++;
        if (SDL_getenv("DS_DMG_LOG")) SDL_Log("life lost frame %llu", (unsigned long long)g_frame);
        rumble(1.0f, 1.0f, 700);
    } else {
        int dmg = wt.h.armor - h.armor;
        if (dmg > 0 && dmg < 5000) {
            s->damage += dmg;
            if (SDL_getenv("DS_DMG_LOG")) SDL_Log("dmg %d armor %d/%d frame %llu", dmg, h.armor, h.armor_max, (unsigned long long)g_frame);
            float k = (float)dmg / (h.armor_max > 0 ? h.armor_max : 600);
            rumble(0.35f + k * 4, 0.25f + k * 4, 140 + (int)(k * 800));
        }
    }
    for (int i = 0; i < 3; i++) {
        int d = wt.h.ammo[i] - h.ammo[i];
        if (wt.h.ammo[i] < 0 || h.ammo[i] < 0 || d <= 0 || d > 50) continue;
        s->shots[i] += d;
        if (SDL_strcmp(game->id, "desert")) fire_rumble(i);   /* Desert: from the weapon sound (works with unlimited ammo) */
    }
    if (h.fuel_max > 0) {                        /* low-fuel warning: once per dip, re-armed by refuelling */
        int lo = h.fuel_max / 4;
        if (h.fuel <= lo && wt.h.fuel > lo && h.fuel > 0 && h.lives == wt.h.lives) amiga_event(EV_LOW_FUEL);
    }
    if (h.lives < wt.h.lives) amiga_event(EV_LIFE_LOST);
    if (h.load > wt.h.load) s->picked_up += h.load - wt.h.load;
    else if (h.load < wt.h.load && h.lives == wt.h.lives) s->delivered += wt.h.load - h.load;
    wt.h = h;
    watch_objectives(s, df);
    watch_rescue();
    watch_threats();
    if (g_frame - wt.stats_saved > 60 * 60) { wt.stats_saved = g_frame; stats_save(stats_path); }
}

/* Stats page lines ("label\tvalue"). */
static void fmt_time(char *b, size_t n, double sec)
{
    int m = (int)(sec / 60);
    if (m >= 60) SDL_snprintf(b, n, "%dh %02dm", m / 60, m % 60);
    else SDL_snprintf(b, n, "%dm %02ds", m, (int)sec % 60);
}
static int stats_lines(char (*l)[96], int max)
{
    if (!game || max < 10) return 0;
    const game_stats *s = stats_for(game->id);
    char t[32];
    int k = 0;
    fmt_time(t, sizeof t, s->play_seconds);
    SDL_snprintf(l[k++], 96, "Time flying\t%s", t);
    SDL_snprintf(l[k++], 96, "Missions started\t%d", s->missions);
    SDL_snprintf(l[k++], 96, "Furthest campaign\t%d", s->best_campaign);
    SDL_snprintf(l[k++], 96, "Objectives completed\t%d", s->objectives);
    SDL_snprintf(l[k++], 96, "Rescued / unloaded\t%d", s->delivered);
    SDL_snprintf(l[k++], 96, "Winched aboard\t%d", s->picked_up);
    SDL_snprintf(l[k++], 96, "Gun rounds fired\t%d", s->shots[0]);
    SDL_snprintf(l[k++], 96, "Hydras / Hellfires fired\t%d / %d", s->shots[1], s->shots[2]);
    SDL_snprintf(l[k++], 96, "Armor lost\t%d", s->damage);
    SDL_snprintf(l[k++], 96, "Lives lost\t%d", s->lives_lost);
    return k;
}

/* ---------------- rewind ---------------- */
static int rewinding;
static uint8_t *rw_state;

static void rewind_record(void)                  /* after each frame: every 2nd frame */
{
    if (!g_set.rewind || !rw_state || g_frame & 1) return;
    cpu_state_save(rw_state);
    rw_push(rw_state);
}

static int rewind_step(void)                     /* one step back; 0 at the start of the history */
{
    if (!rw_state || !rw_pop(rw_state) || !cpu_state_load(rw_state)) return 0;
    drain_sound();
    on_vblank();                                 /* picture of the restored moment */
    return 1;
}

static void draw_game(int ow, int oh)
{
    int dispw = vdp_display_width();
    double aspect;
    switch (g_set.aspect) {
    case ASPECT_SQUARE:  aspect = (double)fb_w / VDP_H; break;
    case ASPECT_STRETCH: aspect = (double)ow / oh; break;
    default:             aspect = (4.0 / 3.0) * fb_w / (double)dispw; break;   /* TV shape */
    }
    double dw = ow, dh = ow / aspect;
    if (dh > oh) { dh = oh; dw = oh * aspect; }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    /* CRT filter: from the 1x picture (the tube makes its own detail), over
     * the whole area (it resamples the picture, so no integer scaling) */
    static int crt_said;
    if (g_set.crt) {
        SDL_FRect area = { (float)SDL_floor((ow - dw) / 2), (float)SDL_floor((oh - dh) / 2), (float)dw, (float)dh };
        int console = dispw <= 256 ? 3 : 2;   /* Lola Retro: Mega Drive H32 / H40 */
        crt_look look = { g_set.crt, g_set.crt_flat, g_set.crt_glow, g_set.crt_wires, g_set.crt_antimoire, g_set.crt_dither, g_set.crt_mask };
        if (crt_draw(&look, fb, VDP_MAX_W, fb_w, VDP_H, console, area, g_frame, fb_picture)) {
            game_dst = area;
            crt_said = 0;
            return;
        }
        if (!crt_said) { say(crt_why_not()); crt_said = 1; }
    } else crt_said = 0;

    int S = 1;
    SDL_Texture *src_tex = tex;
    if (hd_on && hdtex) {
        S = hd.scale;
        src_tex = hdtex;
        SDL_UpdateTexture(hdtex, &(SDL_Rect){ 0, 0, fb_w * S, VDP_H * S }, hdfb, VDP_MAX_W * 4 * 4);
    } else {
        SDL_UpdateTexture(tex, &(SDL_Rect){ 0, 0, fb_w, VDP_H }, fb, VDP_MAX_W * 4);
    }
    if (g_set.scaling == SCALE_INTEGER && oh >= VDP_H) {
        int k = (int)(dh / VDP_H);
        if (k < 1) k = 1;
        dh = k * VDP_H;
        dw = g_set.aspect == ASPECT_SQUARE ? k * fb_w : dh * aspect;
        if (dw > ow) dw = ow;
    }
    SDL_FRect dst = { (float)SDL_floor((ow - dw) / 2), (float)SDL_floor((oh - dh) / 2), (float)dw, (float)dh };
    game_dst = dst;
    SDL_FRect src = { 0, 0, (float)(fb_w * S), (float)(VDP_H * S) };
    if (g_set.scaling == SCALE_SHARP && S > 1 && dh <= VDP_H * S) {
        /* HD art already has the detail: just filter it down */
        SDL_SetTextureScaleMode(src_tex, SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(ren, src_tex, &src, &dst);
    } else if (g_set.scaling == SCALE_SHARP) {
        /* nearest-neighbour to an integer multiple, then smooth down to size */
        int k = (int)(dh / (VDP_H * S) + 0.999);
        if (k < 1) k = 1;
        if (k > 6) k = 6;
        static int up_w, up_h, no_target;
        int uw = fb_w * S * k, uh = VDP_H * S * k;
        if (!no_target && (!upscaled || up_w != uw || up_h != uh)) {
            if (upscaled) SDL_DestroyTexture(upscaled);
            upscaled = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, uw, uh);
            if (upscaled) { SDL_SetTextureScaleMode(upscaled, SDL_SCALEMODE_LINEAR); up_w = uw; up_h = uh; }
            else no_target = 1;
        }
        SDL_SetTextureScaleMode(src_tex, SDL_SCALEMODE_NEAREST);
        if (upscaled) {
            SDL_SetRenderTarget(ren, upscaled);
            SDL_RenderTexture(ren, src_tex, &src, 0);
            SDL_SetRenderTarget(ren, 0);
            SDL_RenderTexture(ren, upscaled, 0, &dst);
        } else {
            SDL_RenderTexture(ren, src_tex, &src, &dst);
        }
    } else {
        SDL_SetTextureScaleMode(src_tex, g_set.scaling == SCALE_SMOOTH || S > 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_RenderTexture(ren, src_tex, &src, &dst);
    }

    if (g_set.scanlines) {
        static const float alpha[SCAN_COUNT] = { 0, 0.18f, 0.32f, 0.5f };
        static SDL_FRect lines[VDP_H];
        float lh = (float)(dh / VDP_H);
        for (int i = 0; i < VDP_H; i++) lines[i] = (SDL_FRect){ dst.x, dst.y + i * lh + lh * 0.5f, dst.w, lh * 0.5f };
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColorFloat(ren, 0, 0, 0, alpha[g_set.scanlines]);
        SDL_RenderFillRects(ren, lines, VDP_H);
    }
}

/* Lines of text in a box in the upper right corner (Show FPS). */
#define OSD_LINES 16
static struct { char text[OSD_LINES][192]; SDL_FColor col[OSD_LINES]; int n; } osd_box;

static void box_line(SDL_FColor c, const char *fmt, ...)
{
    if (osd_box.n >= OSD_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(osd_box.text[osd_box.n], sizeof osd_box.text[0], fmt, ap);
    va_end(ap);
    osd_box.col[osd_box.n++] = c;
}

static void box_draw(void)
{
    float lh = ui_line_h(UI_SMALL), w = 0;
    for (int i = 0; i < osd_box.n; i++) { float t = ui_text_w(UI_SMALL, osd_box.text[i]); if (t > w) w = t; }
    float x = 1260 - w - 16 > 0 ? 1260 - w - 16 : 0;   /* a line wider than the screen: the box from the left edge */
    if (osd_box.n) ui_rect(x, 10, 1260 - x, lh * osd_box.n + 12, ui_rgba(0, 0, 0, 190));
    for (int i = 0; i < osd_box.n; i++) ui_text_right(UI_SMALL, 1252, 16 + lh * i, osd_box.col[i], osd_box.text[i]);
    osd_box.n = 0;
}

/* Show FPS: Diagnostics. What the display really does, with hints in plain
 * words: the questions a tearing or judder report needs answered. Each check
 * colours its line and adds its hint below the measures. */
static void draw_diagnostics(void)
{
    static const char *const wm[] = { "windowed", "borderless fullscreen", "exclusive fullscreen" };
    const SDL_FColor ok = ui_rgba(160, 230, 140, 255), warn = ui_rgba(250, 200, 80, 255), bad = ui_rgba(250, 110, 90, 255);
    const SDL_DisplayMode *dm = display_mode();
    float hz = dm->refresh_rate;
    int ww = 0, wh = 0, vs = -9;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    SDL_GetRenderVSync(ren, &vs);
    int odd_hz = hz > 0.0f && !near_60(hz);
    int comp = compositing > 0 && g_set.window_mode != WIN_WINDOWED;
    int early = g_set.vsync && pstat.early > 2, late = pstat.late > 1;
    int audio = astat.clears || astat.starved > 2, busy = gstat.load > 90;

    box_line(ok, "Game %.1f fps   Display %.1f fps", fps_game, fps_disp);
    box_line(odd_hz ? warn : ok, "%s / %s   screen %dx%d @ %.2f Hz   window %dx%d, %s",
             SDL_GetCurrentVideoDriver(), SDL_GetRendererName(ren), dm->w, dm->h, hz, ww, wh,
             wm[g_set.window_mode < 3 ? g_set.window_mode : 0]);
    box_line(comp ? warn : ok, "VSync %s (driver: %s)   Pacing: %s   Compositor: %s", g_set.vsync ? "on" : "off",
             vs == 1 ? "on" : vs == 0 ? "off" : "?", sync_on ? "one game frame per refresh" : "game clock (59.92 Hz)",
             compositing > 0 ? "running" : compositing < 0 ? "unknown" : "none");
    box_line(late || early ? bad : ok, "Frame time %.1f ms avg, %.1f max   late %d/s   early %d/s",
             pstat.n ? pstat.sum_ms / pstat.n : 0.0, pstat.max_ms, pstat.late, pstat.early);
    box_line(audio ? bad : ok, "Audio queue %.0f-%.0f ms   rate x%.4f   cut %d/s   starved %d/s",
             astat.min_ms, astat.max_ms, astat.ratio, astat.clears, astat.starved);
    if (gstat.source)
        box_line(busy ? bad : gstat.load > 70 ? warn : ok, "Graphics card: load %d%%   %d C   %d W   fan %d%%   %d MHz",
                 gstat.load, gstat.temp, gstat.power, gstat.fan, gstat.clock);
    diag_overlay_extra(ok, warn, bad);

    if (odd_hz) box_line(warn, "The screen runs at %.0f Hz: the game is made for 60. Pick a 60 Hz mode.", hz);
    if (comp) box_line(warn, "A compositor is running: it can tear the fullscreen picture. Stop it while playing.");
    if (early) box_line(warn, "Frames don't wait for the screen: tearing likely (a compositor, or the driver ignores VSync).");
    if (late) box_line(warn, "Frames miss the screen's refresh: the computer or the graphics card is too slow for these settings.");
    if (!g_set.vsync) box_line(warn, "VSync is off: turn it on (Display) for smooth, tear-free motion.");
    if (busy) box_line(warn, "The graphics card is nearly always busy: lighter settings keep it cooler and quieter.");
    box_line(ui_rgba(150, 150, 150, 255), "Log: %sdiagnostics.log", pref);
    box_draw();
}

static void draw_sound_lab(void);
static void present(void)
{
    int ow, oh;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    if (ow != out_w || oh != out_h) {
        out_w = ow; out_h = oh;
        if (g_set.widescreen == WS_AUTO) apply_ws();
    }
    draw_game(ow, oh);
    scene_step();
    draw_scene_swap();
    ui_begin(ow, oh);
    draw_hud();
    draw_minimap();
    draw_sound_lab();
    if (rewinding) {
        int blink = (SDL_GetTicks() / 400) & 1;
        float w = ui_text_w(UI_BODY, "REWIND") + 70;
        ui_panel(640 - w / 2, 18, w, 44, 8, ui_rgba(14, 17, 12, 190), ui_rgba(196, 164, 98, 200));
        SDL_FColor c = ui_rgba(242, 178, 58, blink ? 255 : 170);
        float tx = 640 - w / 2 + 16;
        ui_triangle(tx + 12, 30, tx, 40, tx + 12, 50, c);
        ui_triangle(tx + 24, 30, tx + 12, 40, tx + 24, 50, c);
        ui_text(UI_BODY, tx + 34, 24, c, "REWIND");
    }
    if (menu_is_open()) menu_draw(ow, oh);
    ui_begin(ow, oh);
    if (osd[0] && SDL_GetTicksNS() < osd_until) {
        ui_rect(24, 22, ui_text_w(UI_BODY, osd) + 28, 40, ui_rgba(10, 12, 8, 200));
        ui_text(UI_BODY, 38, 28, ui_rgba(242, 178, 58, 255), osd);
    }
    if (g_set.show_fps == 2) draw_diagnostics();
    else if (g_set.show_fps) {
        box_line(ui_rgba(160, 230, 140, 255), "%s  Game %.0f  Display %.0f", SDL_GetRendererName(ren), fps_game, fps_disp);
        box_draw();
    }
}

static void save_bmp(const char *dir)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/frame_%06llu.bmp", dir, (unsigned long long)g_frame);
    SDL_Surface *s = hd_on
        ? SDL_CreateSurfaceFrom(fb_w * hd.scale, VDP_H * hd.scale, SDL_PIXELFORMAT_XRGB8888, hdfb, VDP_MAX_W * 4 * 4)
        : SDL_CreateSurfaceFrom(fb_w, VDP_H, SDL_PIXELFORMAT_XRGB8888, fb, VDP_MAX_W * 4);
    if (s) { SDL_SaveBMP(s, path); SDL_DestroySurface(s); }
}

static void save_screen(const char *path)
{
    SDL_Surface *s = SDL_RenderReadPixels(ren, 0);
    if (s) { SDL_SaveBMP(s, path); SDL_DestroySurface(s); }
}

/* ---------------- Amiga sound effects (docs/13-amiga-ultimate.md) ----------------
 * The player's Amiga disks are read from <settings>/amiga/. amiga_sfx.ini
 * maps a game's effect ids to samples ("desert 2 a0042DD2A55BAF1A8 11025");
 * the Sound Lab overlay edits it while playing. */
#define MAX_MAP 128
static struct { char game[12]; int id; char sample[20]; int rate; int def; } sfxmap[MAX_MAP];
/* Defaults (sample ids are content hashes, no Amiga data): from the effect
 * bank, identified with speech and sound recognisers (Codex pass,
 * docs/13-amiga-ultimate.md). "genesis" in amiga_sfx.ini turns one off. */
static const struct { const char *game; int id; const char *sample; int rate; } sfx_defaults[] = {
    { "desert", 0, "aC390A9BD0DAA74B1", 17052 },          /* chain gun: fx 1, short pop */
    { "desert", 2, "a8B30C7E4E37AC8D6", 7989 },           /* Hydra launch: fx 4 */
    { "desert", 14, "a8B30C7E4E37AC8D6", 7989 },          /* Hellfire launch: fx 4 */
    { "desert", 31, "a55B40A7AF4AD31FD", 2419 },          /* Hydra hit: fx 19, short explosion */
    /* enemy weapons (the weapon table at $095F86, 30-byte entries: +$12
     * the fire sound, +2 the projectile): 29 = the guns (entries 14-17, the
     * chain gun's bullet), 13 = the missile sites and launchers (entries
     * 8-10, 13; the big missile), 30 = the rocket launchers (11, 12) */
    { "desert", 29, "aC390A9BD0DAA74B1", 17052 },         /* enemy guns: fx 1, as the chain gun */
    { "desert", 13, "a8B30C7E4E37AC8D6", 7989 },          /* enemy missile launch: fx 4 */
    { "desert", 30, "a8B30C7E4E37AC8D6", 7989 },          /* enemy rocket launch: fx 4 */
    { "desert", 1100, "a55B40A7AF4AD31FD", 2419 },        /* AA guns and tanks, silent on the Genesis: fx 19, as the Amiga */
    { "desert", 10, "a7463DDEE03CB1AF6", 8465 },          /* Hellfire hit: fx 3, explosion with rumble */
    { "desert", 12, "a3EBB4F38B729CA4B", 4144 },          /* taking hits: fx 20, sharp bang */
    { "desert", 17, "aB7023AA2CCC5D007", 2013 },          /* destroyed: fx 16, long explosion */
    { "desert", 25, "aB7023AA2CCC5D007", 2013 },
    { "desert", 1001, "aD4ACBDEFDE9A78D0", 7389 },        /* objective (a numbered mission) done: fx 26 "Mission complete" */
    { "desert", 1003, "a088F2E181897E676", 7389 },        /* low fuel: fx 25 "Warning" */
    { "desert", 1005, "a3A2622894A743146", 8867 },        /* rescue call, far: fx 7 "Help!" */
    { "desert", 1006, "aE438223EA486634C", 8475 },        /* rescue call, near: fx 6 "Over here!" */
    { "desert", SFX_ROTOR, "aCF1D882ECEBF7CCE", 7389 },   /* rotor: fx 2, looped while the Genesis rotor (FM channel 5) plays */
};
static int nsfxmap;
static char amiga_dir[1100], sfxmap_path[1100];
static int lab_last = -1, lab_hist[6], lab_nhist;
static uint64_t lab_flash;

/* Desert's effect ids, as identified from play logs (sfx_logger in ds_test). */
static const char *sfx_name(int id)
{
    switch (id) {
    case EV_OBJECTIVE: return "objective done +";
    case EV_MISSION_START: return "mission start +";
    case EV_LOW_FUEL: return "low fuel +";
    case EV_LIFE_LOST: return "life lost +";
    case EV_RESCUE_FAR: return "rescue call far +";
    case EV_RESCUE_NEAR: return "rescue call near +";
    case SFX_ROTOR: return "rotor loop";
    default: break;
    }
    if (!game || SDL_strcmp(game->id, "desert")) return "";
    switch (id) {
    case 0: return "chain gun"; case 2: return "Hydra launch"; case 31: return "Hydra hit";
    case 14: return "Hellfire launch"; case 10: return "Hellfire hit"; case 22: return "out of ammo";
    case 12: case 13: return "taking hits"; case 17: case 25: return "helicopter destroyed";
    case 18: return "low armor"; case 5: return "take-off"; case 6: return "rotor start";
    case 21: case 29: return "enemy fire";
    default: return "";
    }
}

static int sfxmap_find(int id)
{
    for (int i = 0; i < nsfxmap; i++) if (sfxmap[i].id == id && game && !SDL_strcmp(sfxmap[i].game, game->id)) return i;
    return -1;
}

static void sfxmap_save(void)
{
    FILE *f = fopen(sfxmap_path, "w");
    if (!f) return;
    fprintf(f, "# Amiga sound effects: <game> <effect id> <sample> <rate Hz>. Edit with the Sound Lab.\n");
    for (int i = 0; i < nsfxmap; i++)
        if (!sfxmap[i].def) fprintf(f, "%s %d %s %d\n", sfxmap[i].game, sfxmap[i].id, sfxmap[i].sample, sfxmap[i].rate);
    fclose(f);
}

static void sfxmap_load(void)
{
    nsfxmap = 0;
    FILE *f = fopen(sfxmap_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && nsfxmap < MAX_MAP) {
        if (line[0] == '#') continue;
        char g[12], smp[20];
        int id, rate;
        if (SDL_sscanf(line, "%11s %d %19s %d", g, &id, smp, &rate) != 4) continue;
        SDL_strlcpy(sfxmap[nsfxmap].game, g, sizeof sfxmap[nsfxmap].game);
        sfxmap[nsfxmap].id = id;
        SDL_strlcpy(sfxmap[nsfxmap].sample, smp, sizeof sfxmap[nsfxmap].sample);
        sfxmap[nsfxmap].rate = rate;
        sfxmap[nsfxmap].def = 0;
        nsfxmap++;
    }
    fclose(f);
}

static void sfxmap_add_defaults(void)
{
    for (size_t k = 0; k < sizeof sfx_defaults / sizeof sfx_defaults[0] && nsfxmap < MAX_MAP; k++) {
        int have = 0;
        for (int i = 0; i < nsfxmap; i++) have |= sfxmap[i].id == sfx_defaults[k].id && !SDL_strcmp(sfxmap[i].game, sfx_defaults[k].game);
        if (have) continue;
        SDL_strlcpy(sfxmap[nsfxmap].game, sfx_defaults[k].game, sizeof sfxmap[nsfxmap].game);
        sfxmap[nsfxmap].id = sfx_defaults[k].id;
        SDL_strlcpy(sfxmap[nsfxmap].sample, sfx_defaults[k].sample, sizeof sfxmap[nsfxmap].sample);
        sfxmap[nsfxmap].rate = sfx_defaults[k].rate;
        sfxmap[nsfxmap].def = 1;
        nsfxmap++;
    }
}

static void play_mapped(int id)
{
    int m = sfxmap_find(id);
    if (m < 0) return;
    const amiga_sample *a = amiga_sample_at(amiga_sample_find(sfxmap[m].sample));
    if (a) sfx_play(id, a->pcm, a->len, sfxmap[m].rate, g_set.amiga_vol / 100.0f * 0.6f);
}

/* ---------------- Difficulty (docs/14-difficulty.md) ----------------
 * Every hit goes through $00A70C with the damage in d0 and the weapon entry
 * in a4 (patch at $00A6F6). On the player, Remastered and Recruit scale it
 * by weapon type (the bullet spray much less than missiles and rockets) and
 * cap the armour lost in any second, so a crossfire wears you down in a few
 * seconds instead of one. Projectiles, fire rates and enemy numbers are
 * untouched. Original deals the Genesis damage. */
static const struct { float bullet, missile, cap; } difficulty_tab[2] = {
    { 0.35f, 0.60f, 0.15f },                         /* Recruit */
    { 0.55f, 0.80f, 0.25f },                         /* Remastered */
};
static struct { uint64_t frame[64]; int dmg[64]; int n, pos; } dmg_win;

static int host_damage_hook(uint32_t target, uint32_t weapon, int dmg)
{
    if (!game || SDL_strcmp(game->id, "desert") || g_set.difficulty >= 2 || dmg <= 0 || extracting || sandbox) return dmg;
    uint32_t player = (uint32_t)R32(0xFF251C) & 0xFFFFFF;
    if ((target & 0xFFFFFF) != player) return dmg;
    int bullet = weapon >= 0x095F86 && weapon < 0x095F86 + 18 * 0x1E && (weapon - 0x095F86) % 0x1E == 0 &&
                 (g_rom[weapon + 2] << 8 | g_rom[weapon + 3]) == 0x25DA;        /* the bullet projectile */
    const float *t = bullet ? &difficulty_tab[g_set.difficulty].bullet : &difficulty_tab[g_set.difficulty].missile;
    int d = (int)(dmg * *t + 0.5f);
    if (d < 1) d = 1;
    /* armour lost in the last second (60 frames) */
    int maxa = R16(0xFF1040), cap = (int)(maxa * difficulty_tab[g_set.difficulty].cap), recent = 0;
    for (int i = 0; i < dmg_win.n; i++)
        if (g_frame >= dmg_win.frame[i] && g_frame - dmg_win.frame[i] < 60) recent += dmg_win.dmg[i];
    if (cap > 0 && recent + d > cap) d = SDL_max(0, cap - recent);
    if (d > 0) {
        int k = dmg_win.pos; dmg_win.pos = (dmg_win.pos + 1) % 64; if (dmg_win.n < 64) dmg_win.n++;
        dmg_win.frame[k] = g_frame; dmg_win.dmg[k] = d;
    }
    if (SDL_getenv("DS_DMG_LOG")) SDL_Log("hit %d -> %d (%s, last second %d of %d) frame %llu", dmg, d, bullet ? "bullet" : "missile", recent, cap, (unsigned long long)g_frame);
    return d;
}

/* Weapon fire as the Amiga plays it. The weapon fire sound ($00D684) comes
 * from the weapon entry in a4 (the Genesis table at $095F86, 30 bytes an
 * entry); the Amiga's campaign files (disk 3) hold the same table, 44 bytes
 * an entry, with the effect at +$20 and the pitch at +$14: 0 the effect's
 * low period, 1 its high one, -1 a random one between. So the Hydra and the
 * Hellfire are the same sample (fx 4), the Hydra at the lower pitch; the missile sites
 * fire it at the Hydra pitch too; the enemy guns use four different effects. Applied while
 * the sound id's mapping is the default one. */
#define WEAPON_TABLE 0x095F86
static const struct { signed char fx, pitch; } amiga_weapons[18] = {
    { 1, -1 }, { 4, 1 }, { 4, 0 },                  /* chain gun, Hydra, Hellfire */
    { 19, -1 }, { 19, -1 }, { 19, -1 }, { 19, -1 }, { 19, -1 },   /* silent on the Genesis */
    { 4, 1 }, { 4, 1 }, { 4, 1 },                   /* missile sites */
    { 4, -1 }, { 4, 1 },                            /* rocket launchers */
    { 4, 1 },                                       /* missile launcher */
    { 12, -1 }, { 13, -1 }, { 14, -1 }, { 19, -1 }, /* enemy guns */
};

static int play_amiga_weapon(int id, int m)
{
    if (!sfxmap[m].def || (id != 0 && id != 2 && id != 14 && id != 13 && id != 29 && id != 30 && id != 1100)) return 0;
    uint32_t a4 = R.a[4];
    if (a4 < WEAPON_TABLE || a4 >= WEAPON_TABLE + 18 * 0x1E || (a4 - WEAPON_TABLE) % 0x1E) return 0;
    int k = (int)(a4 - WEAPON_TABLE) / 0x1E;
    char label[8];
    SDL_snprintf(label, sizeof label, "fx %d", amiga_weapons[k].fx);
    const amiga_sample *a = NULL;
    for (int i = 0; i < amiga_sample_count(); i++)
        if (!SDL_strcmp(amiga_sample_at(i)->label, label)) { a = amiga_sample_at(i); break; }
    if (!a || a->per_lo <= 0 || a->per_hi <= 0) return 0;
    int lo = SDL_min(a->per_lo, a->per_hi), hi = SDL_max(a->per_lo, a->per_hi);
    int per = amiga_weapons[k].pitch == 0 ? a->per_lo : amiga_weapons[k].pitch == 1 ? a->per_hi : lo + SDL_rand(hi - lo + 1);
    int rate = (int)(3546895.0f / per);
    if (rate > 28000) rate = 28000;
    sfx_play(id, a->pcm, a->len, rate, g_set.amiga_vol / 100.0f * 0.6f);
    if (SDL_getenv("DS_SFX_DEBUG")) SDL_Log("sfx %d (weapon %d) -> Amiga %s at %d Hz (frame %llu)", id, k, label, rate, (unsigned long long)g_frame);
    return 1;
}

/* Called by the game's sound-effect request (hints.json patch). 1 = replaced. */
static void lab_note(int id)
{
    if (!g_sound_lab) return;
    lab_last = id;
    if (!lab_nhist || lab_hist[0] != id) {
        memmove(lab_hist + 1, lab_hist, sizeof lab_hist - sizeof lab_hist[0]);
        lab_hist[0] = id;
        if (lab_nhist < 6) lab_nhist++;
    }
}

static int host_sfx_hook(int id)
{
    if (!game || sandbox || extracting || att_on) return 0;
    lab_note(id);
    if (!SDL_strcmp(game->id, "desert") && (id == 0 || id == 2 || id == 14) && R.a[4] >= 0x095F86 && R.a[4] < 0x095F86 + 3 * 0x1E)
        fire_rumble((int)(R.a[4] - 0x095F86) / 0x1E);   /* the player's own weapons: chain gun, Hydra, Hellfire */
    if (SDL_getenv("DS_SFX_CALLER")) SDL_Log("sfx req %d from %06X a0=%06X a4=%06X (frame %llu)", id, R32(R.a[7] + 56), R.a[0], R.a[4], (unsigned long long)g_frame);
    if (!g_set.amiga_sfx || !amiga_sample_count()) return 0;
    int m = sfxmap_find(id);
    if (m < 0 || amiga_sample_find(sfxmap[m].sample) < 0) return 0;
    if (play_amiga_weapon(id, m)) return 1;
    play_mapped(id);
    if (SDL_getenv("DS_SFX_DEBUG")) SDL_Log("sfx %d -> Amiga %s (frame %llu)", id, sfxmap[m].sample, (unsigned long long)g_frame);
    return 1;
}

/* The rotor: Desert plays it on YM channel 5 (index 4) whenever the
 * helicopter's blades turn. With Amiga effects on and a rotor sample
 * assigned, that channel is left out of the mix and the Amiga rotor plays
 * instead, following the Genesis rotor on and off.
 *
 * Modelled on the Amiga game's own rotor routine (disk 1 program, $0073C4):
 * the rotor sample loops on two channels, left and right, at a shared base
 * period that eases from 487 (idle on deck) down to 307 (flying: spin-up at
 * 5 a frame, run-down at 1 a frame). Speed lowers both periods (pitch up)
 * and raises the level; turning pulls the two channels apart in pitch (left
 * up, right down, or the reverse), which is the shifting, doubled sound. */
static void rotor_frame(void)
{
    extern unsigned ymfm_chan_mute;
    extern unsigned long long ymfm_chan_level[7];
    static unsigned long long last;
    static int on, quiet;
    static float period = 487;
    unsigned long long lv = ymfm_chan_level[4], d = lv >= last ? lv - last : lv;
    last = lv;
    /* Context: the playfield, and after leaving it (the pause screen, the
     * half second before the playfield counts again) for as long as the
     * Genesis rotor keeps sounding without a break. */
    static int ctx;
    quiet = d > 3000 ? 0 : quiet + 1;               /* the Genesis rotor is sounding */
    if (hud_ok) ctx = 1;
    else if (quiet >= 6) ctx = 0;
    int m = game && !SDL_strcmp(game->id, "desert") && ctx && !att_on && !sandbox && g_set.amiga_sfx ? sfxmap_find(SFX_ROTOR) : -1;
    const amiga_sample *a = m >= 0 ? amiga_sample_at(amiga_sample_find(sfxmap[m].sample)) : NULL;
    if (!a) {
        if (on && SDL_getenv("DS_SFX_DEBUG")) SDL_Log("rotor context lost (frame %llu)", (unsigned long long)g_frame);
        if (on) { sfx_stop(SFX_ROTOR); sfx_stop(SFX_ROTOR + 1); on = 0; }
        ymfm_chan_mute &= ~0x10u;
        return;
    }
    ymfm_chan_mute |= 0x10u;
    /* The helicopter: position record [[$FF251C]+$28]: +8 height (16.16;
     * 11 on deck, 20 flying), +$14 turn (high word, about +-$20 at full
     * turn), +$18 / +$1C velocity (16.16 px/frame, about 1.37 at full speed). */
    #define RL(ad) ((uint32_t)g_ram[(ad) & 0xFFFF] << 24 | (uint32_t)g_ram[((ad) + 1) & 0xFFFF] << 16 | (uint32_t)g_ram[((ad) + 2) & 0xFFFF] << 8 | g_ram[((ad) + 3) & 0xFFFF])
    uint32_t pl = RL(0xFF251C) & 0xFFFFFF, pos = pl >= 0xFF0000 ? RL(pl + 0x28) & 0xFFFFFF : 0;
    float sp = 0, turn = 0;
    int flying = 0;
    if (pos >= 0xFF0000) {
        float vx = (int32_t)RL(pos + 0x18) / 65536.0f, vy = (int32_t)RL(pos + 0x1C) / 65536.0f;
        sp = SDL_min(SDL_sqrtf(vx * vx + vy * vy) / 1.37f, 1.2f);
        turn = SDL_clamp((int16_t)(RL(pos + 0x14) >> 16) / 32.0f, -1.0f, 1.0f);
        flying = (int32_t)RL(pos + 8) > 0x000C0000;
    }
    #undef RL
    if (flying) period = SDL_max(307.0f, period - 5);  /* spin up */
    else period = SDL_min(487.0f, period + 1);       /* run down on the deck */
    float sd = sp * 54;                                /* the Amiga: |speed| x 1.5 off the period */
    float pl_ = period - sd + turn * 14, pr_ = period - sd - turn * 14;
    float vol = (23 + sd / 8) / 23.0f * g_set.amiga_vol / 100.0f * 0.05f;
    if (quiet < 6) {
        if (!on && SDL_getenv("DS_SFX_DEBUG")) SDL_Log("rotor on (frame %llu)", (unsigned long long)g_frame);
        if (!on && hud_ok) lab_note(SFX_ROTOR);     /* so the Sound Lab can change it */
        float k = sfxmap[m].rate / (3546895.0f / 480);   /* the sample's own rate is period 480 */
        sfx_loop_pan(SFX_ROTOR, a->pcm, a->len, (int)(3546895.0f / pl_ * k), vol, -0.6f);
        sfx_loop_pan(SFX_ROTOR + 1, a->pcm, a->len, (int)(3546895.0f / pr_ * k), vol, 0.6f);
        on = 1;
    } else if (on) {
        if (SDL_getenv("DS_SFX_DEBUG")) SDL_Log("rotor off (frame %llu)", (unsigned long long)g_frame);
        sfx_stop(SFX_ROTOR); sfx_stop(SFX_ROTOR + 1); on = 0;
    }
}

/* ---------------- Amiga cutscene pictures ----------------
 * The scene player ($005994) marks the scene being shown (g_scene_rec);
 * a scene record holds the pictures (+$12, +$16) and the song (+$A). */
static uint32_t rom_l(uint32_t a)
{
    if (a + 4 > g_rom_size) return 0;
    return (uint32_t)g_rom[a] << 24 | (uint32_t)g_rom[a + 1] << 16 | (uint32_t)g_rom[a + 2] << 8 | g_rom[a + 3];
}

/* Genesis scene pictures shown as their Amiga counterparts. pic: the scene
 * record's picture (+$12); amiga: the cutscene picture/animation id (content
 * hash, amiga.c); g*: the rectangle the Genesis picture covers (320x224
 * screen); a*: the part of the Amiga picture to show (0 width: all of it). */
typedef struct { uint32_t pic; const char *amiga; int gx, gy, gw, gh; int ax, ay, aw, ah; int loop; const char *ground;
                 int vm, vdy, vsy, vsh; } scene_swap;
/* vm: run the cutscene program that holds this picture (amiga_vm.c) and show
 * its screen, rows vsy.. (vsh rows, 0: all) at Genesis row vdy, centred. */
static const scene_swap scene_swaps[] = {
    /* the story (attract and new game); Amiga pictures are 352 wide, so
     * they spill 16 px into the side bars, as on an Amiga screen */
    { 0x0DF748, "cCF6F2DB24234DA73", -16, 48, 352, 112, 0, 0, 352, 112, 0 },   /* the dungeon (animated) */
    { 0x0E40E8, "c8A0DB017CF3305C9", -16, 48, 352, 112, 0, 48, 352, 112, 0 },  /* the madman's secret bunker */
    { 0x0E676C, "c5070193A91B7CFD4", -16, 48, 352, 112, 0, 48, 352, 112, 0 },  /* Kilbaba at the map */
    { 0x0E9626, "c65214A8BD0533C8C", -16, 48, 352, 112, 0, 0, 352, 112, 3 },   /* the general: Kilbaba slaps him once, only in the scene with the slap (a second picture); elsewhere he just stands (issue #10) */
    { 0x0F186A, "c842B0798B98EBBF0", -16, 48, 352, 112, 0, 48, 352, 112, 0 },  /* the TV news report */
    { 0x0F47FA, "c31C4574AE4C38B0A", 0, 48, 320, 112, 0, 19, 320, 112, 0, NULL, 1, 48, 19, 112 },   /* the Apache on the frigate, crew at work */
    /* the refinery: the Amiga scrolls a 640 px sky (with the refinery) and an
     * 816 px ground strip past at two speeds; 88 + 24 rows = the picture */
    { 0x0EE488, "cB757CAFFE3F99720", -16, 48, 352, 88, 0, 0, 352, 88, 2, "cD58D5552E5214F2A", 1, 48, 0, 112 },
    /* the campaign briefing: the briefing room (board and officer) in the
     * Genesis's portrait panel left of the text */
    { 0x0AF484, "c8AD6F5C975A58B8A", 2, 24, 156, 176, 70, 5, 160, 150, 0 },
    /* outcomes */
    { 0x0ABC3E, "cFB3083BE051B0071", 24, 16, 152, 192, 85, 0, 150, 150, 0 },   /* campaign won: the Apache home on the deck */
    { 0x0ADEDC, "c091FDEF2CF1CF57E", -16, 2, 352, 220, 0, 0, 352, 220, 0, NULL, 1, 0, 0, 224 },    /* game over: the burning wreck in the desert */
    { 0x0D9182, "c3522000D86C85073", -16, 0, 352, 224, 0, 0, 352, 224, 0, NULL, 1, 0, 0, 224 },    /* the ending: the ceremony */
};

/* Overlays the Amiga cutscene programs animate on a picture (bobs, amiga.c),
 * as their code does it. The briefing room (disk 2 program, $000178): the
 * cigarette smoke cycles bobs 0-2 every 6 frames at (143, 103); each seated
 * pilot shows a random one of three bobs (3-5 at (124, 90), 6-8 at
 * (171, 90)) and the officer one of four (9-12 at (148, 38)), each on its
 * own random 10-29 frame timer. Frames are PAL vblanks (20 ms). */
typedef struct { const char *pic; int first, count, x, y, cycle, rmin, rmax; } scene_actor;
static const scene_actor scene_actors[] = {
    { "c8AD6F5C975A58B8A", 0, 3, 143, 103, 6, 0, 0 },
    { "c8AD6F5C975A58B8A", 3, 3, 124, 90, 0, 10, 29 },
    { "c8AD6F5C975A58B8A", 6, 3, 171, 90, 0, 10, 29 },
    { "c8AD6F5C975A58B8A", 9, 4, 148, 38, 0, 10, 29 },
};
#define MAX_ACTORS 8

static struct {
    const scene_swap *sw;
    const amiga_anim *a;
    SDL_Texture *tex, *gtex;        /* gtex: the ground strip of a scrolling scene */
    const amiga_anim *g;
    uint64_t t0;
    int frame, shown;
    uint32_t rec;                   /* the scene record shown (an animation restarts with each) */
    int still;                      /* hold the first frame */
    uint32_t wait;                  /* ms of the last frame step */
    int nact;
    struct { const scene_actor *a; int bob, timer; } act[MAX_ACTORS];
    uint32_t vbl;                   /* ms of the last 50 Hz tick */
    uint32_t *rgba;                 /* composited frame */
    int vm;                         /* the scene's program runs in the Amiga VM */
    SDL_Texture *vtex;
    uint32_t vtick;                 /* ms of the last VM frame */
} sc;

static const scene_swap *scene_lookup(uint32_t rec)
{
    if (!rec || !g_set.amiga_cutscenes || !game || SDL_strcmp(game->id, "desert")) return NULL;
    uint32_t pic = rom_l(rec + 0x12);
    for (size_t i = 0; i < sizeof scene_swaps / sizeof scene_swaps[0]; i++)
        if (scene_swaps[i].pic && scene_swaps[i].pic == pic && (amiga_anim_find(scene_swaps[i].amiga) >= 0 || (scene_swaps[i].vm && amiga_program_find(scene_swaps[i].amiga)))) return &scene_swaps[i];
    return NULL;
}

/* Called once per displayed frame: steps the animation (about 12 frames a
 * second, as the Amiga's cutscene programs play them). */
static void scene_step(void)
{
    const scene_swap *sw = scene_lookup(g_scene_rec);
    if (sw != sc.sw || (sw && sw->loop == 3 && g_scene_rec != sc.rec)) {
        sc.rec = g_scene_rec;
        sc.still = sw && sw->loop == 3 && !rom_l(g_scene_rec + 0x16);
        sc.sw = sw;
        sc.a = sw ? amiga_anim_at(amiga_anim_find(sw->amiga)) : NULL;
        sc.frame = 0; sc.shown = -1; sc.wait = (uint32_t)SDL_GetTicks();
        if (sc.tex) { SDL_DestroyTexture(sc.tex); sc.tex = NULL; }
        if (sc.gtex) { SDL_DestroyTexture(sc.gtex); sc.gtex = NULL; }
        sc.g = sw && sw->ground ? amiga_anim_at(amiga_anim_find(sw->ground)) : NULL;
        sc.t0 = SDL_GetTicks();
        if (sc.g && (sc.gtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, sc.g->w, sc.g->h))) {
            uint32_t *px = SDL_malloc((size_t)sc.g->w * sc.g->h * 4);
            if (px) {
                for (int i = 0; i < sc.g->w * sc.g->h; i++) px[i] = sc.g->pal[sc.g->frames[0][i]];
                SDL_UpdateTexture(sc.gtex, NULL, px, sc.g->w * 4);
                SDL_free(px);
            }
        }
        SDL_free(sc.rgba); sc.rgba = NULL;
        sc.nact = 0;
        if (sc.vm) { avm_stop(); sc.vm = 0; }
        if (sc.vtex) { SDL_DestroyTexture(sc.vtex); sc.vtex = NULL; }
        const amiga_program *pg = sw && sw->vm ? amiga_program_find(sw->amiga) : NULL;
        if (pg && avm_start(pg->data, pg->n)) {
            sc.vm = 1;
            sc.vtick = (uint32_t)SDL_GetTicks();
            sc.vtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, AVM_W, AVM_H);
            if (sc.vtex) SDL_SetTextureScaleMode(sc.vtex, SDL_SCALEMODE_LINEAR);
            avm_frame();
        }
        sc.vbl = (uint32_t)SDL_GetTicks();
        if (sc.a) {
            sc.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, sc.a->w, sc.a->h);
            if (sc.tex) SDL_SetTextureScaleMode(sc.tex, SDL_SCALEMODE_LINEAR);
            sc.rgba = SDL_malloc((size_t)sc.a->w * sc.a->h * 4);
            for (size_t i = 0; i < sizeof scene_actors / sizeof scene_actors[0] && sc.nact < MAX_ACTORS; i++)
                if (!SDL_strcmp(scene_actors[i].pic, sc.a->id)) { sc.act[sc.nact].a = &scene_actors[i]; sc.act[sc.nact].bob = 0; sc.act[sc.nact].timer = 0; sc.nact++; }
        }
    }
    if (sc.vm) {                    /* the program's own frames, 50 a second */
        uint32_t now = (uint32_t)SDL_GetTicks();
        int ran = 0;
        for (int guard = 0; now - sc.vtick >= 20 && guard < 4; guard++) { sc.vtick += 20; avm_frame(); ran = 1; }
        if (now - sc.vtick > 200) sc.vtick = now;
        if (ran && sc.vtex) SDL_UpdateTexture(sc.vtex, NULL, avm_screen(NULL, NULL), AVM_W * 4);
        return;
    }
    if (!sc.a || !sc.tex) return;
    uint64_t now = SDL_GetTicks();
    if (sc.a->nframes > 1 && !sc.still && (uint32_t)now - sc.wait >= 83) {    /* about 12 frames a second */
        sc.wait = (uint32_t)now;
        if (sc.frame + 1 < sc.a->nframes) sc.frame++;
        else if (sc.sw->loop == 1) sc.frame = 1;
    }
    int dirty = sc.frame != sc.shown;
    /* the program's overlays, ticking at 50 Hz */
    for (int guard = 0; sc.nact && (uint32_t)now - sc.vbl >= 20 && guard < 10; guard++) {
        sc.vbl += 20;
        for (int i = 0; i < sc.nact; i++) {
            const scene_actor *a = sc.act[i].a;
            if (--sc.act[i].timer > 0) continue;
            if (a->cycle) { sc.act[i].bob = (sc.act[i].bob + 1) % a->count; sc.act[i].timer = a->cycle; }
            else { sc.act[i].bob = SDL_rand(a->count); sc.act[i].timer = a->rmin + SDL_rand(a->rmax - a->rmin + 1); }
            dirty = 1;
        }
    }
    if ((uint32_t)now - sc.vbl > 200) sc.vbl = (uint32_t)now;      /* after a stall */
    if (dirty && sc.rgba) {
        const uint8_t *f = sc.a->frames[sc.frame];
        for (int i = 0; i < sc.a->w * sc.a->h; i++) sc.rgba[i] = sc.a->pal[f[i]];
        for (int i = 0; i < sc.nact; i++) {
            const scene_actor *a = sc.act[i].a;
            const amiga_bob *b = NULL;
            for (int k = 0; k < amiga_bob_count(); k++) {
                const amiga_bob *c = amiga_bob_at(k);
                if (c->index == a->first + sc.act[i].bob && !SDL_strcmp(c->owner, sc.a->id)) { b = c; break; }
            }
            if (!b) continue;
            for (int y = 0; y < b->h; y++)
                for (int x = 0; x < b->w; x++) {
                    int X = a->x + x, Y = a->y + y;
                    if (X < 0 || Y < 0 || X >= sc.a->w || Y >= sc.a->h || !b->mask[y * b->w + x]) continue;
                    sc.rgba[(size_t)Y * sc.a->w + X] = sc.a->pal[b->px[y * b->w + x]];
                }
        }
        SDL_UpdateTexture(sc.tex, NULL, sc.rgba, sc.a->w * 4);
        sc.shown = sc.frame;
    }
}

static void draw_scene_swap(void)
{
    if (!sc.sw || hud_ok) return;
    const scene_swap *w = sc.sw;
    float kx = game_dst.w / fb_w, ky = game_dst.h / VDP_H;
    float ox = (float)(fb_w - 320) / 2;                    /* the picture is centred in a wider frame buffer */
    if (sc.vm) {
        int vw, vh;
        avm_screen(&vw, &vh);
        if (!sc.vtex || vw <= 0 || vh <= w->vsy) return;
        int rows = vh - w->vsy;
        if (w->vsh && rows > w->vsh) rows = w->vsh;
        if (w->vdy + rows > VDP_H) rows = VDP_H - w->vdy;
        SDL_FRect vs = { 0, (float)w->vsy, (float)vw, (float)rows };
        int drows = w->vsh && rows < w->vsh && w->vsh - rows <= 2 ? w->vsh : rows;   /* the refinery is a row short */
        SDL_FRect vd = { game_dst.x + (fb_w - vw) / 2.0f * kx, game_dst.y + w->vdy * ky, vw * kx, drows * ky };
        SDL_RenderTexture(ren, sc.vtex, &vs, &vd);
        return;
    }
    if (!sc.tex) return;
    SDL_FRect dst = { game_dst.x + (ox + w->gx) * kx, game_dst.y + w->gy * ky, w->gw * kx, w->gh * ky };
    SDL_FRect src = { (float)w->ax, (float)w->ay, (float)(w->aw ? w->aw : sc.a->w), (float)(w->ah ? w->ah : sc.a->h) };
    if (w->loop != 2) { SDL_RenderTexture(ren, sc.tex, &src, &dst); return; }
    /* scrolling strips: a 352 px window moving over a wrapping strip */
    float t = (SDL_GetTicks() - sc.t0) / 1000.0f;
    struct { SDL_Texture *tx; int sw, sh; float speed, y, h; } L[2] = {
        { sc.tex, sc.a->w, sc.a->h, 8, dst.y, dst.h },
        { sc.gtex, sc.g ? sc.g->w : 0, sc.g ? sc.g->h : 0, 40, dst.y + dst.h, 24 * ky },
    };
    for (int k = 0; k < 2; k++) {
        if (!L[k].tx || !L[k].sw) continue;
        float off = SDL_fmodf(t * L[k].speed, (float)L[k].sw), win = 352, px = dst.w / win;
        float first = SDL_min(win, L[k].sw - off);
        SDL_FRect s1 = { off, 0, first, (float)L[k].sh }, d1 = { dst.x, L[k].y, first * px, L[k].h };
        SDL_RenderTexture(ren, L[k].tx, &s1, &d1);
        if (first < win) {
            SDL_FRect s2 = { 0, 0, win - first, (float)L[k].sh }, d2 = { dst.x + first * px, L[k].y, (win - first) * px, L[k].h };
            SDL_RenderTexture(ren, L[k].tx, &s2, &d2);
        }
    }
}

static void scene_frame(void)
{
    if (SDL_getenv("DS_SPR_LOG") && hud_ok) {   /* sprite X words whose high bits are not a sign extension */
        uint32_t sat = (g_vreg[5] & 0x7E) << 9;
        for (int k = 0; k < 80; k++) {
            uint16_t x = (uint16_t)(g_vram[(sat + k * 8 + 6) & 0xFFFF] << 8 | g_vram[(sat + k * 8 + 7) & 0xFFFF]);
            int hi = x >> 9;
            if (hi != 0 && hi != 0x7F) SDL_Log("spr %d x %04X frame %llu", k, x, (unsigned long long)g_frame);
        }
    }
    input_rstick_strafe = g_set.rstick_strafe && game && !SDL_strcmp(game->id, "desert");
    { int k = extracting || att_on ? 2 : 2 + g_set.strafe_speed;      /* Strafe Speed setting: x1, x1.5, x2 */
      g_strafe_speed = 0xC00 * k / 2; g_strafe_mul = 0x80 * k / 2; }
    if (SDL_getenv("DS_VEL_LOG") && g_frame % 10 == 0) { uint32_t pl = R32(0xFF251C) & 0xFFFFFF, ps = pl ? R32(pl + 0x28) & 0xFFFFFF : 0; if (ps >= 0xFF0000) SDL_Log("vel %d %d pad %02X frame %llu ss %X f1088 %X", (int)R32(ps + 0x18), (int)R32(ps + 0x1C), g_ram[0x1C79], (unsigned long long)g_frame, (unsigned)g_strafe_speed, (unsigned)R32(0xFF1088)); }
    if (SDL_getenv("DS_PC_LOG") && g_frame % 30 == 0) SDL_Log("pc %06X d7 %08X 254 %08X 25c %08X 258 %08X 20c %08X 1c7d %02X frame %llu", R.pc, R.d[7], R32(0xFF0254), R32(0xFF025C), R32(0xFF0258), R32(0xFF020C), (unsigned)(R32(0xFF1C7C) & 0xFF), (unsigned long long)g_frame);
    if (g_frame == 3000 && SDL_getenv("DS_ROM_POKE")) {   /* test: <addr>:<long>[,...] patches ROM data after boot (the game checksums its ROM at start) */
        for (const char *e = SDL_getenv("DS_ROM_POKE"); e && *e; e = SDL_strchr(e, ',') ? SDL_strchr(e, ',') + 1 : NULL) {
            unsigned a = 0, v = 0;
            if (SDL_sscanf(e, "%x:%x", &a, &v) == 2 && a + 4 <= g_rom_size) {
                g_rom[a] = (uint8_t)(v >> 24); g_rom[a + 1] = (uint8_t)(v >> 16); g_rom[a + 2] = (uint8_t)(v >> 8); g_rom[a + 3] = (uint8_t)v;
            }
        }
    }
    static uint32_t last;
    static long long shot_at = -1;
    static char shot_name[64];
    uint32_t r = g_scene_rec;
    if (r != last) {
        last = r;
        if (r && SDL_getenv("DS_SCENE_LOG"))
            SDL_Log("scene %06X pic %06X pic2 %06X song %d (frame %llu)", r, rom_l(r + 0x12), rom_l(r + 0x16),
                    (int16_t)(rom_l(r + 0xA) >> 16), (unsigned long long)g_frame);
        const char *dir = SDL_getenv("DS_SCENE_SHOTS");
        if (r && dir) { shot_at = (long long)g_frame + 100; SDL_snprintf(shot_name, sizeof shot_name, "%s/scene_%06X.bmp", dir, r); }
    }
    if (shot_at >= 0 && (long long)g_frame >= shot_at) { shot_at = -1; save_screen(shot_name); }
}

/* ---------------- Amiga music ----------------
 * The game's music requests ($00890E) are keyed by id; a request from the
 * campaign briefing gets 100000 + id (g_music_ctx), since campaign 1's
 * briefing reuses the title song; the bunker song after the last life is
 * lost is the game over, 300000 + id. amiga_music.ini ("desert 117 datitle",
 * or "genesis") overrides the defaults below; the Sound Lab edits it. */
typedef struct { char game[12]; int key; char module[24]; } musmap_t;
static const struct { const char *game; int key; const char *module; } music_defaults[] = {
    { "desert", 105, "datitle" }, { "desert", 117, "datitle" },    /* EA logo + title, title */
    { "desert", 9, "story1" }, { "desert", 107, "industry" },     /* story: "The Middle East", the refinery */
    { "desert", 3, "main" },                                       /* story: the madman's bunker */
    { "desert", 100117, "briefing" }, { "desert", 100104, "briefing" },  /* campaign briefings */
    { "desert", 100004, "briefing" }, { "desert", 100109, "briefing" },
    { "desert", 300003, "death" },                                 /* game over (the bunker song after the last life) */
    { "desert", 118, "winlevel" },                                 /* campaign complete */
    { "desert", 2, "death" },                                      /* mission failed */
    { "desert", 108, "end" }, { "desert", 10, "end" },             /* ending, credits */
};
static musmap_t musmap[64];
static int nmusmap, lab_mus = -1;
static char mus_playing[24];
static char musmap_path[1100];

static const char *music_name(int key)
{
    if (!game || SDL_strcmp(game->id, "desert")) return "";
    if (key >= 300000) return "game over";
    if (key >= 100000) return "briefing";
    switch (key) {
    case 105: return "EA logo, title"; case 117: return "title"; case 3: return "bunker";
    case 9: return "story"; case 107: return "refinery"; case 118: return "campaign complete";
    case 2: return "mission failed"; case 108: case 10: return "ending";
    case 7280: return "life lost";
    default: return "";
    }
}

/* The module for a key: NULL = the Genesis music. */
static const char *music_module(int key)
{
    if (!game) return NULL;
    for (int i = 0; i < nmusmap; i++)
        if (musmap[i].key == key && !SDL_strcmp(musmap[i].game, game->id))
            return SDL_strcmp(musmap[i].module, "genesis") ? musmap[i].module : NULL;
    for (size_t i = 0; i < sizeof music_defaults / sizeof music_defaults[0]; i++)
        if (music_defaults[i].key == key && !SDL_strcmp(music_defaults[i].game, game->id)) return music_defaults[i].module;
    return NULL;
}

static void musmap_load(void)
{
    nmusmap = 0;
    FILE *f = fopen(musmap_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && nmusmap < 64) {
        if (line[0] == '#') continue;
        musmap_t m;
        if (SDL_sscanf(line, "%11s %d %23s", m.game, &m.key, m.module) == 3) musmap[nmusmap++] = m;
    }
    fclose(f);
}

static void musmap_save(void)
{
    FILE *f = fopen(musmap_path, "w");
    if (!f) return;
    fprintf(f, "# Amiga music: <game> <song id> <module or genesis>. Briefing songs are 100000 + id, game over 300003. Edit with the Sound Lab.\n");
    for (int i = 0; i < nmusmap; i++) fprintf(f, "%s %d %s\n", musmap[i].game, musmap[i].key, musmap[i].module);
    fclose(f);
}

static void music_set(int key, const char *module)
{
    int i = 0;
    while (i < nmusmap && !(musmap[i].key == key && !SDL_strcmp(musmap[i].game, game->id))) i++;
    if (i == nmusmap) { if (nmusmap >= 64) return; nmusmap++; }
    SDL_strlcpy(musmap[i].game, game->id, sizeof musmap[i].game);
    musmap[i].key = key;
    SDL_strlcpy(musmap[i].module, module ? module : "genesis", sizeof musmap[i].module);
    musmap_save();
}

static void music_stop(void) { mod_stop(); mus_playing[0] = 0; }

static int music_play(int key)
{
    const char *name = music_module(key);
    const amiga_module *m = name ? amiga_module_at(amiga_module_find(name)) : NULL;
    if (!m) return 0;
    mod_set_volume(g_set.amiga_music_vol / 100.0f);
    if (!SDL_strcmp(mus_playing, name) && mod_playing()) return 1;   /* already on: carry on */
    SDL_strlcpy(mus_playing, name, sizeof mus_playing);
    return mod_start(m->data, m->len, AUDIO_RATE, SDL_strcmp(name, "death") != 0);
}

/* Amiga extras. A Sound Lab mapping for the event (a voice line, say) wins;
 * otherwise the objective gets the Amiga objective jingle. Each event has a
 * cooldown so warnings don't nag. */
static void amiga_event(int ev)
{
    static uint64_t next_ok[8];
    if (!game || sandbox || extracting || att_on || !g_set.amiga_extras) return;
    int k = (ev - EV_OBJECTIVE) & 7;
    if (g_frame < next_ok[k]) return;
    next_ok[k] = g_frame + 60 * 12;
    if (ev == EV_RESCUE_FAR || ev == EV_RESCUE_NEAR)          /* one call at a time, every 7 s at most */
        next_ok[(EV_RESCUE_FAR - EV_OBJECTIVE) & 7] = next_ok[(EV_RESCUE_NEAR - EV_OBJECTIVE) & 7] = g_frame + 60 * 7;
    if (SDL_getenv("DS_SFX_DEBUG")) SDL_Log("event %d %s (frame %llu)", ev, sfx_name(ev), (unsigned long long)g_frame);
    lab_note(ev);
    if (sfxmap_find(ev) >= 0) play_mapped(ev);        /* a voice line, say */
    if (ev == EV_OBJECTIVE && !mod_playing()) {       /* and the Amiga objective jingle */
        const amiga_module *m = amiga_module_at(amiga_module_find("obj"));
        if (!m) return;
        mod_set_volume(g_set.amiga_music_vol / 100.0f * 0.6f);   /* a cue over the action, not a song */
        SDL_strlcpy(mus_playing, "obj", sizeof mus_playing);
        mod_start(m->data, m->len, AUDIO_RATE, 0);
    }
}

/* While a jingle plays, the game's own sound steps back. */
static void duck_game_audio(int16_t *buf, int frames)
{
    static float g = 1;
    float target = (mod_playing() && !SDL_strcmp(mus_playing, "obj")) ? 0.45f : 1.0f;
    for (int f = 0; f < frames; f++) {
        g += (target - g) * 0.0004f;                 /* ~50 ms glide */
        buf[f * 2] = (int16_t)(buf[f * 2] * g);
        buf[f * 2 + 1] = (int16_t)(buf[f * 2 + 1] * g);
    }
}

/* Called by the game's music request (hints.json patch): 0 = play the
 * Genesis one; 1 = skip it, the Amiga module plays instead; 2 = the same,
 * but stop the Genesis song that is still playing. Skipping is preferred:
 * the stop path waits for vblanks, and the campaign briefing hangs if it
 * is made to stop there. */
static int host_music_hook(int id)
{
    static int last_id, genesis_on;
    if (!game || sandbox || extracting || att_on) return 0;
    int key = g_music_ctx == 1 ? 100000 + id : id;
    if (id == 0 || id == 100) { music_stop(); genesis_on = 0; return 0; }
    if (id == 3 && last_id == 7280) key = 300000 + id;
    last_id = id;
    if (g_sound_lab) { lab_mus = key; lab_flash = SDL_GetTicksNS() + 1500000000ull; }
    if (SDL_getenv("DS_SFX_DEBUG")) SDL_Log("music %d -> %s (frame %llu)", key, music_module(key) ? music_module(key) : "genesis", (unsigned long long)g_frame);
    if (g_set.amiga_music && music_play(key)) {
        int r = genesis_on ? 2 : 1;
        genesis_on = 0;
        return r;
    }
    music_stop();
    if (id != 7280) genesis_on = 1;                  /* the life-lost jingle ends by itself */
    return 0;
}

/* Sound Lab: , . step the module for the last song (first entry: Genesis). */
static int lab_music_key(SDL_Scancode k)
{
    if (k != SDL_SCANCODE_COMMA && k != SDL_SCANCODE_PERIOD) return 0;
    if (lab_mus < 0) { say("Sound Lab: no song has played yet"); return 1; }
    int n = amiga_module_count();
    if (!n) { say("Add your Amiga disks first (Settings > Audio)"); return 1; }
    const char *cur = music_module(lab_mus);
    int i = cur ? amiga_module_find(cur) + 1 : 0;      /* 0 = Genesis, 1.. = modules */
    i = (i + (k == SDL_SCANCODE_PERIOD ? 1 : n)) % (n + 1);
    music_set(lab_mus, i ? amiga_module_at(i - 1)->name : NULL);
    music_stop();
    if (i && g_set.amiga_music) music_play(lab_mus);
    lab_flash = SDL_GetTicksNS() + 1500000000ull;
    return 1;
}

/* Sound Lab keys (while flying with the lab on). */
static int lab_key(SDL_Scancode k)
{
    if (!g_sound_lab || !game || menu_is_open()) return 0;
    if (lab_music_key(k)) return 1;
    if (lab_last < 0) return 0;
    int n = amiga_sample_count();
    if (!n) { say("Add your Amiga disks first (Settings > Audio)"); return 1; }
    int m = sfxmap_find(lab_last);
    if (k == SDL_SCANCODE_0) {
        if (m < 0 && nsfxmap < MAX_MAP) {
            m = nsfxmap++;
            SDL_strlcpy(sfxmap[m].game, game->id, sizeof sfxmap[m].game);
            sfxmap[m].id = lab_last;
            sfxmap[m].rate = 11025;
        }
        if (m >= 0) { SDL_strlcpy(sfxmap[m].sample, "genesis", sizeof sfxmap[m].sample); sfxmap[m].def = 0; sfxmap_save(); }
        lab_flash = SDL_GetTicksNS() + 1500000000ull;
        return 1;
    }
    if (k != SDL_SCANCODE_LEFTBRACKET && k != SDL_SCANCODE_RIGHTBRACKET && k != SDL_SCANCODE_MINUS &&
        k != SDL_SCANCODE_EQUALS && k != SDL_SCANCODE_BACKSLASH) return 0;
    if (m < 0) {
        if (nsfxmap >= MAX_MAP) return 1;
        m = nsfxmap++;
        SDL_strlcpy(sfxmap[m].game, game->id, sizeof sfxmap[m].game);
        sfxmap[m].id = lab_last;
        SDL_strlcpy(sfxmap[m].sample, amiga_sample_at(0)->id, sizeof sfxmap[m].sample);
        sfxmap[m].rate = amiga_sample_at(0)->rate ? amiga_sample_at(0)->rate : 11025;
        sfxmap[m].def = 0;
        if (k == SDL_SCANCODE_RIGHTBRACKET || k == SDL_SCANCODE_LEFTBRACKET) k = SDL_SCANCODE_BACKSLASH;   /* first pick: sample 1 */
    }
    sfxmap[m].def = 0;                                 /* edited: now the player's own */
    int cur = amiga_sample_find(sfxmap[m].sample);
    if (cur < 0) { cur = 0; if (k == SDL_SCANCODE_RIGHTBRACKET) cur = n - 1; }   /* from Genesis: ] gives fx 1 */
    if (k == SDL_SCANCODE_RIGHTBRACKET) cur = (cur + 1) % n;
    if (k == SDL_SCANCODE_LEFTBRACKET) cur = (cur + n - 1) % n;
    if ((k == SDL_SCANCODE_RIGHTBRACKET || k == SDL_SCANCODE_LEFTBRACKET) && amiga_sample_at(cur)->rate)
        sfxmap[m].rate = amiga_sample_at(cur)->rate;     /* the game's own pitch for its effects */
    SDL_strlcpy(sfxmap[m].sample, amiga_sample_at(cur)->id, sizeof sfxmap[m].sample);
    if (k == SDL_SCANCODE_EQUALS) sfxmap[m].rate = SDL_min(44100, (int)(sfxmap[m].rate * 1.0595f + 0.5f));
    if (k == SDL_SCANCODE_MINUS) sfxmap[m].rate = SDL_max(2000, (int)(sfxmap[m].rate / 1.0595f + 0.5f));
    sfxmap_save();
    play_mapped(lab_last);
    lab_flash = SDL_GetTicksNS() + 1500000000ull;
    return 1;
}

static void draw_sound_lab(void)
{
    if (!g_sound_lab || !game || menu_is_open()) return;
    float w = 470, x = 1280 - w - 20, y = 20, lh = ui_line_h(UI_SMALL);
    int rows = 5 + lab_nhist;
    ui_panel(x, y, w, 16 + lh * rows, 8, ui_rgba(14, 17, 12, 200), ui_rgba(196, 164, 98, 200));
    SDL_FColor lab = ui_rgba(196, 164, 98, 255), txt = ui_rgba(232, 226, 210, 255), hot = ui_rgba(255, 214, 72, 255);
    char l[160];
    float cy = y + 8;
    SDL_snprintf(l, sizeof l, "SOUND LAB  %d Amiga samples  %d assigned", amiga_sample_count(), nsfxmap);
    ui_text(UI_SMALL, x + 12, cy, lab, l); cy += lh;
    ui_text(UI_SMALL, x + 12, cy, ui_rgba(150, 148, 132, 255), "[ ] sample   - = pitch   0 Genesis   \\ replay   , . music"); cy += lh;
    if (lab_mus >= 0) {
        const char *mm = music_module(lab_mus);
        SDL_snprintf(l, sizeof l, "Song %d %s: %s%s", lab_mus, music_name(lab_mus), mm ? "Amiga " : "Genesis", mm ? mm : "");
    } else SDL_snprintf(l, sizeof l, "Song: none yet (%d Amiga songs)", amiga_module_count());
    ui_text(UI_SMALL, x + 12, cy, txt, l); cy += lh;
    if (lab_last < 0) { ui_text(UI_SMALL, x + 12, cy, txt, "Play: the next sound effect shows here."); return; }
    for (int i = 0; i < lab_nhist; i++) {
        int id = lab_hist[i], m = sfxmap_find(id);
        if (m >= 0) {
            int si = amiga_sample_find(sfxmap[m].sample);
            const amiga_sample *as = amiga_sample_at(si);
            if (!as) SDL_snprintf(l, sizeof l, "#%-2d %-16s Genesis", id, sfx_name(id));
            else SDL_snprintf(l, sizeof l, "#%-2d %-16s %2d %-14s %d Hz%s", id, sfx_name(id), si + 1, as->label[0] ? as->label : "Amiga", sfxmap[m].rate, sfxmap[m].def ? " (default)" : "");
        } else SDL_snprintf(l, sizeof l, "#%-2d %-16s Genesis", id, sfx_name(id));
        int fresh = i == 0 && SDL_GetTicksNS() < lab_flash;
        ui_text(UI_SMALL, x + 12, cy, i == 0 ? (fresh ? hot : txt) : ui_rgba(170, 166, 150, 255), l);
        cy += lh;
    }
}

static volatile int amiga_dialog_state;
static char amiga_dialog_paths[8][1024];
static int amiga_dialog_n;
static void SDLCALL amiga_dialog_cb(void *ud, const char *const *files, int filter)
{
    (void)ud; (void)filter;
    amiga_dialog_n = 0;
    for (int i = 0; files && files[i] && i < 8; i++) SDL_strlcpy(amiga_dialog_paths[amiga_dialog_n++], files[i], 1024);
    amiga_dialog_state = files && files[0] ? 1 : 2;
}

static void add_amiga_disks(void)
{
    static const SDL_DialogFileFilter filters[] = { { "Amiga disk image", "adf" }, { "All files", "*" } };
    amiga_dialog_state = 0;
    SDL_ShowOpenFileDialog(amiga_dialog_cb, 0, win, filters, 2, 0, true);
    while (amiga_dialog_state == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) { running = 0; return; }
        SDL_Delay(10);
    }
    if (amiga_dialog_state != 1) return;
    SDL_CreateDirectory(amiga_dir);
    for (int i = 0; i < amiga_dialog_n; i++) {
        const char *base = SDL_strrchr(amiga_dialog_paths[i], '/');
        const char *b2 = SDL_strrchr(amiga_dialog_paths[i], '\\');
        if (b2 > base) base = b2;
        char dst[1200];
        SDL_snprintf(dst, sizeof dst, "%s/%s", amiga_dir, base ? base + 1 : amiga_dialog_paths[i]);
        SDL_CopyFile(amiga_dialog_paths[i], dst);
    }
    int n = amiga_load_dir(amiga_dir);
    char m[96];
    if (n) SDL_snprintf(m, sizeof m, "Amiga disks: %d sound samples, %d songs found", n, amiga_module_count());
    else SDL_snprintf(m, sizeof m, "No Amiga Desert Strike samples found on those disks");
    say(m);
}

/* ---------------- audio ---------------- */
static void pump_audio(int keep)
{
    static int16_t buf[4096 * 2];
    int n;
    while ((n = snd_read(buf, 4096)) > 0) {
        duck_game_audio(buf, n);
        mod_mix(buf, n);
        sfx_mix(buf, n);
        {   /* test: DS_AUDIO_DUMP=<file> records the mixed output (s16 stereo, AUDIO_RATE) */
            static FILE *dump; static int tried;
            if (!tried) { tried = 1; const char *e = SDL_getenv("DS_AUDIO_DUMP"); if (e) dump = fopen(e, "wb"); }
            if (dump) fwrite(buf, 4, (size_t)n, dump);
        }
        if (keep && aud) SDL_PutAudioStreamData(aud, buf, n * 4);
    }
    if (!aud) return;
    /* Hold about 3 frames queued; nudge the playback rate to stay there. */
    float queued = SDL_GetAudioStreamQueued(aud) / (4.0f * AUDIO_RATE);
    if (!astat_now.n++ || queued * 1000 < astat_now.min_ms) astat_now.min_ms = queued * 1000;
    if (queued * 1000 > astat_now.max_ms) astat_now.max_ms = queued * 1000;
    if (keep && queued < 0.005f) astat_now.starved++;
    if (queued > 0.25f) { SDL_ClearAudioStream(aud); queued = 0; astat_now.clears++; }
    float ratio = 1.0f + (queued - 0.05f) * 0.2f;
    if (ratio < 0.99f) ratio = 0.99f;
    if (ratio > 1.01f) ratio = 1.01f;
    astat_now.ratio = ratio;
    SDL_SetAudioStreamFrequencyRatio(aud, ratio);
}

/* Show FPS: Diagnostics also writes <settings>/diagnostics.log: what the
 * system is, every setting (again whenever one changes), then a line of
 * measures a second. A new session starts the file again. */
static void diag_log(uint64_t now)
{
    static FILE *f;
    static int session_started;
    static settings_t logged;
    if (g_set.show_fps != 2) {
        if (f) { fclose(f); f = NULL; }
        return;
    }
    if (!f) {
        char path[1100];
        SDL_snprintf(path, sizeof path, "%sdiagnostics.log", pref);
        f = fopen(path, session_started ? "a" : "w");
        if (!f) return;
        SDL_Time t = 0;
        SDL_DateTime dt;
        SDL_GetCurrentTime(&t);
        SDL_TimeToDateTime(t, &dt, true);
        fprintf(f, "# %s diagnostics, %04d-%02d-%02d %02d:%02d:%02d\n", APP_TITLE, dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        fprintf(f, "# system: %s, SDL %d.%d.%d, video %s, renderer %s, %d CPU cores, %d MB RAM\n", SDL_GetPlatform(),
                SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                SDL_GetCurrentVideoDriver(), SDL_GetRendererName(ren), SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
        fprintf(f, "# game: %s\n", game ? game->id : "none");
        fprintf(f, "# graphics card: NVML (NVIDIA) or amdgpu when available; -1 in the gpu columns is unknown\n");
        session_started = 1;
        memset(&logged, 0xFF, sizeof logged);   /* write the settings below */
    }
    if (memcmp(&logged, &g_set, sizeof g_set)) {
        logged = g_set;
        fprintf(f, "# settings:\n");
        settings_write(f, "#   ");
        fprintf(f, "# time_s game_fps display_fps screen pacing frame_avg_ms frame_max_ms late early gpu_load gpu_temp gpu_w fan clock_mhz audio_min_ms audio_max_ms audio_ratio audio_clears audio_starved compositing extra\n");
    }
    char extra[768];
    diag_extra(extra, sizeof extra);
    fprintf(f, "%.0f %.1f %.1f %dx%d@%.2f %s %.2f %.2f %d %d %d %d %d %d %d %.0f %.0f %.4f %d %d %d %s\n",
            now / 1e9, fps_game, fps_disp, display_mode()->w, display_mode()->h, display_hz(), sync_on ? "display" : "clock",
            pstat.n ? pstat.sum_ms / pstat.n : 0.0, pstat.max_ms, pstat.late, pstat.early,
            gstat.load, gstat.temp, gstat.power, gstat.fan, gstat.clock,
            astat.min_ms, astat.max_ms, astat.ratio, astat.clears, astat.starved, compositing, extra);
    fflush(f);
}

/* Once a second, with the fps figures: the last second's measures, and while
 * Diagnostics is shown the graphics card, the compositor and the log line. */
static void diag_second(uint64_t now)
{
    pstat = pstat_now;
    pstat_now = (present_stat){ 0 };
    astat = astat_now;
    astat_now = (audio_stat){ 0, 0, astat.ratio, 0, 0, 0 };
    if (g_set.show_fps == 2) {
        gpumon_get(&gstat);
        compositing = find_compositing();
    }
    diag_log(now);
}

/* ---------------- frame pacing ----------------
 * The Genesis runs at 59.92 Hz and most screens at 60 Hz. Paced by the clock,
 * the game shows one frame twice about every 12 s, and the scroll stutters
 * while the two clocks are nearly in step. Display sync runs one game frame
 * per screen refresh instead (the game 0.13% faster; the audio rate control
 * in pump_audio absorbs it). It needs VSync to really wait for the refresh:
 * the time between presents is watched, and the clock takes over when the
 * screen isn't near 60 Hz, VSync doesn't wait, or frames are missed. */
static void pacing_update(uint64_t now)
{
    static uint64_t last;
    static int early, late, good, cooldown;
    uint64_t dt = last ? now - last : 0;
    last = now;
    if (!dt) return;
    float hz = display_hz();
    int want = g_set.display_sync && g_set.vsync && (hz == 0.0f || near_60(hz));
    if (sync_on && !want) {                 /* setting, VSync or screen changed: the clock at once */
        sync_on = 0;
        SDL_Log("pacing: display sync off (setting, VSync or refresh rate)");
    }
    /* expected gap: a refresh while synced; with the clock, the whole number of
     * refreshes a game frame lands on when VSync waits, else a game frame */
    double refresh = hz > 0.0f ? 1e9 / hz : (double)FRAME_NS;
    double expect = sync_on ? refresh : g_set.vsync ? SDL_ceil(FRAME_NS / refresh - 0.05) * refresh : (double)FRAME_NS;
    int is_early = g_set.vsync && dt < refresh * 0.85, is_late = dt > expect * 1.15;
    pstat_now.sum_ms += dt / 1e6;
    pstat_now.n++;
    if (dt / 1e6 > pstat_now.max_ms) pstat_now.max_ms = dt / 1e6;
    pstat_now.early += is_early;
    pstat_now.late += is_late;
    early += is_early; late += is_late; good += !is_early && !is_late;

    if (sync_on && early >= 4) {            /* VSync stopped waiting: leave at once */
        sync_on = 0; cooldown = 300;
        SDL_Log("pacing: display sync off (presents not waiting for the refresh)");
    }
    if (early + late + good < 120) return;
    if (cooldown > 0) cooldown -= 120;
    if (sync_on && late > 12) {
        sync_on = 0; cooldown = 600;
        SDL_Log("pacing: display sync off (missed refreshes)");
    } else if (!sync_on && want && cooldown <= 0 && early == 0 && late <= 2) {
        sync_on = 1;
        SDL_Log("pacing: display sync on (%.2f Hz)", hz);
    }
    early = late = good = 0;
}

static void handle_event(const SDL_Event *e)
{
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && lab_key(e->key.scancode)) return;
    input_event(e);
    if (menu_is_open()) menu_event(e);
    switch (e->type) {
    case SDL_EVENT_QUIT: running = 0; break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.key == SDLK_RETURN && (e->key.mod & SDL_KMOD_ALT) && !e->key.repeat) toggle_fullscreen();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: focused = 0; apply_audio(); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: focused = 1; apply_audio(); break;
    case SDL_EVENT_WINDOW_RESIZED:
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) { g_set.window_w = e->window.data1; g_set.window_h = e->window.data2; }
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: load_modes(); break;
    default: break;
    }
}

typedef struct { long long t; char s[200]; } timed_t;
static const char *shot_dir;
static int opt_make_pack;
static int opt_extract, opt_extract_campaign = -1;   /* --extract [campaign] */                   /* --make-hd-pack: build the starter pack at start */
static int shot_every;

/* ---------------- game selection (first version of the collection launcher) ---------------- */
static const struct { const char *name, *sub; int r, g, b; } card_look[] = {
    { "DESERT STRIKE", "Return to the Gulf", 214, 160, 84 },
    { "JUNGLE STRIKE", "", 96, 168, 84 },
    { "URBAN STRIKE", "", 104, 140, 196 },
};

static int sel_row, sel_col;
static int pack_request;            /* Build HD Pack from the game picker: game index + 1 */

static int sel_dir(void)
{
    int d = 0, n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && !d; i++) {
        SDL_Gamepad *p = SDL_GetGamepadFromID(ids[i]);
        if (!p) continue;
        int x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -20000) d = 1;
        else if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 20000) d = 2;
        else if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -20000) d = 3;
        else if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 20000) d = 4;
    }
    SDL_free(ids);
    return d;
}

/* rows: 0 game cards, 1 Settings, 2 Quit */
static int sel_card;                         /* last focused card */
static void sel_move(int d)
{
    if (d == 1 || d == 2) {
        sel_row = (sel_row + (d == 2 ? 1 : -1) + 3) % 3;
        sel_col = sel_row ? 0 : sel_card;
        return;
    }
    if (sel_row) return;
    sel_col = (sel_col + (d == 4 ? 1 : -1) + g_game_count) % g_game_count;
    sel_card = sel_col;
}

typedef struct { float x, y, w, h; int row, col; } sel_hit;

/* ---------------- launcher background: live intros ----------------
 * A random game with a ROM runs its own intro behind the picker, live
 * through the recompiled code (nothing pre-recorded). It is fast-forwarded
 * past the publisher logos while hidden, shown for a while, then
 * crossfades to another game. The first fast-forward is kept as an
 * in-memory snapshot so later visits start instantly. */
#define ATT_SHOW_NS  32000000000ull       /* time on screen */
#define ATT_FADE_NS    900000000ull
static struct {
    int cur, last;                         /* game index, -1 = none */
    int phase;                             /* 0 pick, 1 warm up (hidden), 2 showing, 3 fading out */
    uint64_t t0, next;
    float alpha;
    uint8_t *snap[MAX_GAMES];
} att = { -1, -1 };
/* first frame of each game's title sequence (after the publisher logos) */
static uint32_t att_start_frame(const game_module *g)
{
    if (!SDL_strcmp(g->id, "desert")) return 360;
    if (!SDL_strcmp(g->id, "jungle")) return 600;
    if (!SDL_strcmp(g->id, "urban")) return 1560;
    return 300;
}

static int att_boot(int gi)
{
    size_t n = 0;
    void *rom = SDL_LoadFile(rom_path_of[gi], &n);
    if (!rom) return 0;
    int ok = gen_set_rom(rom, (uint32_t)n) && game_identify_rom() == g_games[gi];
    SDL_free(rom);
    if (!ok) return 0;
    att_on = 1;
    g_ws_ext = g_ws_win = 0;
    game_boot(g_games[gi]);
    if (att.snap[gi] && !cpu_state_load(att.snap[gi])) { SDL_free(att.snap[gi]); att.snap[gi] = NULL; game_boot(g_games[gi]); }
    return 1;
}

static void att_volume(void)
{
    if (aud) SDL_SetAudioStreamGain(aud, (!focused && g_set.mute_unfocused) ? 0.0f : g_set.volume / 100.0f * 0.45f * att.alpha);
}

/* Advance the background by real time. */
static void att_update(void)
{
    uint64_t now = SDL_GetTicksNS();
    if (att.phase == 0) {
        int cand[MAX_GAMES], nc = 0;
        for (int i = 0; i < g_game_count; i++) if (rom_path_of[i][0] && i != att.last) cand[nc++] = i;
        if (!nc && att.last >= 0 && rom_path_of[att.last][0]) cand[nc++] = att.last;
        if (!nc) { att.cur = -1; return; }
        int gi = cand[SDL_rand(nc)];
        if (!att_boot(gi)) { rom_path_of[gi][0] = 0; return; }
        att.cur = gi; att.phase = 1; att.alpha = 0;
    }
    if (att.phase == 1) {
        uint32_t start = att_start_frame(g_games[att.cur]);
        for (int k = 0; k < 40 && g_frame < start; k++) { cpu_run_frame(); pump_audio(0); }
        if (g_frame >= start) {
            if (!att.snap[att.cur]) {
                att.snap[att.cur] = SDL_malloc(cpu_state_size());
                if (att.snap[att.cur]) cpu_state_save(att.snap[att.cur]);
            }
            if (aud) SDL_ClearAudioStream(aud);
            att.phase = 2; att.t0 = att.next = now;
        }
        return;
    }
    /* showing / fading: run in real time */
    if (now > att.next && now - att.next > 250000000ull) att.next = now;
    int ran = 0;
    while (now >= att.next && ran < 4) { cpu_run_frame(); pump_audio(1); att.next += FRAME_NS; ran++; }
    uint64_t t = now - att.t0;
    if (att.phase == 2) {
        att.alpha = t < ATT_FADE_NS ? (float)t / ATT_FADE_NS : 1.0f;
        if (t > ATT_SHOW_NS) { att.phase = 3; att.t0 = now; }
    } else {
        att.alpha = t < ATT_FADE_NS ? 1.0f - (float)t / ATT_FADE_NS : 0.0f;
        if (t >= ATT_FADE_NS) { att.last = att.cur; att.phase = 0; att.alpha = 0; if (aud) SDL_ClearAudioStream(aud); }
    }
    att_volume();
}

/* Leave the launcher: the chosen game boots fresh. */
static void att_stop(void)
{
    att_on = 0;
    att.phase = 0;
    att.alpha = 0;
    music_stop();
    if (aud) SDL_ClearAudioStream(aud);
}

static void draw_band(float y, float h, int a_top, int a_bot)
{
    int steps = 24;
    for (int i = 0; i < steps; i++) {
        int a = a_top + (a_bot - a_top) * i / (steps - 1);
        ui_rect(-2000, y + h * i / steps, 5280, h / steps + 1, ui_rgba(6, 8, 5, a));
    }
}

static void sel_draw(sel_hit *hits, int *nh)
{
    int ow, oh;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    SDL_SetRenderDrawColor(ren, 8, 9, 7, 255);
    SDL_RenderClear(ren);
    float t = SDL_GetTicks() / 1000.0f;

    /* live intro, 4:3 in the middle of the window */
    if (att.cur >= 0 && att.phase >= 2 && att.alpha > 0) {
        SDL_UpdateTexture(tex, &(SDL_Rect){ 0, 0, fb_w, VDP_H }, fb, VDP_MAX_W * 4);
        /* 4:3 like the console, letterboxed/pillarboxed (not zoomed to fill) */
        double ia = 4.0 / 3.0, wa = (double)ow / oh, dw, dh;
        if (wa > ia) { dh = oh; dw = oh * ia; } else { dw = ow; dh = ow / ia; }
        SDL_FRect src = { 0, 0, (float)fb_w, VDP_H }, dst = { (float)((ow - dw) / 2), (float)((oh - dh) / 2), (float)dw, (float)dh };
        SDL_SetTextureScaleMode(tex, dh >= VDP_H * 2 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
        SDL_SetTextureAlphaModFloat(tex, att.alpha);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_RenderTexture(ren, tex, &src, &dst);
        SDL_SetTextureAlphaModFloat(tex, 1.0f);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    }
    ui_begin(ow, oh);
    ui_rect(-2000, -2000, 5280, 4720, ui_rgba(6, 8, 5, 70));          /* overall dim */
    for (int y = -400; y < 1120; y += 4) ui_rect(-2000, (float)y, 5280, 1.5f, ui_rgba(0, 0, 0, 40));   /* scanlines */
    draw_band(-400, 400, 235, 235);
    draw_band(0, 230, 235, 0);                                         /* title band */
    draw_band(380, 340, 0, 235);                                       /* card band */
    ui_rect(-2000, 720, 5280, 400, ui_rgba(6, 8, 5, 235));

    /* title: "STRIKE" with a sweeping shine, subtitle with an amber rule */
    float intro = SDL_min(1.0f, t / 1.2f);
    SDL_FColor tc = ui_rgba(232, 226, 210, (int)(255 * intro));
    ui_text_center(UI_TITLE, 640, 34 - (1 - intro) * 20, tc, "STRIKE");
    float tw = ui_text_w(UI_TITLE, "STRIKE");
    if (icon_tex) {
        SDL_SetTextureAlphaModFloat(icon_tex, intro);
        ui_image(icon_tex, 640 - tw / 2 - 86, 30 - (1 - intro) * 20, 72, 72);
        SDL_SetTextureAlphaModFloat(icon_tex, 1.0f);
    }
    float sx = 640 - tw / 2 + SDL_fmodf(t * 260.0f, tw + 400) - 200;
    for (int i = 0; i < 6; i++)
        ui_rect(sx + i * 6, 40, 4, 58, ui_rgba(255, 236, 190, 18 - i * 3));
    float rw = 520 * intro;
    ui_rect(640 - rw / 2, 108, rw, 2, ui_rgba(242, 178, 58, 230));
    ui_triangle(640 - rw / 2 - 10, 109, 640 - rw / 2, 105, 640 - rw / 2, 113, ui_rgba(242, 178, 58, 230));
    ui_triangle(640 + rw / 2 + 10, 109, 640 + rw / 2, 105, 640 + rw / 2, 113, ui_rgba(242, 178, 58, 230));
    ui_text_center(UI_BODY, 640, 120, ui_rgba(196, 164, 98, (int)(255 * intro)), "R E M A S T E R E D   C O L L E C T I O N   H D");

    /* cards */
    *nh = 0;
    float cw = 330, ch = 168, gap = 30, x0 = 640 - (g_game_count * cw + (g_game_count - 1) * gap) / 2;
    for (int i = 0; i < g_game_count; i++) {
        const game_module *g = g_games[i];
        int f = sel_row == 0 && sel_col == i, have = rom_path_of[i][0] != 0;
        float pulse = f ? 0.5f + 0.5f * SDL_sinf(t * 4.0f) : 0;
        float x = x0 + i * (cw + gap), y = f ? 400.0f : 412.0f;
        SDL_FColor acc = ui_rgba(card_look[i].r, card_look[i].g, card_look[i].b, 255);
        ui_panel(x, y, cw, ch, 20, ui_rgba(16, 19, 13, f ? 236 : 210),
                 f ? ui_rgba(242, 178, 58, 160 + (int)(95 * pulse)) : ui_rgba(196, 164, 98, 70));
        ui_rect(x + 20, y + 8, cw - 40, 5, have ? acc : ui_rgba(80, 80, 72, 255));
        ui_text_center(UI_LARGE, x + cw / 2, y + 30, have ? ui_rgba(232, 226, 210, 255) : ui_rgba(130, 128, 116, 255), card_look[i].name);
        char line[64];
        if (card_look[i].sub[0]) SDL_snprintf(line, sizeof line, "%s  -  %d", card_look[i].sub, g->year);
        else SDL_snprintf(line, sizeof line, "%d", g->year);
        ui_text_center(UI_SMALL, x + cw / 2, y + 74, ui_rgba(170, 166, 150, 255), line);
        ui_text_center(UI_BODY, x + cw / 2, y + 108, have ? ui_rgba(150, 220, 120, 255) : ui_rgba(230, 150, 90, 255),
                       have ? (f ? "PRESS TO PLAY" : "READY") : "ROM NOT FOUND");
        if (!have && f) ui_text_center(UI_SMALL, x + cw / 2, y + 138, ui_rgba(150, 148, 132, 255), "Select to add your ROM");
        if (have) {                              /* career line */
            const game_stats *gs = stats_for(g->id);
            if (gs->play_seconds >= 1) {
                char tm[32], st[96];
                fmt_time(tm, sizeof tm, gs->play_seconds);
                SDL_snprintf(st, sizeof st, "%s flown  -  campaign %d  -  %d objective%s", tm, gs->best_campaign ? gs->best_campaign : 1, gs->objectives, gs->objectives == 1 ? "" : "s");
                ui_text_center(UI_SMALL, x + cw / 2, y + 138, ui_rgba(150, 148, 132, 255), st);
            }
        }
        if (att.cur == i && att.phase >= 2) {
            float a = att.alpha;
            ui_rect(x + cw - 30, y + ch - 22, 10, 10, ui_rgba(214, 74, 52, (int)(255 * a * (0.6f + 0.4f * SDL_sinf(t * 6)))));
        }
        hits[(*nh)++] = (sel_hit){ x, y, cw, ch, 0, i };
    }
    const char *btn[2] = { "Settings", "Quit" };
    for (int i = 0; i < 2; i++) {
        float bw = 220, bh = 38, bx = 640 - bw / 2, by = 592 + i * 44;
        int f = sel_row == 1 + i;
        ui_rect(bx, by, bw, bh, f ? ui_rgba(242, 178, 58, 46) : ui_rgba(16, 19, 13, 220));
        ui_frame(bx, by, bw, bh, 2, f ? ui_rgba(242, 178, 58, 255) : ui_rgba(196, 164, 98, 70));
        ui_text_center(UI_BODY, bx + bw / 2, by + 5, f ? ui_rgba(242, 178, 58, 255) : ui_rgba(232, 226, 210, 255), btn[i]);
        hits[(*nh)++] = (sel_hit){ bx, by, bw, bh, 1 + i, 0 };
    }
    char pr[160];
    if (input_pad_connected())
        SDL_snprintf(pr, sizeof pr, "%s  Select      %s  Quit", input_pad_confirm_label(), input_pad_back_label());
    else
        SDL_snprintf(pr, sizeof pr, "Arrows  Move      Enter  Select      Esc  Quit");
    ui_text_right(UI_SMALL, 1220, 684, ui_rgba(150, 148, 132, 255), pr);
    if (att.cur >= 0 && att.phase >= 2) {
        char now[96];
        SDL_snprintf(now, sizeof now, "NOW SHOWING  %s", card_look[att.cur].name);
        ui_text(UI_SMALL, 60, 684, ui_rgba(196, 164, 98, (int)(220 * att.alpha)), now);
    }
    if (osd[0] && SDL_GetTicksNS() < osd_until) ui_text(UI_SMALL, 60, 650, ui_rgba(242, 178, 58, 255), osd);
    if (menu_is_open()) menu_draw(ow, oh);
}

/* Returns 1 to start game sel_col, 0 if nothing yet, -1 to quit. */
static int sel_activate(void)
{
    if (sel_row == 2) return -1;
    if (sel_row == 1) { menu_open_launcher(); return 0; }
    if (!rom_path_of[sel_col][0]) {
        const game_module *g = add_rom_dialog();
        if (g) { char m[96]; SDL_snprintf(m, sizeof m, "%s added", g->title); say(m); sel_col = game_index(g); }
        return 0;
    }
    return 1;
}

static long long max_ticks, tick;
static long long rw_test_at = -1, rw_test_n;     /* --rewind-at T:N (test) */
static struct { long long t; uint32_t a; uint16_t v; } pokes[16];   /* --poke T:addr:value (test, RAM word) */
static int npokes;
static timed_t keys[64], shots[64];
static int nkeys, nshots;

static void inject_test_input(void)
{
    if (!max_ticks) return;
    for (int i = 0; i < nkeys; i++) if (keys[i].t == tick) {
        SDL_Event k = { 0 };
        k.type = SDL_EVENT_KEY_DOWN;
        k.key.scancode = SDL_GetScancodeFromName(keys[i].s);
        k.key.key = SDL_GetKeyFromScancode(k.key.scancode, 0, false);
        k.key.down = true;
        SDL_PushEvent(&k);
        k.type = SDL_EVENT_KEY_UP; k.key.down = false;
        SDL_PushEvent(&k);
    }
}

static void after_present(void)
{
    for (int i = 0; i < nshots; i++) if (shots[i].t == tick) save_screen(shots[i].s);
    SDL_RenderPresent(ren);
    tick++;
    if (max_ticks && tick >= max_ticks) running = 0;
}

static int run_selector(void)
{
    SDL_SetWindowTitle(win, APP_TITLE);
    static sel_hit hits[16];
    int nh = 0, held = 0;
    uint64_t held_t = 0, rep_t = 0;
    while (running) {
        inject_test_input();
        SDL_Event e;
        int act = 0;
        while (SDL_PollEvent(&e)) {
            input_event(&e);
            if (menu_is_open()) {                /* Settings opened from the picker */
                if (e.type == SDL_EVENT_QUIT) running = 0;
                menu_event(&e);
                continue;
            }
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.scancode) {
                case SDL_SCANCODE_UP: case SDL_SCANCODE_W: sel_move(1); break;
                case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: sel_move(2); break;
                case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: sel_move(3); break;
                case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: sel_move(4); break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (!e.key.repeat) act = 1; break;
                case SDL_SCANCODE_ESCAPE: if (!e.key.repeat) act = -1; break;
                default: break;
                }
                if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) { act = 0; toggle_fullscreen(); }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) act = 1;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) act = -1;
                break;
            case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                float vx, vy;
                ui_to_virtual(e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x,
                              e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y, &vx, &vy);
                for (int i = 0; i < nh; i++)
                    if (vx >= hits[i].x && vy >= hits[i].y && vx < hits[i].x + hits[i].w && vy < hits[i].y + hits[i].h) {
                        sel_row = hits[i].row; sel_col = hits[i].col;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) act = 1;
                    }
                break;
            }
            default: break;
            }
        }
        if (menu_is_open()) { menu_update(); input_clear_pressed(); act = 0; }
        if (pack_request) { att_stop(); return pack_request - 1; }
        int d = menu_is_open() ? 0 : sel_dir();
        uint64_t now = SDL_GetTicksNS();
        if (d != held) { held = d; held_t = rep_t = now; if (d) sel_move(d); }
        else if (d && now - held_t > 350000000ull && now - rep_t > 120000000ull) { rep_t = now; sel_move(d); }
        if (act == -1) { att_stop(); return -1; }
        if (act == 1) {
            int r = sel_activate();
            if (r != 0) { att_stop(); return r < 0 ? -1 : sel_col; }
        }
        if (!running) { att_stop(); return -1; }
        if (g_set.launcher_intros) att_update();
        sel_draw(hits, &nh);
        after_present();
        SDL_Delay(max_ticks ? 0 : 4);
    }
    att_stop();
    return -1;
}

/* ---------------- Continue / Load Game / New Game ----------------
 * Picking a game that has saves asks how to start, like a modern game.
 * Continue loads the state written when the game was last left mid-play,
 * or else the newest save. Returns 1 continue, 2 load, 0 new, -1 back. */
static int newest_state(SDL_Time *when)
{
    int best = -1;
    SDL_Time bt = 0;
    for (int slot = 0; slot <= RESUME_SLOT; slot++) {
        char path[1024];
        state_path(path, sizeof path, slot);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(path, &pi) || pi.type != SDL_PATHTYPE_FILE) continue;
        SDL_Time t = pi.modify_time + (slot == RESUME_SLOT ? 1 : 0);   /* a tie goes to the resume point */
        if (best < 0 || t > bt) { best = slot; bt = t; }
    }
    if (when) *when = bt;
    return best;
}

/* New Game: the difficulty, Remastered first (docs/14-difficulty.md).
 * Returns 0 to go back. Original also brings back the console's slowdown. */
static int pick_difficulty(void)
{
    if (use_script || (max_ticks && !SDL_getenv("DS_PROMPT")) || SDL_strcmp(game->id, "desert")) return 1;
    static const char *name[3] = { "Recruit", "Remastered", "Original" };
    static const char *line1[3] = {
        "New to the Strike games? Enemy fire is much gentler,",
        "Full speed and full chaos, tuned to be tough but fair:",
        "Exactly as the Genesis shipped it: the original damage",
    };
    static const char *line2[3] = {
        "and a crossfire wears you down slowly.",
        "bullets sting less, and a crossfire can't kill you in an instant.",
        "and the console's slowdown in busy fights.",
    };
    static const int order[3] = { 1, 0, 2 };         /* Remastered on top */
    int sel = 0, result = -2;
    for (int i = 0; i < 3; i++) if (order[i] == g_set.difficulty) sel = i;
    while (running && result == -2) {
        inject_test_input();
        int ow, oh;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        float bx = 640 - 330, by = 190, bw = 660, bh = 110;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input_event(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat && e.key.scancode != SDL_SCANCODE_UP && e.key.scancode != SDL_SCANCODE_DOWN) break;
                switch (e.key.scancode) {
                case SDL_SCANCODE_UP: case SDL_SCANCODE_W: sel = (sel + 2) % 3; break;
                case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: sel = (sel + 1) % 3; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: result = sel; break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: result = -1; break;
                default: break;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) sel = (sel + 2) % 3;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) sel = (sel + 1) % 3;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) result = sel;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) result = -1;
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                static int stick;
                int d = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? (e.gaxis.value > 16000) - (e.gaxis.value < -16000) : stick;
                if (d && d != stick) sel = (sel + (d > 0 ? 1 : 2)) % 3;
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) stick = d;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                float vx, vy;
                ui_to_virtual(e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x,
                              e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y, &vx, &vy);
                for (int i = 0; i < 3; i++)
                    if (vx >= bx && vx < bx + bw && vy >= by + i * (bh + 16) && vy < by + i * (bh + 16) + bh) {
                        sel = i;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) result = i;
                    }
                break;
            }
            default: break;
            }
        }
        SDL_SetRenderDrawColor(ren, 8, 9, 7, 255);
        SDL_RenderClear(ren);
        ui_begin(ow, oh);
        SDL_FColor gold = ui_rgba(196, 164, 98, 255), text = ui_rgba(232, 226, 210, 255), dim = ui_rgba(150, 148, 132, 255);
        ui_text_center(UI_TITLE, 640, 60, gold, game->title);
        ui_text_center(UI_BODY, 640, 130, dim, "Choose your difficulty. You can change it any time in Settings > System.");
        for (int i = 0; i < 3; i++) {
            int d = order[i], on = i == sel;
            float y = by + i * (bh + 16);
            ui_panel(bx, y, bw, bh, 8, on ? ui_rgba(60, 52, 30, 230) : ui_rgba(20, 22, 17, 220), on ? gold : ui_rgba(196, 164, 98, 90));
            char t[64];
            SDL_snprintf(t, sizeof t, "%s%s", name[d], d == 1 ? "  (recommended)" : "");
            ui_text_center(UI_LARGE, 640, y + 10, on ? ui_rgba(255, 214, 72, 255) : text, t);
            ui_text_center(UI_SMALL, 640, y + 56, on ? text : dim, line1[d]);
            ui_text_center(UI_SMALL, 640, y + 80, on ? text : dim, line2[d]);
        }
        ui_text_right(UI_SMALL, 1220, 684, dim, input_pad_connected() ? "A  Select      B  Back" : "Enter  Select      Esc  Back");
        after_present();
        SDL_Delay(8);
    }
    input_clear_pressed();
    if (result < 0) return 0;
    g_set.difficulty = order[result];
    g_set.authentic_speed = g_set.difficulty == 2;
    apply_settings();
    settings_save(settings_path);
    return 1;
}

static int continue_prompt(int *slot_out)
{
    SDL_Time when = 0;
    int slot = newest_state(&when);
    if (slot < 0 || !g_set.continue_prompt || use_script || (max_ticks && !SDL_getenv("DS_PROMPT"))) return 0;
    *slot_out = slot;
    SDL_Texture *thumb = NULL;
    char path[1024];
    state_path(path, sizeof path, slot);
    size_t n = 0;
    uint8_t *b = SDL_LoadFile(path, &n);
    if (b && n >= ST_HDR && !memcmp(b, ST_MAGIC, 8)) {
        thumb = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, THUMB_W, THUMB_H);
        if (thumb) { SDL_UpdateTexture(thumb, 0, b + 24, THUMB_W * 4); SDL_SetTextureScaleMode(thumb, SDL_SCALEMODE_LINEAR); }
    }
    SDL_free(b);
    char whens[64] = "";
    SDL_DateTime dt;
    if (when && SDL_TimeToDateTime(when, &dt, true)) {
        static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        SDL_snprintf(whens, sizeof whens, "%s  %d %s %04d  %02d:%02d", slot == RESUME_SLOT ? "Where you left off" : slot >= MENU_SAVE_SLOTS ? "Latest autosave" : "Latest save",
                     dt.day, mon[(dt.month - 1) % 12], dt.year, dt.hour, dt.minute);
    }
    static const char *opt[3] = { "Continue", "Load Game", "New Game" };
    int sel = 0, result = -2;
    SDL_SetWindowTitle(win, game->title);
    while (running && result == -2) {
        inject_test_input();
        int ow, oh;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        float bx = 640 - 170, by = 480, bw = 340, bh = 52;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input_event(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat && e.key.scancode != SDL_SCANCODE_UP && e.key.scancode != SDL_SCANCODE_DOWN) break;
                switch (e.key.scancode) {
                case SDL_SCANCODE_UP: case SDL_SCANCODE_W: sel = (sel + 2) % 3; break;
                case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: sel = (sel + 1) % 3; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: result = sel; break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: result = -1; break;
                default: break;
                }
                if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) { result = -2; toggle_fullscreen(); }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) sel = (sel + 2) % 3;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) sel = (sel + 1) % 3;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) result = sel;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) result = -1;
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                static int stick;
                int d = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? (e.gaxis.value > 16000) - (e.gaxis.value < -16000) : stick;
                if (d && d != stick) sel = (sel + (d > 0 ? 1 : 2)) % 3;
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) stick = d;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                float vx, vy;
                ui_to_virtual(e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x,
                              e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y, &vx, &vy);
                for (int i = 0; i < 3; i++)
                    if (vx >= bx && vx < bx + bw && vy >= by + i * (bh + 10) && vy < by + i * (bh + 10) + bh) {
                        sel = i;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) result = i;
                    }
                break;
            }
            default: break;
            }
        }
        SDL_SetRenderDrawColor(ren, 8, 9, 7, 255);
        SDL_RenderClear(ren);
        ui_begin(ow, oh);
        SDL_FColor gold = ui_rgba(196, 164, 98, 255), text = ui_rgba(232, 226, 210, 255), dim = ui_rgba(150, 148, 132, 255);
        ui_text_center(UI_TITLE, 640, 60, gold, game->title);
        if (thumb) {
            float tw = 400, th = tw * THUMB_H / THUMB_W;
            SDL_FRect dst = { 0 };
            ui_frame(640 - tw / 2 - 3, 130 - 3, tw + 6, th + 6, 2, ui_rgba(196, 164, 98, 160));
            ui_virtual_rect(640 - tw / 2, 130, tw, th, &dst);
            SDL_RenderTexture(ren, thumb, NULL, &dst);
            ui_text_center(UI_SMALL, 640, 130 + th + 12, dim, whens);
        } else ui_text_center(UI_SMALL, 640, 300, dim, whens);
        for (int i = 0; i < 3; i++) {
            float y = by + i * (bh + 10);
            int on = i == sel;
            ui_panel(bx, y, bw, bh, 8, on ? ui_rgba(60, 52, 30, 230) : ui_rgba(20, 22, 17, 220), on ? gold : ui_rgba(196, 164, 98, 90));
            ui_text_center(UI_LARGE, 640, y + 10, on ? ui_rgba(255, 214, 72, 255) : text, opt[i]);
        }
        ui_text_right(UI_SMALL, 1220, 684, dim, input_pad_connected() ? "A  Select      B  Back" : "Enter  Select      Esc  Back");
        after_present();
        SDL_Delay(8);
    }
    if (thumb) SDL_DestroyTexture(thumb);
    input_clear_pressed();
    static const int code[3] = { 1, 2, 0 };          /* Continue, Load Game, New Game */
    return result < 0 ? -1 : code[result];
}

/* ---------------- Amiga intro ----------------
 * When Desert Strike itself starts (never in the game picker): the Amiga
 * title photographs, slowly pushed in and cross-faded, over the Amiga title
 * song, which carries on into the Genesis title. Any button skips. */
static void feed_music_only(void)
{
    if (!aud) return;
    static int16_t buf[1024 * 2];
    while (SDL_GetAudioStreamQueued(aud) < 4 * AUDIO_RATE / 15) {
        memset(buf, 0, sizeof buf);
        mod_mix(buf, 1024);
        SDL_PutAudioStreamData(aud, buf, sizeof buf);
    }
}

static void amiga_intro(void)
{
    int np = amiga_picture_count();
    if (!g_set.amiga_intro || !game || SDL_strcmp(game->id, "desert") || np < 1 || use_script) return;
    if (max_ticks && !SDL_getenv("DS_INTRO")) return;          /* tests: only when asked */
    if (np > 3) np = 3;
    SDL_Texture *tex[3] = { 0 };
    int cw[3], ch[3];
    for (int i = 0; i < np; i++) {
        const amiga_picture *p = amiga_picture_at(i);
        cw[i] = SDL_min(p->w, 352);                   /* past 352 px is off-screen overscan */
        ch[i] = SDL_min(p->h, 224);
        tex[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, p->w, p->h);
        if (!tex[i]) { np = i; break; }
        SDL_UpdateTexture(tex[i], NULL, p->rgba, p->w * 4);
        SDL_SetTextureScaleMode(tex[i], SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(tex[i], SDL_BLENDMODE_BLEND);
    }
    if (g_set.amiga_music) music_play(105);           /* the title song; the Genesis title keeps it */
    const float fade = 90, hold = 330, step = hold + fade;  /* frames at 60 Hz */
    float total = np * step + fade, t = 0, skip_at = -1;
    uint64_t last = SDL_GetTicksNS();
    SDL_SetWindowTitle(win, game->title);
    while (running && t < total && (skip_at < 0 || t < skip_at + 30)) {
        inject_test_input();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !(e.key.mod & SDL_KMOD_ALT)) ||
                     e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (skip_at < 0) skip_at = t;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) toggle_fullscreen();
        }
        int ow, oh;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        float out = skip_at >= 0 ? 1 - (t - skip_at) / 30 : 1;
        for (int i = 0; i < np; i++) {
            float lt = t - i * step;                  /* this picture's own time */
            if (lt < 0 || lt > step + fade) continue;
            float a = lt < fade ? lt / fade : lt > step ? 1 - (lt - step) / fade : 1;
            if (i == np - 1 && lt > step) a = 1 - (lt - step) / fade;
            a *= out;
            if (a <= 0) continue;
            /* a slow push-in, alternately towards the left and the right */
            float z = 1.0f + 0.10f * (lt / (step + fade));
            float sw = cw[i] / z, sh = ch[i] / z;
            float sx = (cw[i] - sw) * (i & 1 ? 0.2f : 0.8f), sy = (ch[i] - sh) * 0.5f;
            SDL_FRect src = { sx, sy, sw, sh };
            float scale = SDL_min((float)ow / cw[i], (float)oh / ch[i]);
            SDL_FRect dst = { (ow - cw[i] * scale) / 2, (oh - ch[i] * scale) / 2, cw[i] * scale, ch[i] * scale };
            SDL_SetTextureAlphaModFloat(tex[i], a);
            SDL_RenderTexture(ren, tex[i], &src, &dst);
        }
        ui_begin(ow, oh);
        if (t > 120 && skip_at < 0) ui_text_right(UI_SMALL, 1240, 690, ui_rgba(150, 148, 132, (int)(140 * SDL_min(1.0f, (t - 120) / 60))), "Any button: skip");
        after_present();
        feed_music_only();
        if (max_ticks) t += 1;
        else {
            uint64_t now = SDL_GetTicksNS();
            t += SDL_min(6.0f, (now - last) / 16666667.0f);
            last = now;
            SDL_Delay(4);
        }
    }
    for (int i = 0; i < np; i++) if (tex[i]) SDL_DestroyTexture(tex[i]);
    input_clear_pressed();
}

static menu_host mh;
static int switch_requested;
static void switch_game(void) { switch_requested = 1; }

/* HD packs for the running game: <settings>/packs/<id>/ and packs/<id>/
 * next to the program. The dumper writes to <settings>/hd_dump/<id>/. */
static void load_hd_pack(void)
{
    hdpack_unload();
    char dir[1100];
    int n = 0;
    SDL_snprintf(dir, sizeof dir, "%spacks/%s/", pref, game->id);
    n += hdpack_load(dir);
    const char *base = SDL_GetBasePath();
    if (base) {
        SDL_snprintf(dir, sizeof dir, "%spacks/%s/", base, game->id);
        n += hdpack_load(dir);
    }
    SDL_snprintf(dir, sizeof dir, "%shd_dump", pref);
    SDL_CreateDirectory(dir);
    SDL_snprintf(dump_dir, sizeof dump_dir, "%shd_dump/%s/", pref, game->id);
    if (n) {
        if (!hdfb) hdfb = SDL_calloc((size_t)VDP_MAX_W * 4 * VDP_H * 4, 4);
        if (!hdtex) {
            hdtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, VDP_MAX_W * 4, VDP_H * 4);
            if (!hdtex) hdtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VDP_MAX_W * 4, VDP_H * 4);
        }
        char m[96];
        SDL_snprintf(m, sizeof m, "HD pack: %d graphics at %dx", n, hdpack_scale());
        say(m);
    }
}

static int extract_game(int gi, int only);
static void extract_begin(settings_t *saved);
static void extract_end(const settings_t *saved);
static int upscale_dump(const char *dump, const char *pack);

/* Settings > System > Build HD Pack: extract every graphic of the current
 * game (all campaigns, headless), upscale it, and load the pack. The game
 * restarts afterwards. */
static void make_hd_pack(void)
{
    if (!game) {                    /* from the game picker's settings: start the highlighted game and build */
        if (sel_col >= 0 && sel_col < g_game_count && rom_path_of[sel_col][0]) { pack_request = sel_col + 1; menu_close(); }
        else say("Add this game's ROM first, then use Build HD Pack");
        return;
    }
    int gi = game_index(game);
    menu_close();
    settings_t saved;
    extract_begin(&saved);
    int ok = extract_game(gi, -1);
    extract_end(&saved);
    char dir[1100];
    SDL_snprintf(dir, sizeof dir, "%spacks", pref);
    SDL_CreateDirectory(dir);
    SDL_snprintf(dir, sizeof dir, "%spacks/%s/", pref, game->id);
    if (ok) ok = upscale_dump(dump_dir, dir);
    g_start_campaign = 0;
    game_boot(game);
    load_hd_pack();
    if (aud) SDL_ClearAudioStream(aud);
    say(ok == 2 ? "HD pack built with xBRZ: the AI upscaler needs a Vulkan graphics card"
               : ok ? "HD pack built" : "HD pack build cancelled");
}

/*
 * Graphics extractor (--extract): for every game with a ROM, dump the intro
 * and title, then start each campaign and tour its whole map with the
 * player kept alive, dumping every new graphic to hd_dump/<game>/. Runs
 * headless as fast as possible. See docs/09-hd-packs.md.
 */
static int start_game(int gi);
static int game_campaigns(const game_module *g) { return g->campaigns > 0 ? g->campaigns : 1; }


/* Progress screen for long jobs; returns 0 if the player asked to cancel. */
static int job_progress(const char *title, const char *line, float frac)
{
    SDL_Event e;
    int cancel = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) { running = 0; cancel = 1; }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) cancel = 1;
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) cancel = 1;
    }
    if (max_ticks) return !cancel;                   /* headless: no drawing */
    int ow, oh;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    SDL_SetRenderDrawColor(ren, 8, 9, 7, 255);
    SDL_RenderClear(ren);
    ui_begin(ow, oh);
    ui_panel(340, 250, 600, 200, 18, ui_rgba(16, 19, 13, 240), ui_rgba(196, 164, 98, 160));
    ui_text_center(UI_LARGE, 640, 272, ui_rgba(232, 226, 210, 255), title);
    ui_text_center(UI_BODY, 640, 326, ui_rgba(196, 164, 98, 255), line);
    ui_rect(380, 372, 520, 14, ui_rgba(40, 46, 32, 230));
    ui_rect(380, 372, 520 * SDL_clamp(frac, 0.0f, 1.0f), 14, ui_rgba(118, 150, 64, 255));
    ui_text_center(UI_SMALL, 640, 404, ui_rgba(150, 148, 132, 255), "Esc / B to cancel");
    SDL_RenderPresent(ren);
    return !cancel;
}

/* Extract one game: intro and title, then every campaign (or just `only`).
 * Returns 0 if cancelled. */
static int extract_game(int gi, int only)
{
    const game_module *g = g_games[gi];
    int nc = only >= 0 ? 1 : game_campaigns(g);
    int total = only >= 0 ? 1 : nc + 1, step = 0;
    for (int ci = only >= 0 ? 0 : -1; ci < nc; ci++, step++) {   /* -1: intro and title, no input */
        int c = only >= 0 ? only : ci;
        g_start_campaign = c < 0 ? 0 : c;
        if (!start_game(gi)) return 1;
        apply_settings();
        if (g->tour) g->tour(1, 0);
        uint64_t f0 = g_frame, toured = 0;
        int state = 0, limit = c < 0 ? 7000 : 60000;
        char line[128];
        if (c < 0) SDL_snprintf(line, sizeof line, "%s: intro and menus", g->title);
        else SDL_snprintf(line, sizeof line, "%s: campaign %d of %d", g->title, c + 1, nc);
        SDL_Log("extract %s", line);
        for (int k = 0; k < limit; k++) {
            unsigned pad = 0;
            if (c >= 0) {
                if (state == 0) {
                    /* get through title, menus and briefings */
                    int t = k % 240, alt = (k / 240) % 4 == 3;
                    if (t < 8) pad = alt ? GB_C : GB_START;
                } else if (toured < 120) pad = GB_UP;   /* lift off */
                state = g->tour ? g->tour(0, hud_ok) : 2;
                if (state) toured++;
                if (state == 2) break;
            }
            pad_now = pad;
            cpu_run_frame();
            pump_audio(0);
            if (shot_every && g_frame % shot_every == 0) save_bmp(shot_dir);
            if (state == 1 && ws_planes_ok && (k & 7) == 0) {
                hdpack_dump_plane(dump_dir, &ws_planes[0]);
                hdpack_dump_plane(dump_dir, &ws_planes[1]);
            }
            if ((k & 255) == 0) {
                float within = c < 0 ? (float)k / limit : (state ? 0.15f + 0.85f * (float)toured / 18000.0f : 0.1f);
                if (!job_progress("Extracting graphics", line, (step + SDL_min(within, 0.99f)) / total)) return 0;
            }
        }
        SDL_Log("  %s: %llu frames, tour %s", line, (unsigned long long)(g_frame - f0),
                state == 2 ? "complete" : state ? "incomplete" : "never reached the map");
    }
    return 1;
}

static void extract_begin(settings_t *saved)
{
    *saved = g_set;
    g_set.hd_dump = 1;
    g_set.hd_graphics = 0;
    g_set.widescreen = WS_32_9;
    g_set.sprite_flicker = 0;
    g_set.status_hud = 0;
    use_script = 0;
    extracting = 1;
}

static void extract_end(const settings_t *saved)
{
    extracting = 0;
    g_set = *saved;
    apply_settings();
}

/* --extract [campaign]: every game with a ROM, headless. */
static void run_extract(void)
{
    settings_t saved;
    extract_begin(&saved);
    for (int gi = 0; gi < g_game_count && running; gi++)
        if (rom_path_of[gi][0] && !extract_game(gi, opt_extract_campaign)) break;
    extract_end(&saved);
}

/* Upscale the dump with the xBRZ tool next to the program (a separate,
 * GPL-licensed executable), or the built-in Scale2x if it is missing. */
static int upscale_dump(const char *dump, const char *pack)
{
    const char *base = SDL_GetBasePath();
    char exe[1100];
#ifdef _WIN32
    SDL_snprintf(exe, sizeof exe, "%shdpack_xbrz.exe", base ? base : "");
#else
    SDL_snprintf(exe, sizeof exe, "%shdpack_xbrz", base ? base : "");
#endif
    SDL_PathInfo info;
    if (SDL_GetPathInfo(exe, &info) && info.type == SDL_PATHTYPE_FILE) {
        /* AI style: Real-ESRGAN (ncnn/Vulkan) in <exe dir>/ai/ */
        char ai[1100];
#ifdef _WIN32
        SDL_snprintf(ai, sizeof ai, "%sai\\realesrgan-ncnn-vulkan.exe", base ? base : "");
#else
        SDL_snprintf(ai, sizeof ai, "%sai/realesrgan-ncnn-vulkan", base ? base : "");
#endif
        int use_ai = g_set.hd_style == 1 && SDL_GetPathInfo(ai, &info) && info.type == SDL_PATHTYPE_FILE;
        const char *args[] = { exe, dump, pack, use_ai ? "--ai" : NULL, ai, NULL };
        char prog[1200];
        SDL_snprintf(prog, sizeof prog, "%sprogress.txt", pack);
        SDL_RemovePath(prog);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        SDL_Process *pr = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (pr) {
            int code = -1;
            float t = 0;
            (void)t;
            char line[160] = "Upscaling...";
            float frac = 0;
            for (int k = 0; !SDL_WaitProcess(pr, false, &code); k++) {
                if (k % 10 == 0) {                   /* "<done> <total> <message>" */
                    char *txt = SDL_LoadFile(prog, NULL);
                    int d = 0, n = 0, off = 0;
                    if (txt && SDL_sscanf(txt, "%d %d %n", &d, &n, &off) >= 2 && n > 0) {
                        char *nl = SDL_strchr(txt + off, '\n');
                        if (nl) *nl = 0;
                        int ai_phase = SDL_strstr(txt + off, "AI") != NULL;
                        SDL_snprintf(line, sizeof line, "%s: %d of %d%s", txt + off, d, n, ai_phase ? " (uses your graphics card)" : "");
                        frac = use_ai ? (ai_phase ? 0.3f + 0.7f * d / n : 0.3f * d / n) : (float)d / n;
                    }
                    SDL_free(txt);
                }
                if (!job_progress("Building HD pack", line, SDL_min(frac, 0.99f))) {
                    SDL_KillProcess(pr, true);
                    SDL_WaitProcess(pr, true, &code);
                    SDL_DestroyProcess(pr);
                    return 0;                        /* cancelled */
                }
                SDL_Delay(30);
            }
            SDL_DestroyProcess(pr);
            if (code == 3) return 2;                 /* AI couldn't run: xBRZ pack */
            if (code == 0) return 1;
        }
    }
    job_progress("Building HD pack", "Upscaling (Scale2x)...", 0.5f);
    return hdpack_make_starter(dump, pack) > 0;
}

static int menu_campaigns(void) { return game ? game_campaigns(game) : 1; }
static void restart_campaign(void)
{
    if (!game) return;
    g_start_campaign = g_cheat.campaign;
    game_boot(game);
    rw_clear();
    if (aud) SDL_ClearAudioStream(aud);
    menu_close();
    char m[96];
    SDL_snprintf(m, sizeof m, "New game starts at campaign %d: press Start", g_cheat.campaign + 1);
    say(m);
}

static int start_game(int gi)
{
    size_t rom_n = 0;
    void *rom = SDL_LoadFile(rom_path_of[gi], &rom_n);
    if (!rom || !gen_set_rom(rom, (uint32_t)rom_n) || game_identify_rom() != g_games[gi]) {
        SDL_free(rom);
        say("Could not load that ROM");
        return 0;
    }
    SDL_free(rom);
    game = g_games[gi];
    remember_rom(rom_path_of[gi], game);
    SDL_SetWindowTitle(win, game->title);
    mh.game_title = game->title;
    for (int i = 0; i < MENU_SLOTS; i++) slot_known[i] = 0;
    user_paused = 0;
    game_boot(game);
    if (extracting) {
        hdpack_unload();
        SDL_snprintf(dump_dir, sizeof dump_dir, "%shd_dump/%s/", pref, game->id);
        char d[1100];
        SDL_snprintf(d, sizeof d, "%shd_dump", pref);
        SDL_CreateDirectory(d);
    } else load_hd_pack();
    apply_settings();                           /* widescreen limits depend on the game */
    if (aud) SDL_ClearAudioStream(aud);
    return 1;
}

static uint64_t menu_down;                       /* the menu button: when it went down */
static int menu_held, menu_used;                 /* held last frame; this press already acted */

static void run_game(void)
{
    uint64_t next = SDL_GetTicksNS(), fps_t0 = next;
    uint64_t game_frames0 = g_frame;
    int disp_frames = 0;
    while (running && !switch_requested) {
        SDL_Event e;
        inject_test_input();
        while (SDL_PollEvent(&e)) handle_event(&e);

        if (menu_is_open()) {
            menu_update();
            input_clear_pressed();
            pad_now = 0;
            menu_held = input_held(ACT_MENU); menu_used = 1;   /* the press that closes it does nothing more */
        } else {
            /* the menu button: a tap opens the menu (on release), a hold shows/hides the minimap */
            {
                int h = input_held(ACT_MENU);
                uint64_t t = SDL_GetTicksNS();
                if (h && !menu_held) { menu_down = t; menu_used = 0; }
                if (h && !menu_used && t - menu_down > 450000000ull) {
                    menu_used = 1;
                    g_set.minimap = !g_set.minimap;
                    settings_save(settings_path);
                    say(g_set.minimap ? "Minimap on" : "Minimap off");
                }
                if (!h && menu_held && !menu_used) { menu_open(); input_clear_pressed(); }
                menu_held = h;
            }
            if (input_pressed(ACT_MM_TOGGLE)) { g_set.minimap = !g_set.minimap; settings_save(settings_path); say(g_set.minimap ? "Minimap on" : "Minimap off"); }
            if (input_pressed(ACT_FULLSCREEN)) toggle_fullscreen();
            if (input_pressed(ACT_PAUSE)) { user_paused = !user_paused; say(user_paused ? "Paused" : "Resumed"); }
            if (input_pressed(ACT_SAVE_STATE)) save_slot(0);
            if (input_pressed(ACT_LOAD_STATE)) load_slot(0);
            if (input_pressed(ACT_MM_NEXT)) mm_step(1);
            if (input_pressed(ACT_MM_PREV)) mm_step(-1);
            pad_now = input_pad_state();
        }

        int halted = menu_is_open() || user_paused || (!focused && g_set.pause_unfocused && !max_ticks);
        uint64_t now = SDL_GetTicksNS();
        int ff = !halted && input_held(ACT_FAST_FORWARD);
        int rw = !halted && g_set.rewind && (input_held(ACT_REWIND) || (max_ticks && tick >= rw_test_at && tick < rw_test_at + rw_test_n));
        if (!halted && !rw) mm_update();
        if (rw != rewinding) {                   /* entering or leaving rewind */
            rewinding = rw;
            if (aud) SDL_ClearAudioStream(aud);
            watch_reset = 1;
            next = now;
        }
        if (halted) {
            next = now;
        } else if (rw) {
            if (max_ticks || now >= next) {       /* one step per display frame at 60 Hz */
                rewind_step();
                next += FRAME_NS;
                if (now > next + 250000000ull) next = now;
            }
        } else if (max_ticks) {
            for (int k = 0; k < npokes; k++) if (pokes[k].t == tick) {
                g_ram[pokes[k].a & 0xFFFF] = (uint8_t)(pokes[k].v >> 8);
                g_ram[(pokes[k].a + 1) & 0xFFFF] = (uint8_t)pokes[k].v;
            }
            cpu_run_frame();                     /* test mode: as fast as possible */
            pump_audio(0);
            rewind_record();
            if (shot_every && g_frame % shot_every == 0) save_bmp(shot_dir);
        } else if (sync_on && !ff) {
            cpu_run_frame();                     /* one game frame per refresh */
            pump_audio(1);
            rewind_record();
            next = now + FRAME_NS;               /* the clock picks up from here if sync stops */
        } else {
            int ran = 0, maxrun = ff ? g_set.ff_speed : 4;
            if (now > next && now - next > 250000000ull) next = now;      /* resync after a stall */
            while (now >= next && ran < maxrun) {
                cpu_run_frame();
                pump_audio(!ff);
                rewind_record();
                next += ff ? FRAME_NS / g_set.ff_speed : FRAME_NS;
                ran++;
            }
        }
        if (!halted && !rw) watch_frame();
        if (!halted) rotor_frame();
        if (!halted) scene_frame();
        present();
        after_present();
        if (!max_ticks) pacing_update(SDL_GetTicksNS());
        disp_frames++;
        if (now - fps_t0 >= 1000000000ull) {
            double dt = (now - fps_t0) / 1e9;
            fps_game = (g_frame - game_frames0) / dt;
            fps_disp = disp_frames / dt;
            fps_t0 = now; game_frames0 = g_frame; disp_frames = 0;
            if (!max_ticks) diag_second(now);
        }
        if (max_ticks) continue;
        if (!halted) {
            uint64_t t = SDL_GetTicksNS();               /* (display sync: present() already waited) */
            /* clock pacing: wait for the next game frame, so the picture (and the CRT
             * filter's GPU work) is drawn once a frame even if VSync doesn't wait */
            if (!sync_on && t < next) SDL_DelayPrecise(next - t);   /* on time: sleeps, then spins the last moment */
        } else {
            SDL_Delay(8);
        }
    }
    if (menu_is_open()) menu_close();
}

/* ---------------- main ---------------- */
int main(int argc, char **argv)
{
    const char *rom_arg = 0, *script = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_ticks = SDL_strtoll(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--make-hd-pack")) opt_make_pack = 1;
        else if (!strcmp(argv[i], "--extract")) { opt_extract = 1; if (i + 1 < argc && argv[i + 1][0] != '-' && SDL_isdigit(argv[i + 1][0])) opt_extract_campaign = SDL_atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--cheats") && i + 1 < argc) {     /* test: g f a l */
            const char *c = argv[++i];
            g_cheat.god = !!SDL_strchr(c, 'g'); g_cheat.fuel = !!SDL_strchr(c, 'f');
            g_cheat.ammo = !!SDL_strchr(c, 'a'); g_cheat.lives = !!SDL_strchr(c, 'l');
        }
        else if (!strcmp(argv[i], "--campaign") && i + 1 < argc) g_start_campaign = SDL_atoi(argv[++i]);
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc && npokes < 16) {
            unsigned long long t = 0; unsigned a = 0, v = 0;
            if (SDL_sscanf(argv[++i], "%llu:%x:%x", &t, &a, &v) == 3) { pokes[npokes].t = (long long)t; pokes[npokes].a = a; pokes[npokes].v = (uint16_t)v; npokes++; }
        }
        else if (!strcmp(argv[i], "--sound-lab")) g_sound_lab = 1;      /* test */
        else if (!strcmp(argv[i], "--rewind-at") && i + 1 < argc) {
            rw_test_at = SDL_strtoll(argv[++i], 0, 10);
            const char *c = SDL_strchr(argv[i], ':');
            rw_test_n = c ? SDL_strtoll(c + 1, 0, 10) : 60;
        }
        else if (!strcmp(argv[i], "--shot-every") && i + 2 < argc) { shot_every = SDL_atoi(argv[++i]); shot_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) script = argv[++i];
        else if ((!strcmp(argv[i], "--key") || !strcmp(argv[i], "--screen-shot")) && i + 1 < argc) {
            timed_t *t = argv[i][2] == 'k' ? &keys[nkeys < 63 ? nkeys++ : 63] : &shots[nshots < 63 ? nshots++ : 63];
            char *c = strchr(argv[i + 1], ':');
            t->t = SDL_strtoll(argv[i + 1], 0, 10);
            SDL_strlcpy(t->s, c ? c + 1 : "", sizeof t->s);
            i++;
        }
        else if (argv[i][0] != '-') rom_arg = argv[i];
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_TITLE, SDL_GetError(), 0);
        return 1;
    }
    /* settings folder keeps its original name so existing settings, controls
     * and Desert Strike save states carry over */
    pref = SDL_GetPrefPath("DesertStrikeHD", "DesertStrikeHD");
    if (!pref) pref = SDL_strdup("./");
    SDL_snprintf(settings_path, sizeof settings_path, "%ssettings.ini", pref);
    SDL_snprintf(controls_path, sizeof controls_path, "%scontrols.ini", pref);
    if (!settings_load(settings_path)) settings_save(settings_path);
    if (g_set.minimap_size <= 2) { static const int was[3] = { 24, 32, 42 }; g_set.minimap_size = was[g_set.minimap_size]; }   /* old Small/Medium/Large */
    if (!input_load(controls_path)) input_save(controls_path);
    SDL_snprintf(stats_path, sizeof stats_path, "%sstats.ini", pref);
    stats_load(stats_path);
    rw_init(cpu_state_size(), 48u << 20);
    SDL_snprintf(amiga_dir, sizeof amiga_dir, "%samiga", pref);
    SDL_snprintf(sfxmap_path, sizeof sfxmap_path, "%samiga_sfx.ini", pref);
    sfxmap_load();
    sfxmap_add_defaults();
    SDL_snprintf(musmap_path, sizeof musmap_path, "%samiga_music.ini", pref);
    musmap_load();
    if (amiga_load_dir(amiga_dir)) SDL_Log("Amiga: %d samples, %d songs", amiga_sample_count(), amiga_module_count());
    if (SDL_getenv("DS_ANIM_DUMP")) {               /* DS_ANIM_DUMP=<anim id>:<png prefix>: every frame */
        char spec[200]; SDL_strlcpy(spec, SDL_getenv("DS_ANIM_DUMP"), sizeof spec);
        char *c = SDL_strchr(spec, ':');
        if (c) {
            *c = 0;
            const amiga_anim *a = amiga_anim_at(amiga_anim_find(spec));
            for (int f = 0; a && f < a->nframes; f++) {
                uint8_t *rgb = SDL_malloc((size_t)a->w * a->h * 3);
                for (int i = 0; i < a->w * a->h; i++) { uint32_t v = a->pal[a->frames[f][i]]; rgb[i*3] = v >> 16; rgb[i*3+1] = v >> 8; rgb[i*3+2] = v; }
                char path[300]; SDL_snprintf(path, sizeof path, "%s%03d.png", c + 1, f);
                stbi_write_png(path, a->w, a->h, 3, rgb, a->w * 3);
                SDL_free(rgb);
            }
            SDL_Log("anim %s: %d frames", spec, a ? a->nframes : -1);
        }
        return 0;
    }
    if (SDL_getenv("DS_LIST_SAMPLES")) {
        for (int i = 0; i < amiga_sample_count(); i++) { const amiga_sample *a = amiga_sample_at(i); SDL_Log("sample %s|%s|%d|%d", a->id, a->label, a->rate, a->len); }
        return 0;
    }
    if (SDL_getenv("DS_AVM_TEST")) {            /* DS_AVM_TEST=<program index or owner>:<frames>:<out prefix> */
        char spec[200]; SDL_strlcpy(spec, SDL_getenv("DS_AVM_TEST"), sizeof spec);
        char *c1 = SDL_strchr(spec, ':'), *c2 = c1 ? SDL_strchr(c1 + 1, ':') : NULL;
        if (c1 && c2) {
            *c1 = *c2 = 0;
            for (int i = 0; i < amiga_program_count(); i++) SDL_Log("program %d %s %zu", i, amiga_program_at(i)->owner, amiga_program_at(i)->n);
            const amiga_program *pg = spec[0] == 'c' ? amiga_program_find(spec) : amiga_program_at(SDL_atoi(spec));
            int frames = SDL_atoi(c1 + 1);
            if (pg && avm_start(pg->data, pg->n)) {
                for (int f = 1; f <= frames && avm_running(); f++) {
                    avm_frame();
                    if (f % 25 == 0 || f == frames) {
                        int w, h; const uint32_t *px = avm_screen(&w, &h);
                        uint8_t *rgb = SDL_malloc((size_t)AVM_W * AVM_H * 3);
                        for (int i = 0; i < w * h; i++) { uint32_t v = px[(i / (w ? w : 1)) * AVM_W + i % (w ? w : 1)]; rgb[i*3] = v >> 16; rgb[i*3+1] = v >> 8; rgb[i*3+2] = v; }
                        char path[300]; SDL_snprintf(path, sizeof path, "%s%04d.png", c2 + 1, f);
                        if (w && h) stbi_write_png(path, w, h, 3, rgb, w * 3);
                        SDL_free(rgb);
                        SDL_Log("avm frame %d %dx%d pc=%06X", f, w, h, avm_debug_pc());
                    }
                }
                avm_debug_dump(); SDL_Log("avm finished=%d", avm_finished());
            } else SDL_Log("avm: no program / failed to start");
        }
        return 0;
    }
    sfx_mix_init(AUDIO_RATE);
    g_sfx_hook = host_sfx_hook;
    g_music_hook = host_music_hook;
    g_damage_hook = host_damage_hook;
    rw_state = SDL_malloc(cpu_state_size());

    win = SDL_CreateWindow(APP_TITLE, g_set.window_w, g_set.window_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    /* Renderer is selected at launch. Environment setting SDL_RENDER_DRIVER
     * retains SDL's normal higher priority as an override. */
    /* The CRT filter runs on OpenGL: Automatic picks it when the filter is on. */
    int want_gl = g_set.renderer == RENDERER_OPENGL || (g_set.renderer == RENDERER_AUTO && g_set.crt);
    if (want_gl) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    else if (g_set.renderer == RENDERER_SOFTWARE) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    ren = win ? SDL_CreateRenderer(win, 0) : 0;
    if (win && !ren && (want_gl || g_set.renderer != RENDERER_AUTO)) {   /* chosen backend unavailable: let SDL pick */
        SDL_Log("renderer: %s unavailable (%s), using automatic", want_gl ? "opengl" : "software", SDL_GetError());
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
        ren = SDL_CreateRenderer(win, 0);
    }
    if (!ren) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_TITLE, SDL_GetError(), 0);
        return 1;
    }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, VDP_MAX_W, VDP_H);
    if (!crt_init(ren)) SDL_Log("crt: %s", crt_why_not());
    ui_init(ren);
    load_icon();
    load_modes();

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    aud = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, 0, 0);
    if (aud) SDL_ResumeAudioStreamDevice(aud);
    else SDL_Log("no audio: %s", SDL_GetError());

    mh = (menu_host){
        .save_slot = save_slot, .load_slot = load_slot, .slot_info = slot_info,
        .apply_settings = apply_settings, .save_controls = save_controls,
        .restart = restart_game, .quit = quit_game, .switch_game = switch_game,
        .mode_count = mode_count, .mode_name = mode_name,
        .open_settings_folder = open_settings_folder,
        .make_hd_pack = make_hd_pack,
        .campaign_count = menu_campaigns, .restart_campaign = restart_campaign,
        .stats_lines = stats_lines,
        .add_amiga_disks = add_amiga_disks,
    };
    menu_init(&mh);
    input_init();
    gen_vblank_cb = on_vblank;
    gen_frame_cb = on_frame;
    cpu_fatal_hook = on_fatal;

    scan_roms();
    int pick = -1;
    if (rom_arg) {                               /* a ROM on the command line starts it directly */
        const game_module *g = identify_file(rom_arg);
        if (g) { pick = game_index(g); SDL_strlcpy(rom_path_of[pick], rom_arg, sizeof rom_path_of[pick]); }
        else SDL_Log("%s is not a supported ROM", rom_arg);
    }
    if (script) use_script = gen_load_inputs(script);
    if (opt_extract) { run_extract(); running = 0; }

    while (running) {
        int from_selector = pick < 0;
        int gi = pick >= 0 ? pick : run_selector();
        int packing = pack_request != 0;
        pack_request = 0;
        if (packing) from_selector = 0;             /* Build HD Pack from the picker: straight in */
        if (from_selector) { g_start_campaign = 0; memset(&g_cheat, 0, sizeof g_cheat); }
        pick = -1;
        if (gi < 0 || !running) break;
        if (!start_game(gi)) continue;
        rw_clear();
        watch_reset = 1;
        memset(&wt, 0, sizeof wt);
        resume_worthy = 0;
        if (opt_make_pack || packing) make_hd_pack();
        if (!SDL_strcmp(game->id, "desert") && !amiga_sample_count() &&
            (g_set.amiga_sfx || g_set.amiga_music || g_set.amiga_intro || g_set.amiga_extras))
            say("Amiga sounds, music and intro need your Amiga disks: Settings > Audio > Add Amiga Disks");
        int how = 0, slot = -1;
        if (from_selector || SDL_getenv("DS_PROMPT")) how = continue_prompt(&slot);   /* DS_PROMPT: tests */
        if (how < 0) { game = 0; continue; }             /* back to the game picker */
        if (how == 1 && !load_slot(slot)) how = 0;
        if (how == 2) menu_open_load();
        if (how == 0 && (from_selector || SDL_getenv("DS_PROMPT")) && !pick_difficulty()) { game = 0; continue; }
        if (how == 0) amiga_intro();
        run_game();
        if (resume_worthy && game) write_state(RESUME_SLOT);   /* Continue picks up here */
        stats_save(stats_path);
        if (switch_requested) { switch_requested = 0; game = 0; sel_row = 0; sel_col = gi; }
    }

    settings_save(settings_path);
    input_save(controls_path);
    stats_save(stats_path);
    ui_shutdown();
    if (pool.done) pool_stop();
    if (SDL_getenv("DS_PROFILE") && render_frames) SDL_Log("render: %.2f ms/frame over %llu frames", render_ns / 1e6 / render_frames, (unsigned long long)render_frames);
    SDL_Quit();
    return 0;
}
