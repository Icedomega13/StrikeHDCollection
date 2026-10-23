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
        fb_w = vdp_display_width();
        return;
    }
    /* Widescreen only applies to the scrolling playfield; other screens
     * are pillarboxed so the picture size never jumps. */
    /* playfield test, confirmed by the module's map check (ws_world returns
     * 0 when the planes don't hold the map, e.g. during a cutscene) */
    vdp_world_plane *wp = ws_planes;
    int play = game && game->ws_active && game->ws_active();
    if (play && game->ws_world && !game->ws_world(wp, ropts.ext_left, ropts.ext_right)) play = 0;
    /* Anything that isn't the live battlefield (menus, maps, cutscenes) is
     * shown 4:3. After a non-playfield frame, wait half a second of clean
     * playfield before widening again, so a cutscene never flickers wide. */
    static int settle;
    if (!play) settle = 30;
    else if (settle > 0) { settle--; play = 0; }
    int active = ropts.ext_left && play;
    hud_ok = play;
    ropts.world[0] = ropts.world[1] = NULL;
    ropts.wide_sprites = 0;
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
static SDL_Texture *slot_tex[MENU_SLOTS];
static int slot_known[MENU_SLOTS], slot_has[MENU_SLOTS];
static char slot_when[MENU_SLOTS][32];

static void state_path(char *out, size_t n, int slot)
{
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%sstates", pref);
    SDL_CreateDirectory(dir);
    if (slot == 0) SDL_snprintf(out, n, "%s/%s_quick.state", dir, game->id);
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
    slot_known[slot] = 0;
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
        rw_clear();
        watch_reset = 1;
        say(slot ? "State loaded" : "Quick state loaded");
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

static void apply_settings(void)
{
    apply_window_mode();
    SDL_SetRenderVSync(ren, g_set.vsync ? 1 : 0);
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

static SDL_FRect game_dst;                   /* where the game image was drawn */

/*
 * Status HUD: armour, fuel, load and lives in the game image's top-left
 * corner, in the pause menu's style. Sized from the image height so it
 * keeps its proportion in every window and widescreen setting.
 */
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
    float lh = ui_line_h(f) * 0.92f;
    float lab = ui_text_w(f, "ARMOR") + 10 * u;
    float barw = 72 * u, valw = ui_text_w(f, "0000");
    float pad = 7 * u, w = pad * 2 + lab + barw + 8 * u + valw, rows = 5;
    float x = x0 + 14 * u, y = y0 + 12 * u, hgt = pad * 2 + lh * rows;
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
    static const float frac[3] = { 0.24f, 0.32f, 0.42f };
    float mh = (y1 - y0) * frac[g_set.minimap_size];       /* map height */
    float k = mh / r[3], mw = r[2] * k;
    float maxw = (x1 - x0) * 0.38f;
    if (mw > maxw) { k = maxw / r[2]; mw = maxw; mh = r[3] * k; }
    float pad = 6 * u, lh = ui_line_h(f) * 0.92f;
    float pw = mw + pad * 2, ph = mh + pad * 2 + lh;
    float px = x0 + 14 * u, py = y1 - 12 * u - ph;
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
    float k = g_set.rumble / 100.0f;
    input_rumble(lo * k, hi * k, ms);
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
        wt.level_save = !watch_reset;            /* not when it came from a loaded state */
        if (wt.level_save) s->missions++;
        memcpy(wt.cnt, cnt, sizeof cnt);
        memset(wt.zero, 0, sizeof wt.zero);
    }
    if (mmd.campaign > s->best_campaign) s->best_campaign = mmd.campaign;
    wt.level_frames += df;
    if (wt.level_save && wt.level_frames >= 120) { wt.level_save = 0; autosave("mission start"); }
    for (int i = 0; i < n; i++) {
        if (cnt[i] > 0) { wt.cnt[i] = cnt[i]; wt.zero[i] = 0; continue; }
        if (wt.cnt[i] <= 0) continue;
        char name[64];
        if (game->minimap_name(i, name, sizeof name) != 0) { wt.cnt[i] = 0; continue; }
        if ((wt.zero[i] += df) < 45) continue;   /* gone for good, not a flicker */
        wt.cnt[i] = 0;
        s->objectives++;
        char why[96];
        SDL_snprintf(why, sizeof why, "%s complete", name);
        autosave(why);
    }
}

static void watch_frame(void)
{
    if (!game || att_on || extracting || sandbox || !game->hud) return;
    game_hud h;
    if (!hud_ok || !game->hud(&h)) { wt.ok = 0; return; }
    game_stats *s = stats_for(game->id);
    int df = (int)(g_frame - wt.last_frame);
    wt.last_frame = g_frame;
    if (watch_reset || !wt.ok || df <= 0 || df > 30) {
        wt.h = h;
        wt.ok = 1;
        if (watch_reset && mmd_ok) {             /* baseline: no autosave for this level */
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
        rumble(1.0f, 1.0f, 700);
    } else {
        int dmg = wt.h.armor - h.armor;
        if (dmg > 0 && dmg < 5000) {
            s->damage += dmg;
            float k = (float)dmg / (h.armor_max > 0 ? h.armor_max : 600);
            rumble(0.35f + k * 4, 0.25f + k * 4, 140 + (int)(k * 800));
        }
    }
    static const float fire_lo[3] = { 0.0f, 0.25f, 0.6f }, fire_hi[3] = { 0.22f, 0.45f, 0.8f };
    static const int fire_ms[3] = { 45, 90, 180 };
    for (int i = 0; i < 3; i++) {
        int d = wt.h.ammo[i] - h.ammo[i];
        if (wt.h.ammo[i] < 0 || h.ammo[i] < 0 || d <= 0 || d > 50) continue;
        s->shots[i] += d;
        rumble(fire_lo[i], fire_hi[i], fire_ms[i]);
    }
    if (h.load > wt.h.load) s->picked_up += h.load - wt.h.load;
    else if (h.load < wt.h.load && h.lives == wt.h.lives) s->delivered += wt.h.load - h.load;
    wt.h = h;
    watch_objectives(s, df);
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
    int S = 1;
    SDL_Texture *src_tex = tex;
    if (hd_on && hdtex) {
        S = hd.scale;
        src_tex = hdtex;
        SDL_UpdateTexture(hdtex, &(SDL_Rect){ 0, 0, fb_w * S, VDP_H * S }, hdfb, VDP_MAX_W * 4 * 4);
    } else {
        SDL_UpdateTexture(tex, &(SDL_Rect){ 0, 0, fb_w, VDP_H }, fb, VDP_MAX_W * 4);
    }
    int dispw = vdp_display_width();
    double aspect;
    switch (g_set.aspect) {
    case ASPECT_SQUARE:  aspect = (double)fb_w / VDP_H; break;
    case ASPECT_STRETCH: aspect = (double)ow / oh; break;
    default:             aspect = (4.0 / 3.0) * fb_w / (double)dispw; break;   /* TV shape */
    }
    double dw = ow, dh = ow / aspect;
    if (dh > oh) { dh = oh; dw = oh * aspect; }
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

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
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

static void present(void)
{
    int ow, oh;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    if (ow != out_w || oh != out_h) {
        out_w = ow; out_h = oh;
        if (g_set.widescreen == WS_AUTO) apply_ws();
    }
    draw_game(ow, oh);
    ui_begin(ow, oh);
    draw_hud();
    draw_minimap();
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
    if (g_set.show_fps) {
        char s[64];
        SDL_snprintf(s, sizeof s, "Game %.0f  Display %.0f", fps_game, fps_disp);
        ui_rect(1260 - ui_text_w(UI_SMALL, s) - 16, 10, ui_text_w(UI_SMALL, s) + 16, 30, ui_rgba(0, 0, 0, 160));
        ui_text_right(UI_SMALL, 1252, 14, ui_rgba(160, 230, 140, 255), s);
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

/* ---------------- audio ---------------- */
static void pump_audio(int keep)
{
    static int16_t buf[4096 * 2];
    int n;
    while ((n = snd_read(buf, 4096)) > 0)
        if (keep && aud) SDL_PutAudioStreamData(aud, buf, n * 4);
    if (!aud) return;
    /* Hold about 3 frames queued; nudge the playback rate to stay there. */
    float queued = SDL_GetAudioStreamQueued(aud) / (4.0f * AUDIO_RATE);
    if (queued > 0.25f) { SDL_ClearAudioStream(aud); queued = 0; }
    float ratio = 1.0f + (queued - 0.05f) * 0.2f;
    if (ratio < 0.99f) ratio = 0.99f;
    if (ratio > 1.01f) ratio = 1.01f;
    SDL_SetAudioStreamFrequencyRatio(aud, ratio);
}

static void handle_event(const SDL_Event *e)
{
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
    if (!game) return;
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
        } else {
            if (input_pressed(ACT_MENU)) { menu_open(); input_clear_pressed(); }
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
        present();
        after_present();
        disp_frames++;
        if (now - fps_t0 >= 1000000000ull) {
            double dt = (now - fps_t0) / 1e9;
            fps_game = (g_frame - game_frames0) / dt;
            fps_disp = disp_frames / dt;
            fps_t0 = now; game_frames0 = g_frame; disp_frames = 0;
        }
        if (max_ticks) continue;
        if (!halted) {
            uint64_t t = SDL_GetTicksNS();
            if (t < next && next - t > 2000000) SDL_DelayNS(next - t - 1000000);
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
    if (!input_load(controls_path)) input_save(controls_path);
    SDL_snprintf(stats_path, sizeof stats_path, "%sstats.ini", pref);
    stats_load(stats_path);
    rw_init(cpu_state_size(), 48u << 20);
    rw_state = SDL_malloc(cpu_state_size());

    win = SDL_CreateWindow(APP_TITLE, g_set.window_w, g_set.window_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    ren = win ? SDL_CreateRenderer(win, 0) : 0;
    if (!ren) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_TITLE, SDL_GetError(), 0);
        return 1;
    }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, VDP_MAX_W, VDP_H);
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
        if (from_selector) { g_start_campaign = 0; memset(&g_cheat, 0, sizeof g_cheat); }
        pick = -1;
        if (gi < 0 || !running) break;
        if (!start_game(gi)) continue;
        rw_clear();
        watch_reset = 1;
        memset(&wt, 0, sizeof wt);
        if (opt_make_pack) make_hd_pack();
        run_game();
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
