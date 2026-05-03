# tuxedo

Minimal headless audio engine, cross-platform (macOS + Linux),
architecture modelled on [Cog](https://github.com/losnoco/Cog).

## Build

```
meson setup build
meson compile -C build
```

## Run

```
./build/tuxedod [flags]
```

### Transports

`tuxedod` accepts control on up to three surfaces simultaneously:

| Surface | Default  | Protocol                | Flag                          |
|---------|----------|-------------------------|-------------------------------|
| Unix socket | on   | JSON-lines              | `--socket PATH`, `--no-socket`|
| Stdin       | on   | line-based (legacy)     | `--no-stdin`                  |
| HTTP REST   | off  | JSON                    | `--http PORT`                 |

Socket default: `$XDG_RUNTIME_DIR/tuxedo.sock`, else `/tmp/tuxedo-$UID.sock`.

HTTP binds `127.0.0.1` unless `--http-host ADDR` is passed. No TLS;
expose it through a reverse proxy if you need remote access.

### JSON protocol

Requests are JSON objects. `id` is optional and echoed on the response.

```json
{"id": 1, "op": "play", "url": "/path/to/file.flac"}
{"op": "pause"}
{"op": "resume"}
{"op": "stop"}
{"op": "seek", "seconds": 30.5}
{"op": "volume", "value": 0.5}
{"op": "replaygain", "mode": "album_peak"}
{"op": "status"}
{"op": "metadata_for_url", "url": "/path/to/file.flac"}
{"op": "properties_for_url", "url": "/path/to/file.flac"}
```

Responses:

```json
{"id": 1, "ok": true}
{"id": 1, "ok": false, "error": "open failed"}
{"id": 1, "ok": true, "state": "playing", "position": 12.3,
 "duration": 156.0, "volume": 0.5, "replaygain_mode": "album_peak",
 "shuffle_mode": "off", "repeat_mode": "all", "current_queue_index": 3,
 "from_playlist": true, "url": "..."}
```

Async events (socket and stdin subscribers; HTTP clients get the same
stream via `GET /events` as Server-Sent Events):

```json
{"event": "status_changed", "state": "playing", "url": "..."}
{"event": "stream_began",   "state": "playing", "url": "..."}
{"event": "metadata_changed", "state": "playing", "url": "...",
 "metadata": {"artist": ["..."], "title": ["..."]}}
{"event": "stream_ended",   "state": "stopped", "url": "..."}
{"event": "error",          "state": "stopped", "message": "..."}
```

### HTTP routes

```
GET  /status
GET  /metadata
GET  /replaygain
GET  /shuffle
GET  /repeat
GET  /queue
GET  /events        text/event-stream; `data: <json>\n\n` per event,
                    `:\n\n` heartbeats every 15s of idle.
POST /play          body: {"url": "..."}
POST /queue         body: {"url": "..."}
POST /load_playlist body: {"url": "...", "action": "queue|play"}
POST /metadata_for_url body: {"url": "..."}
POST /properties_for_url body: {"url": "..."}
POST /queue_clear
POST /queue_jump    body: {"index": N}
POST /previous
POST /skip
POST /pause
POST /resume
POST /stop
POST /seek          body: {"seconds": N}
POST /volume        body: {"value": 0..1}
POST /shuffle       body: {"mode": "off|all"}
POST /repeat        body: {"mode": "off|one|all"}
POST /replaygain    body: {"mode": "off|track|track_peak|album|album_peak|soundcheck"}
POST /rpc           body: full request object
```

`play` and `queue` auto-expand local or remote `.m3u`, `.m3u8`, and
`.pls` playlists into queue entries. HLS-style `.m3u8` files
(`#EXT-X-MEDIA-SEQUENCE`) are passed through unchanged as stream URLs.

### Stdin (dev console)

```
volume 0.5
replaygain album_peak
shuffle all
repeat all
play /path/to/file.flac
queue /path/to/next.flac
playlist /path/to/list.m3u
queue_jump 3
previous
pause
resume
seek 30
status
stop
quit
```

## Supported formats

- Sources: local files plus `http://` / `https://` streams.
- Playlist containers: `.m3u`, `.m3u8`, `.pls` with relative-path
  resolution and HLS passthrough for streaming manifests.
- Native decoders: FLAC via libFLAC, Opus via libopusfile, Ogg Vorbis
  via libvorbisfile, MP3 and WAV via miniaudio/libid3tag.
- FFmpeg fallback: additional containers/codecs such as AAC / M4A and
  other formats without a dedicated native decoder.

ReplayGain is applied in the playback chain, with daemon modes:
`off`, `track`, `track_peak`, `album`, `album_peak`, and `soundcheck`.
Default is `album_peak`.

## Build dependencies

- Meson + Ninja + a C++17 compiler.
- `libFLAC` (`brew install flac`).
- `libopusfile` (`brew install opusfile`).
- `libvorbisfile` / `libvorbis`.
- `libmpcdec` (`brew install musepack`).
- `libid3tag`.
- `libcurl`.
- FFmpeg libraries: `libavformat`, `libavcodec`, `libavutil`,
  `libswresample`.

On Arch Linux:

```
sudo pacman -S base-devel meson ninja pkgconf flac opusfile libvorbis libmpcdec libid3tag curl ffmpeg alsa-lib libpulse
```

## Status

Pre-alpha. See `NOTICE.md` for provenance and licensing.
