# Tube3DS

An unofficial, modern YouTube client for the Nintendo 3DS. Search, watch with
sound, browse channels, subscribe, keep a watch history and a Watch later list,
all on the console. No PC or proxy server needed.

Not affiliated with YouTube or Google.

## Features

- **Video playback with audio**: H.264 video (144p–480p) plus AAC/Opus audio,
  decoded on the console with A/V sync, seeking, and resume-where-you-left-off
- **Search** with infinite scroll and recent searches
- **Channels**: browse uploads, subscribe/unsubscribe
- **Home feed** built from your subscriptions (stored locally; no Google account needed)
- **Library**: watch history and Watch later
- **Up next** related videos with autoplay
- Dark, touch-friendly UI: video preview on the top screen, scrolling list on
  the bottom, full D-pad support
- Tuned per model: 144p on Old 3DS, 240p on New 3DS (uses the 804 MHz mode and
  the extra CPU core), changeable in Settings or per video

## Install

You need a 3DS with custom firmware (e.g. Luma3DS).

1. Copy `Tube3DS.cia` to your SD card.
2. Install it with FBI (or any CIA installer). Tube3DS appears on the HOME Menu.
3. **For sound**, your console needs a DSP firmware dump at `sdmc:/3ds/dspfirm.cdc`.
   If you've never made one, run the [DSP1](https://github.com/zoogie/DSP1) homebrew once.

Prefer the Homebrew Launcher? Copy `Tube3DS.3dsx` to `sdmc:/3ds/` instead.

## Controls

| Where      | Button            | Action                                  |
|------------|-------------------|-----------------------------------------|
| Anywhere   | START             | Quit                                    |
| Lists      | D-pad / Circle Pad / touch | Move / scroll                  |
|            | A                 | Open                                    |
|            | Y                 | Go to the video's channel               |
|            | SELECT            | Add to / remove from Watch later        |
|            | X                 | Search                                  |
|            | L / R             | Previous / next tab                     |
| Channel    | X                 | Subscribe / unsubscribe                 |
|            | B                 | Back                                    |
| Watching   | A                 | Play / pause                            |
|            | L / R             | Back / forward 10 s                     |
|            | D-pad left/right  | Back / forward 5 s                      |
|            | D-pad up/down     | Browse *Up next* (A opens, B leaves)    |
|            | X                 | Switch quality                          |
|            | Y                 | Go to channel                           |
|            | SELECT            | Watch later                             |
|            | B                 | Back                                    |
|            | Touch             | Drag the seek bar, tap the buttons      |

## Files on the SD card

Everything lives in `sdmc:/3ds/Tube3DS/`:

| File            | Contents                                            |
|-----------------|-----------------------------------------------------|
| `settings.json` | Settings                                            |
| `library.json`  | Subscriptions, history, Watch later, resume points  |
| `config.json`   | Optional overrides for how Tube3DS talks to YouTube |
| `log.txt`       | Log of the last run (useful when something breaks)  |

### When YouTube changes something

Tube3DS talks to YouTube's own API (InnerTube) the way YouTube's apps do. It
uses the `visionOS` client for video streams, because that client returns plain
H.264 stream URLs without a PO token. If YouTube changes the required client
version, edit `config.json`: rename `_player_client` to `player_client` and
update `clientVersion` / `userAgent`, using the values in yt-dlp's
`yt_dlp/extractor/youtube/_base.py`. No rebuild needed.

YouTube sometimes refuses a stream partway through; Tube3DS fetches fresh
stream URLs and carries on automatically.

## Building

Requirements: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`3ds-dev` group (devkitARM, libctru, citro2d/citro3d), plus `git`, `curl`,
`python3`, `make` and a host C/C++ compiler.

```sh
make deps   # once: builds zlib, mbedtls, curl and FFmpeg for the 3DS, plus makerom
            # and bannertool, into ./deps (no sudo, nothing installed system-wide)
make        # -> Tube3DS.3dsx
make cia    # -> Tube3DS.cia
```

`make` runs `make deps` automatically the first time. The FFmpeg build is
trimmed to the H.264/AAC/Opus decoders and MP4/WebM demuxers, and takes a few
minutes.

### Tests

The platform-independent parts (YouTube API parsing, the HTTP range-request
stream, FFmpeg demux/decode/seek) can be tested on a desktop against live
YouTube. This needs libcurl, plus FFmpeg's libraries for the media test (e.g.
`brew install ffmpeg`):

```sh
make -C tests
```

For emulator testing (e.g. Azahar), create `sdmc:/3ds/Tube3DS/autotest.txt`
containing a search query. On launch, Tube3DS searches for it, plays the first
result, and logs playback stats to `log.txt`. Without that file it does nothing.

### Layout

```
source/
  main.cpp            service setup/teardown
  ui/                 app shell, pages, list widget, drawing, watch screen
  yt/                 InnerTube client + JSON parser
  net/                libcurl wrapper
  media/              player: range-request streams, FFmpeg demux/decode,
                      NDSP audio, frame queue (decoded frames -> GPU textures)
  thumbnails.cpp      async thumbnail cache
  library.cpp         settings, subscriptions, history (JSON on SD)
  gfx_util.cpp        tiled-texture conversion (YUV/RGB -> RGB565)
assets/               icon, banner, banner sound, CIA spec (app.rsf)
romfs/cacert.pem      root certificates for HTTPS
scripts/              dependency build, asset generator, patches
tests/                host-side tests
```
