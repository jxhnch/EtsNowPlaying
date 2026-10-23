# EtsNowPlaying

**See what you are listening to on your truck's GPS screen** – in Euro Truck Simulator 2 and American Truck Simulator.
The in-game **radio** (station, song, artist, cover) or **Spotify** (cover, title, artist, progress), and you can click
previous / pause / next right in the cabin.

*English · [Deutsch](README.de.md)*

<p align="center">
  <img src="docs/images/radio.png" alt="In-game radio with song cover" width="48%">
  <img src="docs/images/spotify.png" alt="Spotify with controls" width="48%">
</p>

## What you get

- **Radio:** the song, artist and cover of the station you hear in the game's radio, plus the station's logo.
- **Spotify:** cover, title, artist, album, a progress bar and play/pause – shown from the Windows media controls,
  so Spotify can stay in the background.
- **Switches by itself:** radio on → radio, radio off → Spotify.
- **Click to control:** previous, play/pause and next work with the mouse, right on the screen. Click or drag the progress bar to
  jump around in the song.
- **Fits every cabin:** the picture adapts to the shape of each vehicle's display by itself – no setup per truck.
- **One file.** Nothing else to install. Works with TruckersMP.

## Install

1. Close the game.
2. Download **`EtsNowPlaying.dll`** from the [Releases](../../releases) page.
3. Put it into the game's `plugins` folder:  
   `...\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\`
4. Start the game, load your profile and sit in the cabin. The new screen replaces the GPS display.  
   If you still see the normal GPS, restart the game once (the plugin sets itself up on the first start).

Remove other plugins that change the GPS screen first, they would get in each other's way.

## Use

- **Spotify:** just play something.
- **Radio:** switch on the radio in the game. Song titles come from the radio stream itself, so a few stations that do not
  send titles only show their name.
- **Buttons:** with the mouse pointer visible in the cabin (don't hold the right mouse button), click ⏮ ⏯ ⏭ on the screen.
  Click or drag on the progress bar to rewind / fast-forward (Spotify).

## Settings

`Documents\Euro Truck Simulator 2\EtsNowPlaying\EtsNowPlaying.ini` is created on the first start:

| Setting | Default | |
|---|---|---|
| `[screen] flip_vertical` | `1` | Set `0` if the picture is upside down. (Only used when `auto_fit=0`.) |
| `[screen] auto_fit` | `1` | Fit the picture to each vehicle's display. `0` = off. |
| `[media] app_filter` | `spotify` | Only show this app. Empty = any media player. |
| `[radio] cover_lookup` | `1` | Look up song covers and station logos online. `0` = off. |
| `[touch] enabled` | `1` | Mouse buttons on the screen. `0` = off. |

## Privacy

No tracking, no accounts. With `cover_lookup=1` the plugin sends the current radio *title* and the *station name*
to iTunes, Deezer and radio-browser.info to find a cover or logo. Nothing else leaves your PC. Set it to `0` to switch this off.

## Something wrong?

The plugin writes `Documents\Euro Truck Simulator 2\EtsNowPlaying\diag.log`. If you report a problem, please attach it.

## Build it yourself

You need the Visual Studio 2022 Build Tools (C++). Run `build.bat`; the plugin is `build\EtsNowPlaying.dll`.
Included: [MinHook](https://github.com/TsudaKageyu/minhook) and the SCS SDK headers, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer

Not affiliated with SCS Software, TruckersMP, Spotify, Apple or Deezer. The plugin changes game code in memory – use it at your
own risk and follow the rules of the servers you play on.

[MIT License](LICENSE)
