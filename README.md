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
{"op": "status"}
```

Responses:

```json
{"id": 1, "ok": true}
{"id": 1, "ok": false, "error": "open failed"}
{"id": 1, "ok": true, "state": "playing", "position": 12.3,
 "duration": 156.0, "volume": 0.5, "url": "..."}
```

Async events (socket and stdin subscribers only — HTTP is request/reply):

```json
{"event": "status_changed", "state": "playing", "url": "..."}
{"event": "stream_began",   "state": "playing", "url": "..."}
{"event": "stream_ended",   "state": "stopped", "url": "..."}
{"event": "error",          "state": "stopped", "message": "..."}
```

### HTTP routes

```
GET  /status
POST /play      body: {"url": "..."}
POST /pause
POST /resume
POST /stop
POST /seek      body: {"seconds": N}
POST /volume    body: {"value": 0..1}
POST /rpc       body: full request object
```

### Stdin (dev console)

```
volume 0.5
play /path/to/file.flac
pause
resume
seek 30
status
stop
quit
```

## Supported formats

MP3, WAV, Ogg Vorbis via miniaudio; FLAC via libFLAC. More decoders
(Opus, WavPack, Musepack, …) will be ported from Cog as the `Decoder`
ABI stabilises.

## Status

Pre-alpha. See `NOTICE.md` for provenance and licensing.
