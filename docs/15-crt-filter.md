# CRT filter (prototype)

## What it is
Settings → **CRT** → **Filter** runs the [Lola Retro](https://github.com/zoran123456/lola-retro) presets
on the game picture: a simulation of the whole analog chain (the Mega Drive's video encoder, the cable, the
TV's decoder, the electron beam, the phosphor mask and the glass). Eight of its fifteen presets fit a
Genesis: Consumer TV (Composite), Consumer TV (RGB SCART), Trinitron TV (S-Video), Old TV (RF Antenna),
Studio Monitor (Composite), Studio Monitor (RGB), Commodore 1084S Monitor and CRT Projector. **Screen**
picks curved or flat glass.

They run as Lola Retro's **lite** chain: the same picture as its full presets (44–66 dB PSNR upstream)
for a fraction of the GPU time. The full chain cost an RTX 3090 four times the time at 4K for no visible
gain, and the super lite chain gives up a little of the picture to save nothing more on a desktop GPU, so
neither is offered.

## How it works
- The shaders are Lola Retro's GLSL port for Kodi (`third_party/lola-retro`, MIT), embedded in the program
  at build time by `cmake/embed_files.cmake`. Upstream generates the lite chain for RetroArch only; its
  GLSL was generated with upstream's own tool (see `third_party/lola-retro/README.md`).
- `src/host/crt.c` is a small RetroArch-style `.glslp` runner: passes, scale types, aliases, float
  framebuffers, `#pragma parameter` defaults and the preset's values. Each pass draws a quad into a
  framebuffer; the last pass draws into an SDL target texture that SDL then puts on screen, so the HUD,
  minimap and menus are drawn on top, unfiltered, as before. The same picture with the same options is
  not filtered again (menus, pause); the parameters go to the programs only when they change; every preset
  shares one chain of passes, so switching presets or the screen shape compiles nothing.
- **Screen: Flat** is the curved preset with `LR_CURV_X`, `LR_CURV_Y` and `LR_CORNER` at 0, exactly what
  upstream's flat presets change.
- It needs the OpenGL renderer: the GL calls go between SDL's (`SDL_FlushRenderer`, available since SDL
  3.2.0). With Display → Renderer on Automatic, OpenGL is chosen at startup while the filter is on (on
  Windows, Automatic is otherwise Direct3D). OpenGL 3.3 is the minimum.
- The input is the 1× picture, also when an HD pack is on: the tube makes its own detail.
- Two parameters are set by the game over the preset's: Console timing (`LR_CONSOLE`) 2, Mega Drive H40
  (3 for H32 screens), because Auto guesses from the frame size and a widescreen frame isn't 320 wide; and
  Picture height (`LR_FILL`) 1, so the 224 lines fill the picture height and the picture covers the same
  area as without the filter (the HUD and minimap are placed on that area).
- At most one filtered frame is in flight (a fence waited for before the next one): with a GPU slower than
  the game, frames would otherwise pile up and the picture lag.

## Options over the presets
The presets stay as Lola Retro made them; these options (CRT tab) change one or two parameters each:

| Option | Default | Parameters |
|---|---|---|
| Glass Glow | On | Off: `LR_GLOW` 0 (the halo the glass spreads around bright things) and `LR_REFLECT` 0 (the room's reflection) |
| Damper Wires | Off | On keeps `LR_DAMPER` as in the preset: the two wires that hold a Trinitron's aperture grille, at about 1/3 and 2/3 of the height (Trinitron and studio monitor presets; lola-retro issue #16). Off: 0 |
| Anti-Moire | On | `LR_MASK_PX` 1 (the mask on whole screen pixels, lola-retro issue #11) and `LR_SMOOTH` 1 (each screen pixel shows the light averaged over its height): no moire bands or dark patches where the mask or the lines beat against the screen's pixels. Off: the mask at the tube's size |
| Dither Blend | On | `LR_DBLEND_AUTO` 1: on the RGB and S-Video presets, 1-pixel dither patterns (water, sand, shadows) are blended as composite would have, without its rainbow. Off: as in the preset (0) |
| Mask | As the Tube | Half: `LR_MASK_STR` × 0.5; Off: 0. Below 4K the mask is finer than the screen's pixels and shimmers on moving detail (water) |

With the filter on, Scaling: Integer Only is ignored (the tube resamples the picture anyway), so the
picture fills the same area as with the other scaling modes.

## Widescreen
Kodi's port draws the tube the shape of the area it is given (it doesn't define `LR_FIT_43`), and the TV
model spreads the source line across the tube's width. So a widescreen frame (432 pixels at 16:9) becomes a
16:9 tube, like the widescreen CRT TVs of the late 1990s. Screens that stay 4:3 (menus, maps, cutscenes) are
already pillarboxed inside the frame by the host, so the tube keeps its shape.

The encoder's absolute width is meant for a 320-pixel line (1280 samples, 4 a pixel): a wider frame gets
it in proportion, so it keeps as many samples per pixel.

## Performance
Settings → Display → Show FPS → Diagnostics shows the GPU time of each pass (GPU timestamps read a few
frames later, so nothing stalls) and writes it to `diagnostics.log` in the settings folder.

RTX 3090 on Batocera, 3840×2133 (4K, 16:9), RGB SCART: 4.5 ms a frame (crt-lite 2.9, decode 1.2) with the
card at about 510 MHz, 38% load and 62 W. The full chain took 12.4 ms (its CRT pass alone 12.1, most likely
its reads of the 8192-wide beam texture: 15 MB, more than the card's 6 MB L2), 76% load and 310 W at
1970 MHz.

Intel UHD 630 (Mesa 26), 1920×1067: 72 ms a frame (crt-lite 47.5, decode 12.1, beam 8.9): too slow for 60 Hz.
Intel's compiler spills registers on the large passes (`INTEL_DEBUG=perf`); Lola Retro's README gives
2.2–3.4 ms for lite on an Intel Core Ultra 5 225U, so recent integrated graphics should have room to spare.

The shaders compile once per preset and are cached by the driver (the first start took several seconds on
Mesa, later ones under a second).

## Tested on
- Batocera 44-dev, X11 (openbox + picom), NVIDIA RTX 3090 (driver 610.57), 1080p and 4K at 60 Hz. In
  fullscreen a torn band stayed at a fixed height (seen only in motion, gone in a window), with sound cuts:
  VSync was on and honoured; the cause was picom, Batocera's compositor, copying the window on its own
  clock. A Batocera launcher therefore stops picom while the game runs and starts it again afterwards.
  Diagnostics shows a running compositor.
- Intel UHD 630 (Mesa): runs, too slow for full speed.
