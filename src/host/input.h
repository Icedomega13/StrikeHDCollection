/*
 * Input bindings. Every game button and every host hotkey ("action") can be
 * bound to several keyboard keys and several gamepad inputs. Bindings are
 * stored in controls.ini in the user's settings folder, so they can be
 * edited by hand today and by the in-game rebinding menu later.
 */
#ifndef DS_INPUT_H
#define DS_INPUT_H
#include <SDL3/SDL.h>

typedef enum {
    /* Genesis pad */
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_A, ACT_B, ACT_C, ACT_START,
    /* host */
    ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_FULLSCREEN, ACT_FAST_FORWARD, ACT_PAUSE, ACT_MENU,
    ACT_MM_NEXT, ACT_MM_PREV, ACT_REWIND,
    ACT_STRAFE, ACT_MM_TOGGLE,
    ACT_COUNT
} action;

#define ACT_PAD_COUNT 8
#define MAX_BINDS 4

/* Gamepad input codes: 0..SDL_GAMEPAD_BUTTON_COUNT-1 are buttons;
 * PAD_AXIS(axis, positive) are stick/trigger directions. */
#define PAD_AXIS_BASE 1000
#define PAD_AXIS(axis, pos) (PAD_AXIS_BASE + (axis) * 2 + ((pos) ? 1 : 0))
#define PAD_NONE (-1)

typedef struct {
    SDL_Scancode key[ACT_COUNT][MAX_BINDS];   /* SDL_SCANCODE_UNKNOWN = unused */
    int pad[ACT_COUNT][MAX_BINDS];            /* PAD_NONE = unused */
    int stick_deadzone;                       /* 0-32767 */
} bindings;

extern bindings g_bind;

const char *action_name(action a);          /* config key, e.g. "save_state" */
void input_defaults(bindings *b);
int  input_load(const char *path);          /* 1 if file existed */
int  input_save(const char *path);

void input_init(void);                      /* open gamepads already connected */
void input_event(const SDL_Event *e);       /* hot-plug + edge-triggered actions */
/* Current state of the 8 Genesis buttons as GB_* bits. */
unsigned input_pad_state(void);
int  input_held(action a);
/* Returns 1 once per press of a host action (save state, fullscreen, ...). */
int  input_pressed(action a);
extern int input_rstick_strafe;   /* right stick left/right strafes (Desert Strike); main.c sets it from the settings */
void input_clear_pressed(void);

/* Rebinding. Start a capture for a keyboard key (pad = 0) or a gamepad
 * input (pad = 1); feed events through input_event() as usual; poll until
 * it returns non-zero: 1 = captured (*code set), -1 = cancelled,
 * 2 = clear requested (Delete / Backspace). */
void input_capture_start(int pad);
int  input_capture_poll(int *code);
int  input_capturing(void);

/* Human-readable names for the menu. Gamepad names follow the type of the
 * first connected pad (Xbox / PlayStation / Nintendo), else Xbox style. */
const char *input_key_name(SDL_Scancode k);
const char *input_pad_name(int code);
/* "A"/"Cross" etc for the confirm / back face buttons, for on-screen prompts. */
const char *input_pad_confirm_label(void);
const char *input_pad_back_label(void);
int  input_pad_connected(void);
/* Rumble every connected pad: low / high frequency motors 0..1, for ms. */
void input_rumble(float lo, float hi, int ms);

#endif
