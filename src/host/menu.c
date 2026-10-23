/* HD pause menu. See menu.h. */
#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "ui.h"
#include "input.h"
#include "settings.h"

/* ---------------- look ---------------- */
#define C_PANEL   ui_rgba(14, 17, 12, 236)
#define C_PANEL2  ui_rgba(22, 27, 19, 240)
#define C_EDGE    ui_rgba(196, 164, 98, 255)
#define C_EDGE_D  ui_rgba(196, 164, 98, 90)
#define C_ACCENT  ui_rgba(242, 178, 58, 255)
#define C_TEXT    ui_rgba(232, 226, 210, 255)
#define C_DIM     ui_rgba(150, 148, 132, 255)
#define C_DARK    ui_rgba(8, 9, 7, 255)
#define C_SEL     ui_rgba(242, 178, 58, 46)

/* ---------------- items ---------------- */
enum { IT_BUTTON, IT_CHOICE, IT_SLIDER, IT_MODE };

typedef struct {
    int kind;
    const char *label, *hint;
    int *val;
    const char *const *opts; int nopts;          /* choice */
    int lo, hi, step; const char *unit;          /* slider */
    void (*fn)(void);                            /* button */
} item;

static const char *const OFF_ON[] = { "Off", "On" };
static const char *const MMSIZE[] = { "Small", "Medium", "Large" };
static const char *const HDSTYLE[] = { "xBRZ (sharp)", "AI (Real-ESRGAN)" };
static const char *const WINMODE[] = { "Windowed", "Borderless Fullscreen", "Exclusive Fullscreen" };
static const char *const RENDERER[] = { "Automatic", "OpenGL (GPU)", "Software" };
static const char *const ASPECT[] = { "TV Shape (original)", "Square Pixels", "Stretch to Window" };
static const char *const WIDE[] = { "Off (4:3)", "16:10", "16:9", "21:9", "32:9", "Match window" };
static const char *const SCALING[] = { "Sharp", "Integer Only", "Smooth", "Pixelated" };
static const char *const SCANS[] = { "Off", "Light", "Medium", "Strong" };
static const char *const SPEED[] = { "Smooth (no slowdown)", "Original (with slowdown)" };
static const char *const FLICKER[] = { "Off (all sprites shown)", "Original (flicker)" };

static const menu_host *H;
static void open_folder(void) { if (H->open_settings_folder) H->open_settings_folder(); }
static void make_pack(void) { if (H->make_hd_pack) H->make_hd_pack(); }

static item tab_display[] = {
    { IT_CHOICE, "Window Mode", "Borderless fullscreen switches instantly; exclusive uses the resolution below.", &g_set.window_mode, WINMODE, 3 },
    { IT_MODE,   "Fullscreen Resolution", "Used in exclusive fullscreen. Desktop keeps your monitor's current mode.", &g_set.fullscreen_mode },
    { IT_CHOICE, "VSync", "Syncs to your monitor's refresh to prevent tearing.", &g_set.vsync, OFF_ON, 2 },
    { IT_CHOICE, "Show FPS", "Shows game and display frame rates in the corner.", &g_set.show_fps, OFF_ON, 2 },
    { IT_CHOICE, "Renderer (restart)", "Choose the SDL rendering backend. The active backend appears in the FPS display; restart to apply.", &g_set.renderer, RENDERER, RENDERER_COUNT },
};
static item tab_picture[] = {
    { IT_CHOICE, "Widescreen", "Shows more of the battlefield to the sides. Menus and maps stay 4:3.", &g_set.widescreen, WIDE, 6 },
    { IT_CHOICE, "Pixel Shape", "TV shape matches a 4:3 set of the era. Square pixels is the raw image.", &g_set.aspect, ASPECT, 3 },
    { IT_CHOICE, "Scaling", "Sharp keeps pixels crisp at any size. Integer leaves borders but is exact.", &g_set.scaling, SCALING, 4 },
    { IT_CHOICE, "Scanlines", "Darkens alternate lines like a CRT.", &g_set.scanlines, SCANS, 4 },
    { IT_CHOICE, "Sprite Flicker", "The console could show only 20 sprites per line; Off draws them all.", &g_set.sprite_flicker, FLICKER, 2 },
    { IT_CHOICE, "Status HUD", "Shows armor, fuel, load and lives in the top-left corner while flying.", &g_set.status_hud, OFF_ON, 2 },
    { IT_CHOICE, "Minimap", "The pause map in the lower-left corner while flying. LB / RB (Q / E) step through the objectives.", &g_set.minimap, OFF_ON, 2 },
    { IT_CHOICE, "Minimap Size", "How large the minimap is drawn.", &g_set.minimap_size, MMSIZE, 3 },
    { IT_CHOICE, "HD Graphics", "Uses replacement art from an HD pack in the packs folder, when one is installed.", &g_set.hd_graphics, OFF_ON, 2 },
};
static item tab_audio[] = {
    { IT_SLIDER, "Master Volume", "", &g_set.volume, 0, 0, 0, 100, 5, "%" },
    { IT_SLIDER, "FM Synth (music, effects)", "The YM2612 chip: most music and sound effects.", &g_set.fm_level, 0, 0, 0, 150, 5, "%" },
    { IT_SLIDER, "PSG (tones, noise)", "The SN76489 chip: rotor noise, beeps, some percussion.", &g_set.psg_level, 0, 0, 0, 150, 5, "%" },
    { IT_CHOICE, "Mute When Unfocused", "Silence the game while another window is active.", &g_set.mute_unfocused, OFF_ON, 2 },
};
static item tab_system[] = {
    { IT_CHOICE, "Game Speed", "Smooth gives the game a faster CPU so busy scenes never slow down. Original matches the console.", &g_set.authentic_speed, SPEED, 2 },
    { IT_SLIDER, "Fast-Forward Speed", "Hold Tab or the right trigger to fast-forward.", &g_set.ff_speed, 0, 0, 2, 8, 1, "x" },
    { IT_CHOICE, "Rewind", "Hold the left trigger (Backspace) to rewind the last half minute or so.", &g_set.rewind, OFF_ON, 2 },
    { IT_CHOICE, "Autosave", "Saves when a mission starts and when an objective is completed (Load State, Auto 1-3).", &g_set.autosave, OFF_ON, 2 },
    { IT_SLIDER, "Rumble", "Gamepad rumble when you fire, take hits or lose a life. 0 turns it off.", &g_set.rumble, 0, 0, 0, 100, 10, "%" },
    { IT_CHOICE, "Pause When Unfocused", "Freeze the game while another window is active.", &g_set.pause_unfocused, OFF_ON, 2 },
    { IT_CHOICE, "Launcher Intros", "Plays the games' intros live behind the game picker.", &g_set.launcher_intros, OFF_ON, 2 },
    { IT_CHOICE, "Dump Graphics", "For HD pack makers: saves every new graphic on screen to hd_dump in the settings folder.", &g_set.hd_dump, OFF_ON, 2 },
    { IT_CHOICE, "HD Pack Style", "How Build HD Pack upscales. AI uses your graphics card and the ai folder next to the game; without it, xBRZ is used.", &g_set.hd_style, HDSTYLE, 2 },
    { IT_BUTTON, "Build HD Pack", "Extracts every graphic of this game (a few minutes; the game restarts), upscales them 4x with xBRZ and loads the pack.", 0, 0, 0, 0, 0, 0, 0, make_pack },
    { IT_BUTTON, "Open Settings Folder", "Settings, controls, save states and your ROM copy live here.", 0, 0, 0, 0, 0, 0, 0, open_folder },
};
static const char *const CAMPAIGNS[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };
static void restart_campaign(void) { if (H->restart_campaign) H->restart_campaign(); }
static item tab_cheats[] = {
    { IT_CHOICE, "God Mode", "Armor stays full: nothing can destroy you.", &g_cheat.god, OFF_ON, 2 },
    { IT_CHOICE, "Unlimited Fuel", "The fuel gauge never drops.", &g_cheat.fuel, OFF_ON, 2 },
    { IT_CHOICE, "Unlimited Ammo", "Guns, Hydras and Hellfires stay at their full load.", &g_cheat.ammo, OFF_ON, 2 },
    { IT_CHOICE, "Unlimited Lives", "Losing a helicopter doesn't cost a life.", &g_cheat.lives, OFF_ON, 2 },
    { IT_CHOICE, "Campaign", "The campaign a new game starts at. Use Start Campaign, or it applies on the next new game.", &g_cheat.campaign, CAMPAIGNS, 4 },
    { IT_BUTTON, "Start Campaign", "Restarts the game; press Start through the title and it begins at the chosen campaign.", 0, 0, 0, 0, 0, 0, 0, restart_campaign },
};
#define NCHEATS 6
static struct { const char *name; item *it; int n; } tabs[] = {
#define N(a) (int)(sizeof a / sizeof a[0])
    { "Display", tab_display, N(tab_display) }, { "Picture", tab_picture, N(tab_picture) },
    { "Audio", tab_audio, N(tab_audio) }, { "System", tab_system, N(tab_system) },
#undef N
};
#define NTABS 4

/* ---------------- state ---------------- */
enum { M_RESUME, M_SAVE, M_LOAD, M_SETTINGS, M_CONTROLS, M_CHEATS, M_STATS, M_RESTART, M_SWITCH, M_QUIT, M_COUNT };
static const char *main_labels[M_COUNT] = { "Resume", "Save State", "Load State", "Settings", "Controls", "Cheats", "Stats", "Restart Game", "Switch Game", "Quit to Desktop" };

static int is_open, pane, main_focus, sub_focus, tab, slot_focus;
static int launcher_mode;                /* opened from the game picker: settings only */
static int vis[M_COUNT], nvis;           /* main entries shown, in order */

static void build_vis(void)
{
    nvis = 0;
    if (launcher_mode) { vis[nvis++] = M_SETTINGS; vis[nvis++] = M_CONTROLS; vis[nvis++] = M_RESUME; return; }
    for (int i = 0; i < M_COUNT; i++) vis[nvis++] = i;
}
static const char *main_label(int i) { return launcher_mode && i == M_RESUME ? "Back" : main_labels[i]; }
static int ctl_row, ctl_col, ctl_scroll;
static int confirm_kind = -1, confirm_yes;
static int capturing;                    /* controls: waiting for a key/button */
static char note[160];
static uint64_t note_until;

/* held-direction repeat */
static int held_dir;
static uint64_t held_since, last_repeat;

/* mouse hit regions recorded while drawing */
typedef struct { float x, y, w, h; int pane, idx, part; } hit;
static hit hits[128];
static int nhits;
static void add_hit(float x, float y, float w, float h, int p, int idx, int part)
{
    if (nhits < 128) hits[nhits++] = (hit){ x, y, w, h, p, idx, part };
}

static void set_note(const char *s)
{
    SDL_strlcpy(note, s, sizeof note);
    note_until = SDL_GetTicksNS() + 3000000000ull;
}

void menu_init(const menu_host *h) { H = h; }
int menu_is_open(void) { return is_open; }

void menu_open(void)
{
    is_open = 1; pane = 0; main_focus = M_RESUME; confirm_kind = -1; capturing = 0;
    held_dir = 0;
    launcher_mode = 0;
    build_vis();
}

void menu_open_launcher(void)
{
    menu_open();
    launcher_mode = 1;
    build_vis();
    main_focus = M_SETTINGS;
}

void menu_close(void)
{
    is_open = 0; capturing = 0;
    input_clear_pressed();
}

/* the item page shown in the right pane: a settings tab, or the cheats */
static item *page(int *n)
{
    if (main_focus == M_CHEATS) {
        tab_cheats[4].nopts = H->campaign_count ? SDL_clamp(H->campaign_count(), 1, 10) : 1;
        if (*tab_cheats[4].val >= tab_cheats[4].nopts) *tab_cheats[4].val = 0;
        *n = NCHEATS;
        return tab_cheats;
    }
    *n = tabs[tab].n;
    return tabs[tab].it;
}
static int is_page(int m) { return m == M_SETTINGS || m == M_CHEATS; }

static int sub_count(void)
{
    switch (main_focus) {
    case M_SAVE: case M_LOAD: return MENU_SLOTS;
    case M_SETTINGS: case M_CHEATS: { int n; page(&n); return n; }
    case M_CONTROLS: return ACT_COUNT + 1;
    }
    return 0;
}

static int has_sub(int m) { return m == M_SAVE || m == M_LOAD || m == M_SETTINGS || m == M_CHEATS || m == M_CONTROLS; }

/* ---------------- value changes ---------------- */
static void changed(void) { if (H->apply_settings) H->apply_settings(); }

static void adjust(item *it, int d)
{
    if (it->kind == IT_CHOICE) {
        *it->val = (*it->val + d + it->nopts) % it->nopts;
    } else if (it->kind == IT_SLIDER) {
        int v = *it->val + d * it->step;
        *it->val = v < it->lo ? it->lo : v > it->hi ? it->hi : v;
    } else if (it->kind == IT_MODE) {
        int n = H->mode_count ? H->mode_count() : 0;
        int v = *it->val + 1 + d;                   /* -1 .. n-1 shifted to 0..n */
        v = (v % (n + 1) + n + 1) % (n + 1);
        *it->val = v - 1;
    } else {
        return;
    }
    changed();
}

/* ---------------- controls rebinding ---------------- */
static const char *act_label[ACT_COUNT] = {
    "Up", "Down", "Left", "Right", "A Button", "B Button", "C Button", "Start",
    "Save State (quick)", "Load State (quick)", "Fullscreen", "Fast Forward (hold)", "Pause", "Open This Menu",
    "Next Objective (minimap)", "Previous Objective (minimap)", "Rewind (hold)",
};

static void bind_set(int a, int col, int code, int clear)
{
    int pad = col >= 2, slot = col & 1;
    if (clear) {
        if (pad) g_bind.pad[a][slot] = PAD_NONE; else g_bind.key[a][slot] = SDL_SCANCODE_UNKNOWN;
    } else {
        /* one input does one thing: take it off any other action */
        for (int o = 0; o < ACT_COUNT; o++)
            for (int i = 0; i < MAX_BINDS; i++) {
                if (pad && g_bind.pad[o][i] == code && !(o == a && i == slot)) {
                    g_bind.pad[o][i] = PAD_NONE;
                    char m[120]; SDL_snprintf(m, sizeof m, "Removed from \"%s\".", act_label[o]); set_note(m);
                }
                if (!pad && (int)g_bind.key[o][i] == code && !(o == a && i == slot)) {
                    g_bind.key[o][i] = SDL_SCANCODE_UNKNOWN;
                    char m[120]; SDL_snprintf(m, sizeof m, "Removed from \"%s\".", act_label[o]); set_note(m);
                }
            }
        if (pad) g_bind.pad[a][slot] = code; else g_bind.key[a][slot] = (SDL_Scancode)code;
    }
    /* keep a way into the menu */
    int any = 0;
    for (int i = 0; i < MAX_BINDS; i++) any |= g_bind.key[ACT_MENU][i] != SDL_SCANCODE_UNKNOWN || g_bind.pad[ACT_MENU][i] != PAD_NONE;
    if (!any) { g_bind.key[ACT_MENU][0] = SDL_SCANCODE_ESCAPE; set_note("The menu always needs a binding: Esc restored."); }
    if (H->save_controls) H->save_controls();
}

/* ---------------- actions ---------------- */
static void confirm(int kind) { confirm_kind = kind; confirm_yes = 0; }

static void activate_main(void)
{
    switch (main_focus) {
    case M_RESUME: menu_close(); break;
    case M_STATS: break;
    case M_RESTART: confirm(M_RESTART); break;
    case M_SWITCH: confirm(M_SWITCH); break;
    case M_QUIT: confirm(M_QUIT); break;
    default: pane = 1; sub_focus = 0; slot_focus = 0; ctl_row = 0; ctl_col = 0; break;
    }
}

static void activate_sub(void)
{
    if (main_focus == M_SAVE) {
        if (slot_focus >= MENU_SAVE_SLOTS) { set_note("Autosaves are made for you: pick another slot."); return; }
        H->save_slot(slot_focus);
    } else if (main_focus == M_LOAD) {
        if (H->load_slot(slot_focus)) menu_close();
    } else if (is_page(main_focus)) {
        int n;
        item *it = &page(&n)[sub_focus];
        if (it->kind == IT_BUTTON) it->fn();
        else adjust(it, +1);
    } else if (main_focus == M_CONTROLS) {
        if (ctl_row == ACT_COUNT) {
            input_defaults(&g_bind);
            if (H->save_controls) H->save_controls();
            set_note("Controls reset to defaults.");
        } else {
            capturing = 1;
            input_capture_start(ctl_col >= 2);
        }
    }
}

static void do_confirm(void)
{
    int k = confirm_kind;
    confirm_kind = -1;
    if (!confirm_yes) return;
    if (k == M_RESTART) { H->restart(); menu_close(); }
    if (k == M_SWITCH && H->switch_game) { menu_close(); H->switch_game(); }
    if (k == M_QUIT) H->quit();
}

static void back(void)
{
    if (confirm_kind >= 0) { confirm_kind = -1; return; }
    if (pane == 1) { pane = 0; return; }
    menu_close();
}

static void move(int dx, int dy)
{
    if (confirm_kind >= 0) { if (dx) confirm_yes = !confirm_yes; return; }
    if (pane == 0) {
        if (dy) {
            int k = 0;
            while (k < nvis && vis[k] != main_focus) k++;
            main_focus = vis[(k + dy + nvis) % nvis];
        }
        if (dx > 0 && has_sub(main_focus)) activate_main();
        return;
    }
    if (main_focus == M_SAVE || main_focus == M_LOAD) {
        int c = slot_focus % 3, r = slot_focus / 3, rows = MENU_SLOTS / 3;
        if (dx) { if (c + dx < 0) { pane = 0; return; } c = (c + dx + 3) % 3; }
        if (dy) r = (r + dy + rows) % rows;
        slot_focus = r * 3 + c;
    } else if (is_page(main_focus)) {
        int n;
        item *its = page(&n);
        if (dy) sub_focus = (sub_focus + dy + n) % n;
        if (dx) adjust(&its[sub_focus], dx);
    } else if (main_focus == M_CONTROLS) {
        if (dy) ctl_row = (ctl_row + dy + ACT_COUNT + 1) % (ACT_COUNT + 1);
        if (dx) {
            if (ctl_col + dx < 0) { pane = 0; return; }
            ctl_col = (ctl_col + dx + 4) % 4;
        }
    }
}

static void tab_step(int d)
{
    if (main_focus != M_SETTINGS) return;
    tab = (tab + d + NTABS) % NTABS;
    sub_focus = 0;
}

/* ---------------- input ---------------- */
static int dir_now(void)
{
    /* Gamepads are polled (sticks have no repeat of their own); the
     * keyboard is handled as events in menu_event, using OS key repeat. */
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

static void apply_dir(int d)
{
    static const int dx[5] = { 0, 0, 0, -1, 1 }, dy[5] = { 0, -1, 1, 0, 0 };
    move(dx[d], dy[d]);
}

void menu_update(void)
{
    if (!is_open) return;
    if (capturing) {
        int code;
        int r = input_capture_poll(&code);
        if (r) {
            capturing = 0;
            if (r == 1) bind_set(ctl_row, ctl_col, code, 0);
            else if (r == 2) bind_set(ctl_row, ctl_col, 0, 1);
            held_dir = dir_now();                /* don't auto-move from a held stick */
            held_since = SDL_GetTicksNS();
        }
        return;
    }
    int d = dir_now();
    uint64_t now = SDL_GetTicksNS();
    if (d != held_dir) {
        held_dir = d;
        held_since = last_repeat = now;
        if (d) apply_dir(d);
    } else if (d && now - held_since > 350000000ull && now - last_repeat > 85000000ull) {
        last_repeat = now;
        apply_dir(d);
    }
}

static void confirm_press(void)
{
    if (confirm_kind >= 0) { do_confirm(); return; }
    if (pane == 0) activate_main(); else activate_sub();
}

static void click(float vx, float vy, int act)
{
    for (int i = nhits - 1; i >= 0; i--) {
        hit *h = &hits[i];
        if (vx < h->x || vy < h->y || vx >= h->x + h->w || vy >= h->y + h->h) continue;
        if (h->pane == 9) {                       /* confirm dialog buttons */
            confirm_yes = h->idx;
            if (act) do_confirm();
            return;
        }
        if (confirm_kind >= 0) return;
        if (h->pane == 0) {
            main_focus = h->idx; pane = 0;
            if (act) activate_main();
        } else if (h->pane == 2) {                /* settings tab */
            if (act) { tab = h->idx; sub_focus = 0; pane = 1; }
        } else if (h->pane == 1) {
            pane = 1;
            if (main_focus == M_SAVE || main_focus == M_LOAD) slot_focus = h->idx;
            else if (is_page(main_focus)) sub_focus = h->idx;
            else if (main_focus == M_CONTROLS) { ctl_row = h->idx >> 2; ctl_col = h->idx & 3; if (h->idx == 4 * ACT_COUNT) { ctl_row = ACT_COUNT; ctl_col = 0; } }
            if (act) {
                if (is_page(main_focus) && h->part) { int n; adjust(&page(&n)[sub_focus], h->part); }
                else activate_sub();
            }
        }
        return;
    }
}

void menu_event(const SDL_Event *e)
{
    if (!is_open) return;
    if (capturing) return;                        /* input.c owns events during capture */
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        switch (e->key.scancode) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: apply_dir(1); return;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: apply_dir(2); return;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: apply_dir(3); return;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: apply_dir(4); return;
        default: break;
        }
        if (e->key.repeat) break;
        switch (e->key.scancode) {
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: confirm_press(); break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: back(); break;
        case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: tab_step(-1); break;
        case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: tab_step(+1); break;
        default: break;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: confirm_press(); break;
        case SDL_GAMEPAD_BUTTON_EAST: back(); break;
        case SDL_GAMEPAD_BUTTON_BACK: case SDL_GAMEPAD_BUTTON_START: menu_close(); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: tab_step(-1); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: tab_step(+1); break;
        default: break;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        float vx, vy;
        ui_to_virtual(e->motion.x, e->motion.y, &vx, &vy);
        click(vx, vy, 0);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float vx, vy;
        ui_to_virtual(e->button.x, e->button.y, &vx, &vy);
        if (e->button.button == SDL_BUTTON_LEFT) click(vx, vy, 1);
        else if (e->button.button == SDL_BUTTON_RIGHT) back();
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (pane == 1) move(0, e->wheel.y > 0 ? -1 : 1);
        break;
    default: break;
    }
}

/* ---------------- drawing ---------------- */
static void draw_arrow(float x, float y, float s, int left, SDL_FColor c)
{
    if (left) ui_triangle(x + s, y, x + s, y + s * 1.6f, x, y + s * 0.8f, c);
    else      ui_triangle(x, y, x, y + s * 1.6f, x + s, y + s * 0.8f, c);
}

static void draw_main(void)
{
    float x = 60, y = 150, w = 400, h = 470;
    ui_panel(x, y, w, h, 18, C_PANEL, pane == 0 ? C_EDGE : C_EDGE_D);
    for (int k = 0; k < nvis; k++) {
        int i = vis[k];
        float step = SDL_min(50.5f, (h - 24) / nvis), rh = step - 0.5f;
        float iy = y + 12 + k * step;
        int f = i == main_focus;
        if (f) {
            ui_rect(x + 14, iy, w - 28, rh, pane == 0 ? C_SEL : ui_rgba(242, 178, 58, 20));
            ui_rect(x + 14, iy, 5, rh, pane == 0 ? C_ACCENT : C_DIM);
        }
        ui_text(UI_LARGE, x + 36, iy + 6 - (50 - rh) / 2, f ? (pane == 0 ? C_ACCENT : C_TEXT) : C_TEXT, main_label(i));
        if (has_sub(i)) draw_arrow(x + w - 40, iy + rh / 2 - 8, 9, 0, f ? C_ACCENT : C_DIM);
        add_hit(x + 14, iy, w - 28, rh, 0, i, 0);
    }
}

/* Career stats of the running game ("label\tvalue" lines from the host). */
static void draw_stats(void)
{
    float x = 490, y = 150, w = 730, h = 470;
    ui_panel(x, y, w, h, 18, C_PANEL, C_EDGE_D);
    ui_text(UI_BODY, x + 30, y + 18, C_ACCENT, "Career");
    ui_text_right(UI_SMALL, x + w - 30, y + 24, C_DIM, H->game_title ? H->game_title : "");
    ui_rect(x + 24, y + 56, w - 48, 2, C_EDGE_D);
    char lines[16][96];
    int n = H->stats_lines ? H->stats_lines(lines, 16) : 0;
    float ly = y + 72, lh = SDL_min(36.0f, (h - 90) / (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        char *tab = SDL_strchr(lines[i], '\t');
        if (tab) *tab++ = 0;
        ui_text(UI_BODY, x + 36, ly, C_DIM, lines[i]);
        if (tab) ui_text_right(UI_BODY, x + w - 36, ly, C_TEXT, tab);
        ly += lh;
    }
    if (!n) ui_text(UI_BODY, x + 36, ly, C_DIM, "No flying recorded yet.");
}

static void draw_slots(int saving)
{
    float x = 490, y = 150, w = 730, h = 470;
    ui_panel(x, y, w, h, 18, C_PANEL, pane == 1 ? C_EDGE : C_EDGE_D);
    ui_text(UI_BODY, x + 24, y + 14, C_DIM, saving ? "Choose a slot to save into" : "Choose a state to load");
    float cw = 222, chh = (h - 58) / (MENU_SLOTS / 3);
    for (int i = 0; i < MENU_SLOTS; i++) {
        float cx = x + 22 + (i % 3) * (cw + 15), cy = y + 50 + (i / 3) * chh;
        int f = pane == 1 && i == slot_focus;
        char when[64] = "";
        SDL_Texture *th = 0;
        int has = H->slot_info(i, when, sizeof when, &th);
        ui_rect(cx, cy, cw, chh - 6, f ? C_SEL : C_PANEL2);
        float tht = chh - 18, tw = tht * 128 / 90;
        ui_rect(cx + 6, cy + 6, tw, tht, C_DARK);
        if (has && th) ui_image(th, cx + 6, cy + 6, tw, tht);
        else ui_text_center(UI_SMALL, cx + 6 + tw / 2, cy + 6 + tht / 2 - 10, C_DIM, "Empty");
        char lab[32];
        if (i == 0) SDL_strlcpy(lab, "Quick", sizeof lab);
        else if (i >= MENU_SAVE_SLOTS) SDL_snprintf(lab, sizeof lab, "Auto %d", i - MENU_SAVE_SLOTS + 1);
        else SDL_snprintf(lab, sizeof lab, "Slot %d", i);
        ui_text(UI_BODY, cx + tw + 16, cy + 8, f ? C_ACCENT : C_TEXT, lab);
        if (has && when[0]) {
            /* when = "YYYY-MM-DD HH:MM" -> "Oct 22" / "10:46" */
            static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
            int yy = 0, mm = 1, dd = 0, hh = 0, mi = 0;
            sscanf(when, "%d-%d-%d %d:%d", &yy, &mm, &dd, &hh, &mi);
            char d[16], t[16];
            SDL_snprintf(d, sizeof d, "%s %d", mon[(mm - 1) % 12], dd);
            SDL_snprintf(t, sizeof t, "%02d:%02d", hh, mi);
            ui_text(UI_SMALL, cx + tw + 16, cy + 38, C_DIM, d);
            ui_text(UI_SMALL, cx + tw + 16, cy + 60, C_DIM, t);
        }
        if (f) ui_frame(cx, cy, cw, chh - 6, 2, C_ACCENT);
        add_hit(cx, cy, cw, chh - 6, 1, i, 0);
    }
}

static void value_text(item *it, char *buf, size_t n)
{
    if (it->kind == IT_CHOICE) SDL_strlcpy(buf, it->opts[*it->val], n);
    else if (it->kind == IT_SLIDER) SDL_snprintf(buf, n, "%d%s", *it->val, it->unit ? it->unit : "");
    else if (it->kind == IT_MODE) {
        if (*it->val < 0 || !H->mode_name) SDL_strlcpy(buf, "Desktop", n);
        else SDL_strlcpy(buf, H->mode_name(*it->val), n);
    } else buf[0] = 0;
}

static void draw_settings(void)
{
    float x = 490, y = 150, w = 730, h = 470;
    ui_panel(x, y, w, h, 18, C_PANEL, pane == 1 ? C_EDGE : C_EDGE_D);
    /* tabs (the cheats page has a plain header instead) */
    float tx = x + 24;
    if (main_focus == M_CHEATS) {
        ui_text(UI_BODY, tx + 6, y + 20, C_ACCENT, "Cheats");
        ui_text_right(UI_SMALL, x + w - 24, y + 26, C_DIM, !launcher_mode && H->game_title ? H->game_title : "");
        ui_rect(tx, y + 54, w - 48, 2, C_EDGE_D);
    }
    for (int t = 0; t < NTABS && main_focus != M_CHEATS; t++) {
        float tw = ui_text_w(UI_BODY, tabs[t].name) + 34;
        int f = t == tab;
        ui_rect(tx, y + 14, tw, 40, f ? C_SEL : C_PANEL2);
        if (f) ui_rect(tx, y + 50, tw, 4, C_ACCENT);
        ui_text_center(UI_BODY, tx + tw / 2, y + 20, f ? C_ACCENT : C_DIM, tabs[t].name);
        add_hit(tx, y + 14, tw, 40, 2, t, 0);
        tx += tw + 8;
    }
    if (main_focus == M_CHEATS) {
    } else if (input_pad_connected()) {
        char tl[48];
        SDL_snprintf(tl, sizeof tl, "%s / %s", input_pad_name(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), input_pad_name(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        ui_text_right(UI_SMALL, x + w - 24, y + 26, C_DIM, tl);
    } else {
        ui_text_right(UI_SMALL, x + w - 24, y + 26, C_DIM, "Q / E");
    }
    int nits;
    item *its = page(&nits);
    float step = (h - 122) / (float)nits, bh;
    if (step > 62) step = 62;
    bh = step - 8;                                   /* row box height; rows fit any count */
    float dy = (bh - 52) / 2;                        /* centre content in shorter rows */
    for (int i = 0; i < nits; i++) {
        item *it = &its[i];
        float iy = y + 70 + i * step + dy;
        int f = pane == 1 && i == sub_focus;
        if (f) { ui_rect(x + 14, iy - dy, w - 28, bh, C_SEL); ui_rect(x + 14, iy - dy, 5, bh, C_ACCENT); }
        ui_text(UI_BODY, x + 36, iy + 12, f ? C_ACCENT : C_TEXT, it->label);
        add_hit(x + 14, iy - dy, w - 28, bh, 1, i, 0);
        char v[96];
        value_text(it, v, sizeof v);
        float vx = x + w - 300, vw = 260;
        if (it->kind == IT_SLIDER) {
            float frac = (float)(*it->val - it->lo) / (float)(it->hi - it->lo);
            ui_rect(vx + 20, iy + 23, vw - 100, 6, C_PANEL2);
            ui_rect(vx + 20, iy + 23, (vw - 100) * frac, 6, f ? C_ACCENT : C_EDGE);
            ui_rect(vx + 20 + (vw - 100) * frac - 4, iy + 16, 8, 20, C_TEXT);
            ui_text_right(UI_BODY, vx + vw - 14, iy + 12, C_TEXT, v);
        } else if (it->kind != IT_BUTTON) {
            ui_text_center(UI_BODY, vx + vw / 2, iy + 12, C_TEXT, v);
        }
        if (it->kind != IT_BUTTON) {
            draw_arrow(vx - 4, iy + 18, 8, 1, f ? C_ACCENT : C_DIM);
            draw_arrow(vx + vw + 4, iy + 18, 8, 0, f ? C_ACCENT : C_DIM);
            add_hit(vx - 16, iy, 40, 52, 1, i, -1);
            add_hit(vx + vw - 16, iy, 40, 52, 1, i, +1);
        } else {
            draw_arrow(vx + vw + 4, iy + 18, 8, 0, f ? C_ACCENT : C_DIM);
        }
    }
    if (pane == 1 && sub_focus < nits && its[sub_focus].hint && *its[sub_focus].hint)
        ui_text(UI_SMALL, x + 24, y + h - 40, C_DIM, its[sub_focus].hint);
}

static void bind_text(int a, int col, char *buf, size_t n)
{
    int slot = col & 1;
    if (col < 2) {
        SDL_Scancode k = g_bind.key[a][slot];
        SDL_strlcpy(buf, k == SDL_SCANCODE_UNKNOWN ? "-" : input_key_name(k), n);
    } else {
        int c = g_bind.pad[a][slot];
        SDL_strlcpy(buf, c == PAD_NONE ? "-" : input_pad_name(c), n);
    }
}

static void draw_controls(void)
{
    float x = 490, y = 150, w = 730, h = 470;
    ui_panel(x, y, w, h, 18, C_PANEL, pane == 1 ? C_EDGE : C_EDGE_D);
    float c0 = x + 24, cw = 128, cx0 = x + 210;
    ui_text(UI_SMALL, c0, y + 14, C_DIM, "ACTION");
    ui_text_center(UI_SMALL, cx0 + cw, y + 14, C_DIM, "KEYBOARD");
    ui_text_center(UI_SMALL, cx0 + 3 * cw, y + 14, C_DIM, "GAMEPAD");
    ui_rect(x + 14, y + 38, w - 28, 1, C_EDGE_D);
    int rows = ACT_COUNT + 1, vis = 13;
    if (ctl_row < ctl_scroll) ctl_scroll = ctl_row;
    if (ctl_row >= ctl_scroll + vis) ctl_scroll = ctl_row - vis + 1;
    for (int r = ctl_scroll; r < rows && r < ctl_scroll + vis; r++) {
        float ry = y + 46 + (r - ctl_scroll) * 30;
        if (r == ACT_COUNT) {
            int f = pane == 1 && ctl_row == r;
            ui_rect(c0 - 10, ry + 2, 230, 26, f ? C_SEL : C_PANEL2);
            ui_text(UI_SMALL, c0, ry + 5, f ? C_ACCENT : C_TEXT, "Reset to Defaults");
            add_hit(c0 - 10, ry + 2, 230, 26, 1, 4 * ACT_COUNT, 0);
            continue;
        }
        if (r == ACT_PAD_COUNT) ui_rect(x + 14, ry - 2, w - 28, 1, C_EDGE_D);
        ui_text(UI_SMALL, c0, ry + 5, C_TEXT, act_label[r]);
        for (int c = 0; c < 4; c++) {
            float bx = cx0 + c * cw + 4;
            int f = pane == 1 && ctl_row == r && ctl_col == c;
            char b[48];
            bind_text(r, c, b, sizeof b);
            ui_rect(bx, ry + 2, cw - 8, 26, f ? C_SEL : C_PANEL2);
            if (f) ui_frame(bx, ry + 2, cw - 8, 26, 2, C_ACCENT);
            if (f && capturing) SDL_strlcpy(b, "...", sizeof b);
            ui_text_center(UI_SMALL, bx + (cw - 8) / 2, ry + 5, f ? C_ACCENT : (b[0] == '-' ? C_DIM : C_TEXT), b);
            add_hit(bx, ry + 2, cw - 8, 26, 1, r * 4 + c, 0);
        }
    }
    const char *hint = capturing
        ? (ctl_col >= 2 ? "Press a gamepad button, trigger or stick direction.  Esc cancels." :
                          "Press a key.  Esc cancels, Delete clears.")
        : "Select a box to rebind it. Delete / Backspace while waiting clears it.";
    ui_text(UI_SMALL, x + 24, y + h - 34, capturing ? C_ACCENT : C_DIM, hint);
}

static void draw_confirm(void)
{
    const char *q = confirm_kind == M_QUIT ? "Quit to desktop?" : confirm_kind == M_SWITCH ? "Switch to another game?" : "Restart the game?";
    const char *sub = "Progress since your last save state will be lost.";
    float w = 560, h = 210, x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    ui_dim(0.45f);
    ui_panel(x, y, w, h, 18, C_PANEL2, C_EDGE);
    ui_text_center(UI_LARGE, UI_W / 2, y + 28, C_TEXT, q);
    ui_text_center(UI_SMALL, UI_W / 2, y + 76, C_DIM, sub);
    const char *opt[2] = { "No", "Yes" };
    for (int i = 0; i < 2; i++) {
        float bx = UI_W / 2 - 150 + i * 160, by = y + 130;
        int f = confirm_yes == i;
        ui_rect(bx, by, 140, 48, f ? C_SEL : C_PANEL);
        ui_frame(bx, by, 140, 48, 2, f ? C_ACCENT : C_EDGE_D);
        ui_text_center(UI_BODY, bx + 70, by + 10, f ? C_ACCENT : C_TEXT, opt[i]);
        add_hit(bx, by, 140, 48, 9, i, 0);
    }
}

static void draw_prompts(void)
{
    char s[160];
    if (input_pad_connected())
        SDL_snprintf(s, sizeof s, "%s  Select      %s  Back      %s  Resume",
                     input_pad_confirm_label(), input_pad_back_label(), input_pad_name(SDL_GAMEPAD_BUTTON_BACK));
    else
        SDL_snprintf(s, sizeof s, "Enter  Select      Esc  Back      Arrows  Move");
    ui_text_right(UI_SMALL, 1220, 640, C_DIM, s);
    if (note[0] && SDL_GetTicksNS() < note_until) ui_text(UI_SMALL, 60, 640, C_ACCENT, note);
}

void menu_draw(int ow, int oh)
{
    if (!is_open) return;
    nhits = 0;
    ui_begin(ow, oh);
    ui_dim(0.62f);
    ui_text(UI_TITLE, 58, 52, C_TEXT, launcher_mode ? "SETTINGS" : "PAUSED");
    ui_rect(60, 120, 1160, 2, C_EDGE_D);
    ui_text_right(UI_BODY, 1220, 82, C_DIM, !launcher_mode && H->game_title ? H->game_title : "");
    draw_main();
    switch (main_focus) {
    case M_SAVE: draw_slots(1); break;
    case M_LOAD: draw_slots(0); break;
    case M_SETTINGS: case M_CHEATS: draw_settings(); break;
    case M_CONTROLS: draw_controls(); break;
    case M_STATS: draw_stats(); break;
    default: {
        float x = 490, y = 150;
        ui_panel(x, y, 730, 470, 18, ui_rgba(14, 17, 12, 150), C_EDGE_D);
        const char *t = main_focus == M_RESUME ? (launcher_mode ? "Back to the game picker." : "Back to the mission.") :
                        main_focus == M_RESTART ? "Start again from the EA logo." :
                        main_focus == M_SWITCH ? "Back to the collection to pick another game." : "Close Strike Collection HD.";
        ui_text(UI_BODY, x + 30, y + 30, C_DIM, t);
        if (!launcher_mode) ui_text(UI_SMALL, x + 30, y + 80, C_DIM, "F5 / F7 quick save and load also work in game.");
        break;
    }
    }
    draw_prompts();
    if (confirm_kind >= 0) draw_confirm();
}
