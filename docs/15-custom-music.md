# Custom music

The player's own audio files replace a game's songs (an idea from issue #8: CD-quality versions of the music, without supporting the unreleased Mega-CD build).

- **Where:** `<settings folder>/music/<game id>/`, made on start with a `README.txt` in `music/` that lists the songs (an older Desert-only README is rewritten once with Jungle's section). Desert's song requests come through the `$00890E` hook, Jungle's through `$0012EC` (below). Urban's through `$000822` (below).
- **Which file:** a file whose name starts with the song's number (`117 my title.ogg`, `100104.wav`), or whose whole name is the song's name (`title`, `story`, `refinery`, `bunker`, `briefing`, `campaign complete`, `mission failed`, `game over`, `ending`; spaces, underscores and hyphens are interchangeable, case doesn't matter). The number wins. 105 and 117 (title) and 108 and 10 (ending) stand in for each other. All the campaign briefings (100000 + id) fall back to `briefing`.
- **Formats:** `.ogg` (Vorbis, decoded with stb_vorbis, public domain, `third_party/stb/stb_vorbis.c` v1.22) and `.wav` (SDL). The whole file is decoded when the song starts and resampled to the mixer's rate with `SDL_ConvertAudioSamples`.
- **Order:** custom file, then the Amiga module (Music: Amiga), then the Genesis. Songs loop, except the jingles (mission failed, game over, campaign complete).
- **Settings > Audio:** Custom Music (on by default; a file has to be there for anything to change) and Custom Music Volume.
- Code: `src/host/musicpack.c`, `custom_music_play` and `write_music_readme` in `main.c`. Test: `DS_SFX_DEBUG=1` logs "music 105 -> custom <path>". Verified with a WAV and an OGG test tone (the audio dump peaks at the tone's frequency).

## Jungle Strike (Phase 3, 2026-11-12)
- Jungle sends every sound, songs and effects, as one command byte to the Z80 driver: `$0012EC` writes d0 to `$A01FFF` (callers `$03E598`, `$03E5A8`). A patch there calls `g_music_hook(byte)`: 1 skips the command (the bus request is released at `$001304`), 2 sends 0 (stop) instead.
- From the logs (ds_test `DS_SFX_LOG`, the gameplay and seed runs): 0 stops the music before each song; 1-47 are songs; 49 and up are effects, which the host leaves alone. Seen so far, with the screen at that moment: 20 the logos and title, 31 the story ("Somewhere in the Pacific"), 10 a campaign's opening ("The White House"), 25 in flight, 26 a short cue in flight, after which 25 comes back (so 26 plays once). Names: `title`, `story`, `mission`, `flying`. Nobody has listened yet, so the names are from the screens.
- No Amiga music in Jungle: custom file, else the Genesis.
- Test: a 440 Hz `flying.wav` in `music/jungle/`: `DS_SFX_DEBUG=1` logs "music 25 -> custom .../flying.wav".

## Urban Strike (Phase 3, 2026-11-12)
- Urban's sound requests go through `$007CDC` / `$007D02`: d0 below 63 is a song, handed to `$000822` (0 stops; it starts the sequence with driver command 4 through the driver interface at `$01135A`), 63 and up an effect (`$0007E0`). A patch at `$000822` calls `g_music_hook(d0)`: 1 skips the request (to its `rts`; nothing is pushed yet), 2 sends 0 (stop) instead.
- Seen so far: 14 the logos (EA, The Edge), 15 the title and menu, 7 the briefing ("Welcome back, commander"), stopped when the mission starts (Urban has no music in flight), 9 once late in a scripted run (not identified). Names: `intro`, `title`, `briefing`.
