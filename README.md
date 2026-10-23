# EtsNowPlaying

**A Now Playing screen for the cabin of Euro Truck Simulator 2 and American Truck Simulator.**
Your truck's GPS display shows what you are listening to: the **in-game internet radio** (station, song, artist, cover)
or **Spotify** (cover, title, artist, progress) – read through the Windows media controls, not by filming a window.

*English · [Deutsch](README.de.md)*

<p align="center">
  <img src="docs/images/radio-cover.png" alt="In-game radio with song cover" width="48%">
  <img src="docs/images/radio-loading.png" alt="Station without track info" width="48%">
</p>

## Features

- **Radio:** shows the song, artist and album cover of the station you are listening to in the game's own radio.
  The title comes straight from the stream the game is playing, so it always matches what you hear.
- **Spotify (and friends):** cover, title, artist, album, progress bar with time and play/pause state – via the
  Windows media controls (SMTC), so the Spotify window can stay hidden.
- **Automatic switching:** radio on → radio screen, radio off or paused → back to Spotify. Station changes do not flicker.
- **Cover-driven design:** the background is the blurred cover, accent colours are picked from it.
- **One file:** a single DLL. No helper app, no extra installs, no .NET or C++ runtime needed.
- **Safe:** if the game ever crashes, the plugin switches parts of itself off step by step on the next start.
- **Tested with TruckersMP** as well as single-player. Everything runs on your machine.

## Requirements

- Windows 10 or 11
- Euro Truck Simulator 2 (tested with 1.61.1.9, DirectX 11) – American Truck Simulator shares the engine but is untested
- Spotify desktop app (optional; any app that shows up in the Windows media overlay works, see `app_filter`)

## Installation

1. Close the game.
2. Get `EtsNowPlaying.dll` from the [Releases](../../releases) page once a release is published, or [build it yourself](#building).
3. Copy it to the game's plugin folder, usually  
   `C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\`
4. Start the game, load your profile and sit in the cabin. On the very first start the plugin creates two small files in  
   `Documents\Euro Truck Simulator 2\EtsNowPlaying\` – if the screen is still the stock GPS, restart the game once.

If you used another GPS-screen plugin (for example *PrismTextureStreamer*), remove it first: both would fight over the same texture.

The screen replaces the **GPS** display of the truck. Radio stations come from your `live_streams.sii`, so custom stations
work, too.

## Configuration

Settings live in `Documents\Euro Truck Simulator 2\EtsNowPlaying\EtsNowPlaying.ini` (created on first start).

| Section / key | Default | Meaning |
|---|---|---|
| `[screen] flip_vertical` | `1` | Set to `0` if the picture is upside down on your truck. |
| `[media] app_filter` | `spotify` | Only show media from apps whose name contains this text. Empty = any player. |
| `[radio] cover_lookup` | `1` | Look up song covers for radio titles (see [Privacy](#privacy)). `0` turns it off. |
| `[radio] pause_after_ms` | `2500` | No stream data for this long means the radio is paused (adapts to the stream). |
| `[radio] switch_hold_ms` | `600` | Keep the radio screen this long after a stream ends, so station changes do not flash Spotify. |
| `[features] force_stage` | `-1` | `-1` = automatic crash fallback. `0..4` forces a [stage](#safe-mode). |
| `[beta] touch` | `1` | Mouse control of the screen (this branch only, see [Beta: touch control](#beta-touch-control)). |

## How it works

The plugin is a normal SCS telemetry plugin that does four things inside the game process:

1. **Screen texture.** The game's texture loader is asked for the GPS texture; the plugin redirects that request to a tiny
   placeholder (a 64×2048 image it writes itself). When the game binds the placeholder to a shader, the plugin swaps in its own
   texture, which it fills with a Direct2D / DirectWrite picture. The game's own GPS UI is switched off.
2. **Radio.** The game's audio engine (FMOD) opens radio streams over plain HTTP sockets. The plugin watches those
   connections, matches the URL against your `live_streams.sii` and reads the ICY metadata (`StreamTitle`) out of the stream.
   Nothing is injected into the stream and nothing extra is downloaded for it.
3. **Spotify.** The Windows *System Media Transport Controls* API provides title, artist, cover and position.
4. **Covers for radio songs.** If the station sends `Artist - Song`, the plugin asks the iTunes Search API for the cover.

Engine hooks are found by byte signatures, so they survive most game updates. If a signature stops matching after a big update,
the plugin simply stays passive and writes the reason to its log.

## Beta: touch control

> **Experimental – this is the `beta/touch` branch.** It may misbehave on your setup; the crash fallback turns it off first.

With the mouse cursor visible in the cabin (do not hold the right mouse button, that is look-around) you can click the
**⏮ ⏯ ⏭** buttons on the Spotify screen. A small ring on the screen shows where the plugin thinks the cursor is.

How it works: while the game draws the screen mesh, the plugin repeats that draw call into a 1×1 render target that only covers
the pixel under the mouse cursor, using a probe texture whose texels contain their own texture coordinates. Reading the pixel back
gives the position on the screen. The buttons then talk to the player through the Windows media controls.

Turn it off with `[beta] touch=0`. If something looks off, please send `diag.log` (lines starting with `TOUCH`).
## Safe mode

Hooking a game can go wrong on some driver/game combinations. The plugin remembers whether the last session ended cleanly.
After a crash it takes one step back on the next start:

| Stage | What is on |
|---|---|
| 0 | everything |
| 1 | no touch input (beta) |
| 2 | additionally no frame upload (screen texture swapped, but not filled) |
| 3 | additionally the game's own GPS UI stays on |
| 4 | display off, only logging |

A clean exit keeps the stage; delete `state.ini` (or set `force_stage=0`) to go back to stage 0.
If you hit this, please attach `diag.log` and `diag.prev.log` to an issue.

## Privacy

- No telemetry, no accounts. The plugin never sends your listening history anywhere.
- With `cover_lookup=1` it sends the current radio **title** (e.g. `Artist - Song`) to `itunes.apple.com` and downloads the artwork
  from Apple's image servers. Turn it off with `cover_lookup=0`.
- Everything else (radio titles, Spotify info) stays on your machine. Logs are local files in the plugin's documents folder.

## Troubleshooting

- **Screen is the stock GPS / black:** restart the game once; check `diag.log` for lines starting with `ENGINE` and `SCREEN`.
- **Upside down or mirrored:** change `flip_vertical` in the ini.
- **Radio shows the station name instead of a song:** many stations do not send titles (or only after a few seconds). The screen
  says so. The plugin can only show what the stream contains.
- **Radio is not detected at all:** `diag.log` shows every radio connection (`RADIO request/response`).
- **Spotify is not shown:** the media overlay of Windows must list it; check `[media] app_filter`.

## Building

Needs the Visual Studio 2022 Build Tools (C++ workload, CMake and Ninja are included).

```bat
build.bat
```

produces `build\EtsNowPlaying.dll`. `deploy.ps1` copies it into the game's plugin folder
(`-DisableOld` moves a previous screen plugin away, `-Uninstall` removes the plugin again).

Dependencies are vendored: [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause) and the
[SCS Software SDK](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry) headers.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Branches

- `main` – stable.
- `beta/touch` – experimental: operate the screen with the mouse inside the cabin.

## Disclaimer

Not affiliated with or endorsed by SCS Software, TruckersMP, Spotify or Apple. The plugin patches game code in memory; use it at
your own risk, especially in multiplayer, and follow the rules of the servers you play on.

## License

[MIT](LICENSE)
