/* Keyboard and gamepad bindings. See input.h. */
#include <stdio.h>
#include <string.h>
#include "input.h"
#include "hw/genesis.h"

bindings g_bind;

static const char *act_names[ACT_COUNT] = {
    "up", "down", "left", "right", "a", "b", "c", "start",
    "save_state", "load_state", "fullscreen", "fast_forward", "pause", "menu",
    "objective_next", "objective_prev", "rewind",
    "strafe", "minimap_toggle",
};

const char *action_name(action a) { return act_names[a]; }

/* Gamepad button names in controls.ini. Positional names (south = the
 * bottom face button: A on Xbox, Cross on PlayStation, B on Nintendo). */
static const struct { const char *name; int code; } pad_names[] = {
    { "south", SDL_GAMEPAD_BUTTON_SOUTH }, { "east", SDL_GAMEPAD_BUTTON_EAST },
    { "west", SDL_GAMEPAD_BUTTON_WEST },   { "north", SDL_GAMEPAD_BUTTON_NORTH },
    { "back", SDL_GAMEPAD_BUTTON_BACK },   { "guide", SDL_GAMEPAD_BUTTON_GUIDE },
    { "start", SDL_GAMEPAD_BUTTON_START },
    { "leftstick", SDL_GAMEPAD_BUTTON_LEFT_STICK }, { "rightstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK },
    { "leftshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER }, { "rightshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
    { "dpup", SDL_GAMEPAD_BUTTON_DPAD_UP }, { "dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
    { "dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT }, { "dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    { "misc1", SDL_GAMEPAD_BUTTON_MISC1 }, { "touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD },
    { "+leftx", PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1) },   { "-leftx", PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0) },
    { "+lefty", PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1) },   { "-lefty", PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0) },
    { "+rightx", PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1) }, { "-rightx", PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0) },
    { "+righty", PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1) }, { "-righty", PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0) },
    { "lefttrigger", PAD_AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1) },
    { "righttrigger", PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1) },
};
#define N_PAD_NAMES (int)(sizeof pad_names / sizeof pad_names[0])

static const char *pad_code_name(int code)
{
    for (int i = 0; i < N_PAD_NAMES; i++) if (pad_names[i].code == code) return pad_names[i].name;
    return 0;
}

static int pad_code_from_name(const char *s)
{
    for (int i = 0; i < N_PAD_NAMES; i++) if (!SDL_strcasecmp(pad_names[i].name, s)) return pad_names[i].code;
    return PAD_NONE;
}

static void set(bindings *b, action a, SDL_Scancode k0, SDL_Scancode k1, int p0, int p1)
{
    b->key[a][0] = k0; b->key[a][1] = k1;
    b->pad[a][0] = p0; b->pad[a][1] = p1;
}

void input_defaults(bindings *b)
{
    memset(b->key, 0, sizeof b->key);
    for (int a = 0; a < ACT_COUNT; a++) for (int i = 0; i < MAX_BINDS; i++) b->pad[a][i] = PAD_NONE;
    set(b, ACT_UP,    SDL_SCANCODE_UP,    SDL_SCANCODE_W, SDL_GAMEPAD_BUTTON_DPAD_UP,    PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0));
    set(b, ACT_DOWN,  SDL_SCANCODE_DOWN,  SDL_SCANCODE_S, SDL_GAMEPAD_BUTTON_DPAD_DOWN,  PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1));
    set(b, ACT_LEFT,  SDL_SCANCODE_LEFT,  SDL_SCANCODE_A, SDL_GAMEPAD_BUTTON_DPAD_LEFT,  PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0));
    set(b, ACT_RIGHT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1));
    /* Genesis A B C sit left to right, like west / south / east on a modern pad */
    set(b, ACT_A, SDL_SCANCODE_J, SDL_SCANCODE_Z, SDL_GAMEPAD_BUTTON_WEST,  PAD_NONE);
    set(b, ACT_B, SDL_SCANCODE_K, SDL_SCANCODE_X, SDL_GAMEPAD_BUTTON_SOUTH, PAD_NONE);
    set(b, ACT_C, SDL_SCANCODE_L, SDL_SCANCODE_C, SDL_GAMEPAD_BUTTON_EAST,  PAD_NONE);
    set(b, ACT_START, SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_START, PAD_NONE);
    set(b, ACT_SAVE_STATE, SDL_SCANCODE_F5, SDL_SCANCODE_UNKNOWN, PAD_NONE, PAD_NONE);
    set(b, ACT_LOAD_STATE, SDL_SCANCODE_F7, SDL_SCANCODE_UNKNOWN, PAD_NONE, PAD_NONE);
    set(b, ACT_FULLSCREEN, SDL_SCANCODE_F11, SDL_SCANCODE_UNKNOWN, PAD_NONE, PAD_NONE);
    set(b, ACT_FAST_FORWARD, SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1), PAD_NONE);
    set(b, ACT_PAUSE, SDL_SCANCODE_P, SDL_SCANCODE_PAUSE, PAD_NONE, PAD_NONE);
    set(b, ACT_MENU, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_BACK, PAD_NONE);
    set(b, ACT_MM_NEXT, SDL_SCANCODE_E, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_NONE);
    set(b, ACT_MM_PREV, SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_NONE);
    set(b, ACT_REWIND, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UNKNOWN, PAD_AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), PAD_NONE);
    set(b, ACT_STRAFE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_NORTH, PAD_NONE);
    set(b, ACT_MM_TOGGLE, SDL_SCANCODE_M, SDL_SCANCODE_UNKNOWN, PAD_NONE, PAD_NONE);
    b->stick_deadzone = 12000;
}

/* ---------------- controls.ini ---------------- */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

int input_load(const char *path)
{
    input_defaults(&g_bind);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int seen_mm = 0;
    char line[512], section[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; SDL_strlcpy(section, s + 1, sizeof section); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcmp(section, "options")) {
            if (!strcmp(key, "stick_deadzone")) g_bind.stick_deadzone = SDL_atoi(val);
            continue;
        }
        int a = -1;
        for (int i = 0; i < ACT_COUNT; i++) if (!strcmp(key, act_names[i])) a = i;
        if (a < 0) continue;
        if (a == ACT_MM_NEXT || a == ACT_MM_PREV) seen_mm = 1;
        int is_kb = !strcmp(section, "keyboard"), is_pad = !strcmp(section, "gamepad");
        if (!is_kb && !is_pad) continue;
        /* a listed action replaces its defaults; an empty value unbinds it */
        for (int i = 0; i < MAX_BINDS; i++) {
            if (is_kb) g_bind.key[a][i] = SDL_SCANCODE_UNKNOWN; else g_bind.pad[a][i] = PAD_NONE;
        }
        int n = 0;
        char *save = 0;
        for (char *tok = SDL_strtok_r(val, ",", &save); tok && n < MAX_BINDS; tok = SDL_strtok_r(0, ",", &save)) {
            tok = trim(tok);
            if (!*tok) continue;
            if (is_kb) {
                SDL_Scancode sc = SDL_GetScancodeFromName(tok);
                if (sc != SDL_SCANCODE_UNKNOWN) g_bind.key[a][n++] = sc;
            } else {
                int c = pad_code_from_name(tok);
                if (c != PAD_NONE) g_bind.pad[a][n++] = c;
            }
        }
    }
    fclose(f);
    /* Files from before the minimap had LB / RB as second A / C buttons;
     * they now step through the objectives. */
    if (!seen_mm) {
        for (int a = ACT_A; a <= ACT_C; a++)
            for (int i = 0; i < MAX_BINDS; i++)
                if (g_bind.pad[a][i] == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || g_bind.pad[a][i] == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
                    g_bind.pad[a][i] = PAD_NONE;
    }
    return 1;
}

int input_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Desert Strike HD controls. Several bindings per action, comma separated.\n"
               "# Key names are SDL key names (Up, Return, Left Shift, F5, ...).\n"
               "# Gamepad names are positional: south/east/west/north face buttons,\n"
               "# dpup..dpright, leftshoulder, lefttrigger, +leftx/-leftx, +lefty/-lefty, start, back.\n\n");
    fprintf(f, "[keyboard]\n");
    for (int a = 0; a < ACT_COUNT; a++) {
        fprintf(f, "%s = ", act_names[a]);
        int first = 1;
        for (int i = 0; i < MAX_BINDS; i++) if (g_bind.key[a][i] != SDL_SCANCODE_UNKNOWN) {
            fprintf(f, "%s%s", first ? "" : ", ", SDL_GetScancodeName(g_bind.key[a][i]));
            first = 0;
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n[gamepad]\n");
    for (int a = 0; a < ACT_COUNT; a++) {
        fprintf(f, "%s = ", act_names[a]);
        int first = 1;
        for (int i = 0; i < MAX_BINDS; i++) if (g_bind.pad[a][i] != PAD_NONE) {
            const char *n = pad_code_name(g_bind.pad[a][i]);
            if (n) { fprintf(f, "%s%s", first ? "" : ", ", n); first = 0; }
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n[options]\nstick_deadzone = %d\n", g_bind.stick_deadzone);
    fclose(f);
    return 1;
}

/* ---------------- runtime state ---------------- */
#define MAX_PADS 8
static SDL_Gamepad *pads[MAX_PADS];
static int pressed[ACT_COUNT];

void input_init(void)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && i < MAX_PADS; i++) pads[i] = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

static void pad_added(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return;
    for (int i = 0; i < MAX_PADS; i++) if (!pads[i]) { pads[i] = SDL_OpenGamepad(id); return; }
}

static void pad_removed(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id) { SDL_CloseGamepad(pads[i]); pads[i] = 0; }
}

static int pad_input_down(SDL_Gamepad *p, int code)
{
    if (code == PAD_NONE) return 0;
    if (code < PAD_AXIS_BASE) return SDL_GetGamepadButton(p, (SDL_GamepadButton)code);
    int axis = (code - PAD_AXIS_BASE) / 2, pos = (code - PAD_AXIS_BASE) & 1;
    int v = SDL_GetGamepadAxis(p, (SDL_GamepadAxis)axis);
    int dz = g_bind.stick_deadzone;
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) dz = 8000;
    return pos ? v > dz : v < -dz;
}

int input_held(action a)
{
    const bool *ks = SDL_GetKeyboardState(0);
    for (int i = 0; i < MAX_BINDS; i++) {
        SDL_Scancode k = g_bind.key[a][i];
        if (k != SDL_SCANCODE_UNKNOWN && ks[k]) return 1;
    }
    for (int p = 0; p < MAX_PADS; p++) {
        if (!pads[p]) continue;
        for (int i = 0; i < MAX_BINDS; i++) if (pad_input_down(pads[p], g_bind.pad[a][i])) return 1;
    }
    return 0;
}

static int cap_active, cap_pad, cap_result, cap_code;
static uint64_t cap_start;

void input_capture_start(int pad)
{
    cap_active = 1; cap_pad = pad; cap_result = 0; cap_code = 0;
    cap_start = SDL_GetTicksNS();
}

int input_capturing(void) { return cap_active; }

int input_capture_poll(int *code)
{
    if (cap_active && cap_pad && SDL_GetTicksNS() - cap_start > 6000000000ull) { cap_active = 0; cap_result = -1; }
    if (cap_active || !cap_result) return 0;
    int r = cap_result;
    *code = cap_code;
    cap_result = 0;
    return r;
}

static void capture_event(const SDL_Event *e)
{
    uint64_t dt = SDL_GetTicksNS() - cap_start;
    if (dt < 150000000ull) return;                     /* ignore the press that started it */
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        SDL_Scancode k = e->key.scancode;
        if (k == SDL_SCANCODE_ESCAPE) { cap_result = -1; cap_active = 0; return; }
        if (k == SDL_SCANCODE_DELETE || k == SDL_SCANCODE_BACKSPACE) { cap_result = 2; cap_active = 0; return; }
        if (!cap_pad) { cap_code = k; cap_result = 1; cap_active = 0; }
        return;
    }
    if (!cap_pad) return;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        cap_code = e->gbutton.button; cap_result = 1; cap_active = 0;
    } else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        int v = e->gaxis.value;
        if (v > 24000 || v < -24000) {
            int trig = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            cap_code = PAD_AXIS(e->gaxis.axis, trig ? 1 : v > 0);
            cap_result = 1; cap_active = 0;
        }
    }
}

void input_event(const SDL_Event *e)
{
    if (cap_active) {
        if (e->type == SDL_EVENT_GAMEPAD_ADDED) pad_added(e->gdevice.which);
        else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) pad_removed(e->gdevice.which);
        else capture_event(e);
        return;
    }
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: pad_added(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e->gdevice.which); break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) break;
        for (int a = ACT_PAD_COUNT; a < ACT_COUNT; a++)
            for (int i = 0; i < MAX_BINDS; i++)
                if (g_bind.key[a][i] == e->key.scancode) pressed[a] = 1;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        for (int a = ACT_PAD_COUNT; a < ACT_COUNT; a++)
            for (int i = 0; i < MAX_BINDS; i++)
                if (g_bind.pad[a][i] == e->gbutton.button) pressed[a] = 1;
        break;
    default: break;
    }
}

int input_pressed(action a)
{
    int r = pressed[a];
    pressed[a] = 0;
    return r;
}

void input_clear_pressed(void) { memset(pressed, 0, sizeof pressed); }

int input_pad_connected(void)
{
    for (int i = 0; i < MAX_PADS; i++) if (pads[i]) return 1;
    return 0;
}

static SDL_GamepadType pad_type(void)
{
    for (int i = 0; i < MAX_PADS; i++) if (pads[i]) return SDL_GetGamepadType(pads[i]);
    return SDL_GAMEPAD_TYPE_XBOXONE;
}

static int is_ps(SDL_GamepadType t)
{
    return t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
}

static int is_nin(SDL_GamepadType t)
{
    return t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR ||
           t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
}

const char *input_key_name(SDL_Scancode k)
{
    const char *n = SDL_GetScancodeName(k);
    return n && *n ? n : "?";
}

const char *input_pad_name(int code)
{
    SDL_GamepadType t = pad_type();
    int ps = is_ps(t), nin = is_nin(t);
    switch (code) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ps ? "Cross" : nin ? "B" : "A";
    case SDL_GAMEPAD_BUTTON_EAST:  return ps ? "Circle" : nin ? "A" : "B";
    case SDL_GAMEPAD_BUTTON_WEST:  return ps ? "Square" : nin ? "Y" : "X";
    case SDL_GAMEPAD_BUTTON_NORTH: return ps ? "Triangle" : nin ? "X" : "Y";
    case SDL_GAMEPAD_BUTTON_BACK:  return ps ? "Share" : nin ? "Minus" : "View";
    case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : nin ? "Plus" : "Menu";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS click";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS click";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nin ? "L" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nin ? "R" : "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad Right";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    case SDL_GAMEPAD_BUTTON_MISC1: return "Capture";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0): return "L-stick Left";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1): return "L-stick Right";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0): return "L-stick Up";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1): return "L-stick Down";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0): return "R-stick Left";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1): return "R-stick Right";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0): return "R-stick Up";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1): return "R-stick Down";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1): return ps ? "L2" : nin ? "ZL" : "LT";
    case PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1): return ps ? "R2" : nin ? "ZR" : "RT";
    }
    return "?";
}

const char *input_pad_confirm_label(void) { return input_pad_name(SDL_GAMEPAD_BUTTON_SOUTH); }
const char *input_pad_back_label(void) { return input_pad_name(SDL_GAMEPAD_BUTTON_EAST); }

unsigned input_pad_state(void)
{
    static const unsigned bits[ACT_PAD_COUNT] = { GB_UP, GB_DOWN, GB_LEFT, GB_RIGHT, GB_A, GB_B, GB_C, GB_START };
    unsigned s = 0;
    for (int a = 0; a < ACT_PAD_COUNT; a++) if (input_held((action)a)) s |= bits[a];
    /* Strafe: Desert Strike strafes while A is held, and a tap of A (released
     * within 15 frames) fires a Hellfire. The strafe button holds A for at
     * least 16 frames, so it never fires. */
    static uint64_t strafe_from;
    static int strafing;
    if (input_held(ACT_STRAFE)) {
        if (!strafing) { strafing = 1; strafe_from = g_frame; }
        s |= GB_A;
    } else if (strafing) {
        if (g_frame - strafe_from < 16) s |= GB_A;
        else strafing = 0;
    }
    /* a pad cannot press opposite directions at once */
    if ((s & (GB_UP | GB_DOWN)) == (GB_UP | GB_DOWN)) s &= ~(unsigned)(GB_UP | GB_DOWN);
    if ((s & (GB_LEFT | GB_RIGHT)) == (GB_LEFT | GB_RIGHT)) s &= ~(unsigned)(GB_LEFT | GB_RIGHT);
    return s;
}

void input_rumble(float lo, float hi, int ms)
{
    if (lo < 0) lo = 0;
    if (lo > 1) lo = 1;
    if (hi < 0) hi = 0;
    if (hi > 1) hi = 1;
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i]) SDL_RumbleGamepad(pads[i], (Uint16)(lo * 65535), (Uint16)(hi * 65535), (Uint32)ms);
}
