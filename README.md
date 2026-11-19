# EtsNowPlaying

**See what you are listening to on your truck's GPS screen** – in Euro Truck Simulator 2 and American Truck Simulator.
The in-game **radio** (station, song, artist, cover or logo) or **Spotify** (cover, title, artist, progress), and you can click
previous / pause / next right in the cabin.

*English · [Deutsch](README.de.md)*

<p align="center">
  <img src="docs/images/radio.png" alt="In-game radio with song cover" width="48%">
  <img src="docs/images/spotify.png" alt="Spotify with controls" width="48%">
</p>

## What you get

- **Radio:** the song and artist of the station you hear in the game's radio, with the song's cover – or the station's logo,
  or your own picture for it.
- **Spotify** (and other players): cover, title, artist, album, a progress bar and play/pause, taken from the Windows media
  controls, so the player can stay in the background. Apple Music, Deezer, Tidal and Amazon Music show their names.
- **Switches by itself:** radio on → radio, radio off → music player.
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

- **Spotify:** just play something. Other players: set `app_filter` (see Settings).
- **Radio:** switch on the radio in the game. Song titles come from the radio stream itself, so a few stations that do not
  send titles only show their name (see also *Optional: extra song sources*).
- **Buttons:** with the mouse pointer visible in the cabin (don't hold the right mouse button), click ⏮ ⏯ ⏭ on the screen.
  Click or drag on the progress bar to rewind / fast-forward (Spotify).

## Settings

`Documents\Euro Truck Simulator 2\EtsNowPlaying\EtsNowPlaying.ini` is created on the first start:

| Setting | Default | |
|---|---|---|
| `[screen] auto_fit` | `1` | Fit the picture to each vehicle's display. `0` = off. |
| `[screen] flip_vertical` | `1` | Set `0` if the picture is upside down. (Only used when `auto_fit=0`.) |
| `[media] app_filter` | `spotify` | Only show this app (part of its name). Empty = any media player. |
| `[radio] cover_lookup` | `1` | Look up song covers and station logos online. `0` = off. |
| `[radio] cover_mode` | `cover` | `cover` = song covers (the station's logo when none is found). `logo` = always the station's logo, never a song cover. |
| `[radio] extra_sources` | `1` | Use the optional file `EtsNowPlaying.sources.ini` (see below). `0` = never. |
| `[touch] enabled` | `1` | Mouse buttons on the screen. `0` = off. |

## Covers, logos and your own pictures

For radio the plugin shows, in this order: the song's **cover** (found online by artist and title; short titles must match
exactly, so unrelated covers are rare), otherwise the station's **logo** (from radio-browser.info), otherwise the station's
**initials**. With `cover_mode=logo` the cover step is skipped and the station's logo is always shown.

A station without a logo, or with a wrong one, can get **your own picture**: put a file named like the station into
`Documents\Euro Truck Simulator 2\EtsNowPlaying\logos\custom\`, for example `Real Hardstyle.png` (the name is the station name
from your `live_streams.sii`, upper/lower case does not matter; PNG, JPG, WEBP, GIF, BMP and ICO work). Your picture wins over
the logo from radio-browser.info, is read from your PC only and works with `cover_lookup=0`. Restart the game after adding a
file. The plugin creates the folder, with a short note, on the first start.

## Optional: extra song sources

A few stations only send the name of a *show* in their stream, not the song (Real Hardstyle Radio does this during its shows).
If such a station also offers the current song on a web address, it can be listed in
`Documents\Euro Truck Simulator 2\EtsNowPlaying\EtsNowPlaying.sources.ini`. The plugin creates this file on the first start,
**it is optional** and comes with one entry for Real Hardstyle Radio. For every entry you say how to recognise the station
(`match`), where to ask (`url`) and where the song is inside the answer (`title`, optionally `artist`):

```ini
[Real Hardstyle]
enabled=1
match=realhardstyle|real hardstyle
url=https://api.realhardstyle.com/get/nowplaying
title=response.activeSession.rawTitle
interval=10
```

The plugin asks that address only while the station is playing (every 10 seconds by default). If the address gives no song,
or cannot be reached, the title from the stream is shown as usual. If the stream already carries the song, its artist is kept.
Set `enabled=0` for an entry, or `extra_sources=0` in the INI to switch it all off.

*Real Hardstyle in the game's station list:* the game cannot follow the https redirect of `https://stream.realhardstyle.nl/`.
If the station does not play, use `http://server-23.stream-server.nl:8326/` as its address in `live_streams.sii`.

## Something wrong?

The plugin writes `Documents\Euro Truck Simulator 2\EtsNowPlaying\diag.log`. If you report a problem, please attach it.

**After a game update:** a new game version can move something the plugin relies on. Then the plugin leaves (or puts back) the
game's normal GPS and tells you with a small window ("does not support this game version yet", shown once per game version). Check the
[Releases](../../releases) page for a plugin update; if there is none yet, please open an issue and attach the log.

## Build it yourself

You need the Visual Studio 2022 Build Tools (C++). Run `build.bat`; the plugin is `build\EtsNowPlaying.dll`.
Included: [MinHook](https://github.com/TsudaKageyu/minhook) and the SCS SDK headers, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Privacy Policy & Terms of Use

**Privacy.** EtsNowPlaying has no accounts, no analytics and no telemetry of its own. It reads what is playing from the game's
radio stream and the Windows media controls on your PC. With `[radio] cover_lookup=1` (the default) it sends the **current radio
song title and the station name** to **iTunes Search (Apple), Deezer and radio-browser.info** to find a cover or logo; like any web
request, these services see your IP address. Nothing else is sent: no personal data, no game data, no Spotify data. Set
`cover_lookup=0` to switch off all online requests; your own pictures in `logos\custom` never leave your PC. Logos are cached in
`Documents\Euro Truck Simulator 2\EtsNowPlaying\logos`, and `diag.log` stays on your PC unless you send it yourself. If an entry of
the optional file `EtsNowPlaying.sources.ini` is enabled (one for Real Hardstyle Radio is included), the plugin also requests the web
address listed there every few seconds, but only while that station is playing; the request contains no data about you, and the
server sees your IP address. Set `[radio] extra_sources=0` or `enabled=0` to switch this off.

**Terms.** Free software under the [MIT License](LICENSE), provided "as is" without warranty. The plugin changes game code in
memory: use it at your own risk and follow the rules of the servers you play on (e.g. TruckersMP). Covers and logos belong to their
rights holders and are only displayed, not stored beyond the logo cache; the terms of iTunes, Deezer and radio-browser.info apply
to their data. Not affiliated with SCS Software, TruckersMP, Spotify, Apple, Deezer, Tidal or Amazon.

[MIT License](LICENSE)
