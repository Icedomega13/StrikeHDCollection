# CRT display (Lola Retro)

**Settings > Picture > CRT** puts the game picture through [Lola Retro](https://github.com/zoran123456/lola-retro) (MIT, by Zoki), a shader chain that simulates the whole analog path: the console's video encoder, the cable, the TV's decoder, the electron beam, the phosphor mask and the glass. On composite, Desert Strike's dithering softens the way it did on a living-room TV.

- **CRT:** Off, TV (composite), TV (S-Video), Monitor (RGB). The three are the Mega Drive preset (`systems/flat/mega-drive-genesis.slangp`: console timing 2, the Mega Drive's encoder) with `LR_CABLE` 1, 2 or 3.
- **CRT Curvature:** the curved presets' glass (`LR_CURV_X` 0.045, `LR_CURV_Y` 0.06, `LR_CORNER` 0.045).
- The HUD, minimap and menus are drawn on top, sharp. Widescreen pictures go through the tube at their own shape (`LR_FIT_43` is off).

## How it runs
- The passes are the Kodi build of Lola Retro (`kodi/lola-retro/shaders`: RetroArch-style GLSL 3.30, exact-size textures), copied to `third_party/lola-retro` with the licence and the commit they came from (`VERSION`), and embedded by `tools/embed_crt.py` into `src/host/crt_shaders.c` (with the Mega Drive preset's values). All Kodi presets use the same eight passes; only their values differ.
- `src/host/crt.c` runs them on SDL's OpenGL renderer, in its context: `SDL_FlushRenderer`, then raw GL. It copies the frame to an exact-size texture, runs native, encode (1280 wide), decode, glow-h (1/8 x 1/2), glow-v, beam (8192 wide, 2.05x the lines), crt and present (the size of the picture on screen) into ping-pong targets, half-float except the last, and writes the last pass into an SDL render-target texture the host draws like any other. The aliases `LR_DecodedTexture` and `LR_GlowTexture` and `OrigTexture` are bound as the later passes ask for them; parameters come from the `#pragma parameter` defaults, then the preset. The renderer's GL state (program, framebuffer, VAO, viewport, texture units 0-1, blend, scissor) is saved and put back.
- **OpenGL only.** With the renderer on Auto, turning CRT on makes the next start use OpenGL (Windows otherwise picks Direct3D). If the renderer isn't OpenGL, or the passes don't compile, the game says so once and draws the picture normally.
- **Cost:** a graphics card handles it easily; Mesa's software OpenGL (cloud tests) manages about 3 frames a second.

## Testing in the cloud
The usual Linux build has no desktop OpenGL (no GL headers). `/tmp/build-gl` is configured with `-DSDL_OPENGL=ON -DSDL_X11=ON` and headers from libglvnd and the Khronos registries (`-I` to a local copy, libraries symlinked from `/usr/lib/x86_64-linux-gnu`), and runs under `Xvfb :99` with Mesa (OpenGL 4.5 compatibility profile on llvmpipe). Verified: composite and RGB, flat and curved, 4:3 title and widescreen gameplay.
