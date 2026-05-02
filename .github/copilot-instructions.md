# Copilot instructions for Tuxedo

## Build, run, and verification commands

```bash
# Configure once
meson setup build

# Rebuild after edits
meson compile -C build

# Run with the default socket + stdin console
./build/tuxedod

# Run HTTP + SSE without stdin
./build/tuxedod --no-stdin --http 8765
```

There are currently no automated test targets or lint targets in this repository. `meson test -C build --list` reports no tests, so there is no single-test command yet.

Validation is manual and uses the real daemon:

```bash
# Inspect daemon status over HTTP
curl -s http://127.0.0.1:8765/status

# Watch SSE events
curl -N http://127.0.0.1:8765/events

# Send a socket RPC
printf '%s\n' '{"op":"status"}' | nc -U /tmp/tuxedo-$UID.sock
```

For empty-body HTTP POSTs such as `/stop`, `/pause`, or `/resume`, send `-d ''`. A bare `-X POST` can hang until cpp-httplib times out.

## High-level architecture

`src/daemon/main.cpp` is just wiring: it registers compiled-in plugins, constructs `Player` and `Controller`, then exposes them over the unix socket server, HTTP server, and optional stdin console.

`Controller` is the single JSON command surface. HTTP, socket, and stdin all route through it, and async events fan out through `Controller::subscribe()`. If you add or change an operation, update `Controller::dispatch()` and then mirror it in the transport layer instead of re-implementing behavior per transport.

`Player` owns the playback state machine: the current track, logical queue/playlist state, shuffle/repeat/replaygain modes, history, and the watchdog thread. Queue mutations and transport-visible state generally belong here, not in the transports.

The playback graph is `InputNode -> ConverterNode -> OutputNode`, wrapped by `BufferChain`. `InputNode` and `ConverterNode` are worker-thread producers with ring buffers; `OutputNode` runs in the miniaudio callback thread and atomically hot-swaps `next_source_` for gapless transitions. The watchdog thread reacts to audio-thread signals and promotes queued chains when a stream drains.

Playlist expansion happens above the player in `src/core/playlist_parser.cpp` and `Controller::dispatch()`. `play`, `queue`, and `load_playlist` expand local or remote `.m3u`, `.m3u8`, and `.pls` files before they reach `Player`; HLS-style `.m3u8` manifests are passed through unchanged so FFmpeg can handle them as streams.

Sources and decoders are compiled-in plugins registered in `src/plugin/registry.cpp`. New concrete plugins belong under `src/plugin/input/`, `src/plugin/output/`, or `src/plugin/dsp/`; abstract interfaces stay at `src/plugin/`. Native decoders handle common formats first, and FFmpeg is the broad fallback for streams, extensionless URLs, and extra codecs.

ReplayGain is applied inside the playback chain, not just surfaced in metadata. `src/core/replaygain.cpp` derives the gain scalar from canonical metadata keys and also handles Opus R128 q8 values and iTunes SoundCheck data.

## Key conventions

This is C++17 code with tabs for indentation.

Metadata returned by decoders is normalized JSON: keys are lowercase, multi-value tags are arrays even when there is only one value, and album art is exposed as `{"mime","data_b64"}` under `album_art`. Keep new decoder metadata aligned with the existing canonical shape instead of inventing format-specific field names.

Tag canonicalization is centralized in `src/plugin/input/vorbis_common.*` and reused across decoders. Preserve the existing renames such as `lyrics` / `unsynced lyrics` -> `unsyncedlyrics` and `comments:itunnorm` / FFmpeg `itunnorm` -> `soundcheck`.

The stdin console is a legacy line-based parser for manual smoke testing, not a JSON transport. Socket and HTTP are the real request/response/event surfaces and should stay behaviorally aligned through `Controller`.

When changing playback-chain wiring, keep the `OutputNode` ordering rules intact: `OutputNode::set_previous()` must happen after `OutputNode::open()`, because `open()` resets the previous source.

Volume is persistent across output teardowns. A `volume` command sent before `play` must still take effect when the device is opened later.

Testing in this repo is smoke-test oriented: use the real daemon, real transports, and real audio files rather than mocks.

Use `COG_PORT_TODO.md` as the living tracking source for Cog-derived port status and remaining work. When a task touches porting scope, consult it first and update it if the status changes.
