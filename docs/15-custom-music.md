# Custom music

The player's own audio files replace a game's songs (an idea from issue #8: CD-quality versions of the music, without supporting the unreleased Mega-CD build).

- **Where:** `<settings folder>/music/<game id>/`, made on start with a `README.txt` in `music/` that lists the songs. Desert Strike only for now: the song requests come through the `$00890E` hook; Jungle and Urban Strike use a different sound engine and have no music hook yet.
- **Which file:** a file whose name starts with the song's number (`117 my title.ogg`, `100104.wav`), or whose whole name is the song's name (`title`, `story`, `refinery`, `bunker`, `briefing`, `campaign complete`, `mission failed`, `game over`, `ending`; spaces, underscores and hyphens are interchangeable, case doesn't matter). The number wins. 105 and 117 (title) and 108 and 10 (ending) stand in for each other. All the campaign briefings (100000 + id) fall back to `briefing`.
- **Formats:** `.ogg` (Vorbis, decoded with stb_vorbis, public domain, `third_party/stb/stb_vorbis.c` v1.22) and `.wav` (SDL). The whole file is decoded when the song starts and resampled to the mixer's rate with `SDL_ConvertAudioSamples`.
- **Order:** custom file, then the Amiga module (Music: Amiga), then the Genesis. Songs loop, except the jingles (mission failed, game over, campaign complete).
- **Settings > Audio:** Custom Music (on by default; a file has to be there for anything to change) and Custom Music Volume.
- Code: `src/host/musicpack.c`, `custom_music_play` and `write_music_readme` in `main.c`. Test: `DS_SFX_DEBUG=1` logs "music 105 -> custom <path>". Verified with a WAV and an OGG test tone (the audio dump peaks at the tone's frequency).
