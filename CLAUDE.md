# CLAUDE.md — tuxedo

Headless, cross-platform audio daemon. Architecture derived from
[Cog](https://github.com/losnoco/Cog); implementation is fresh C++17.
Upstream repo: `github.com/kddlb/Tuxedo`. See `NOTICE.md` for
provenance and licensing (still TBD — treat as all-rights-reserved).

## What this daemon is (and isn't)

- **Is**: the *server* half of a client/server music app. Exposes JSON
  IPC over a unix socket, JSON REST + Server-Sent-Events over HTTP,
  and a line-based stdin console for manual testing. Plays files,
  gapless-queues tracks, surfaces tag metadata + album art.
- **Isn't** (yet): a library catalog, a playlist parser, a transcoder,
  a full-featured CLI client. No GUI lives here — that would be a
  separate client project.

## Quick commands

```
meson setup build                    # once
meson compile -C build               # always after edits
./build/tuxedod                      # socket default on; stdin on; http off
./build/tuxedod --http 8765          # add REST + /events SSE on localhost
./build/tuxedod --no-socket --http 8765 --no-stdin   # minimal HTTP-only
```

Kill strays before re-testing: `pkill -9 -f build/tuxedod`.

**Do not run `meson compile` if the user is going to iterate** — they
prefer to build manually in many cases. For tuxedo specifically,
`meson compile` is cheap and expected during development; run it
yourself after edits. (This is different from the Cog repo, which has
an explicit *don't-build-from-Claude* memory.)

## Build dependencies

- macOS: `brew install meson ninja flac opusfile`.
- Linux (untested as of writing): `pkg-config`, `libflac-dev`,
  `libopusfile-dev`, ALSA/Pulse dev headers for miniaudio's backend.
- Vendored, no action needed: miniaudio, nlohmann/json, cpp-httplib —
  all under `vendor/`.

## Source layout

```
src/
├── core/                             engine state + chain types
│   ├── player.{hpp,cpp}              top-level orchestrator; queue + watchdog
│   ├── audio_chunk.{hpp,cpp}         interleaved float32 chunk with timestamp
│   ├── format.hpp                    StreamFormat {rate, channels}
│   ├── status.hpp                    PlaybackStatus enum
│   └── chain/                        the playback graph
│       ├── node.{hpp,cpp}            threaded producer base w/ ring buffer
│       ├── input_node.{hpp,cpp}      drives a Decoder
│       ├── output_node.{hpp,cpp}    render callback; atomic next_source hot-swap
│       └── buffer_chain.{hpp,cpp}    owns InputNode, remembers URL
├── plugin/                           plugin interfaces + implementations
│   ├── source.hpp                    I/O interface (file/http/…)
│   ├── decoder.hpp                   Decoder interface: open/read/seek/metadata
│   ├── output_backend.hpp            device sink interface
│   ├── registry.{hpp,cpp}            static registration by scheme/extension
│   ├── input/                        SOURCES + DECODERS go here
│   │   ├── file_source.{hpp,cpp}
│   │   ├── flac_decoder.{hpp,cpp}    libFLAC, STREAMINFO+VC+PICTURE
│   │   ├── opus_decoder.{hpp,cpp}    libopusfile, VC+PICTURE+R128
│   │   └── miniaudio_decoder.{hpp,cpp}  MP3/WAV/OGG fallback, no tags
│   ├── output/                       OUTPUT BACKENDS go here
│   │   └── miniaudio_backend.{hpp,cpp}
│   └── dsp/                          EFFECTS will go here (empty)
├── ipc/                              transports — all route through Controller
│   ├── controller.{hpp,cpp}          JSON dispatch + event fan-out
│   ├── socket_server.{hpp,cpp}       unix socket, JSON-lines
│   ├── http_server.{hpp,cpp}         cpp-httplib wrapper, REST + /events SSE
│   ├── event_mailbox.hpp             bounded queue per SSE subscriber
│   └── stdin_control.{hpp,cpp}       dev console (line-based, NOT JSON)
└── daemon/
    └── main.cpp                      arg parse, wiring, signal shutdown
```

**Plugin layout rule**: new plugin implementations go under
`src/plugin/{input,output,dsp}/` by type. Plugin *interfaces* (the
abstract classes) stay at `src/plugin/` top level — they aren't
plugins themselves.

## Architecture essentials

1. **Player** owns a current `BufferChain` and a `std::deque` queue of
   pre-built, pre-decoding `BufferChain`s for gapless.
2. **BufferChain** holds an `InputNode` (decoder thread) and remembers
   the URL it was opened with.
3. **Node** base class: worker thread, ring buffer, `read_chunk` waits
   on a condvar until data or end-of-stream.
4. **OutputNode** is NOT threaded. It's called from miniaudio's audio
   callback. It holds `std::atomic<Node*> previous_` and
   `std::atomic<Node*> next_source_`. When `previous_` drains and EOS
   is set, the render callback atomically takes `next_source_` and
   swaps — that's the gapless mechanism.
5. **Player watchdog thread** is woken by atomic-signalled
   `on_stream_consumed` / `on_stream_advanced` callbacks from the
   audio thread. It pops the queue front, emits events, re-arms
   `next_source_` if another queued track matches the output format.
6. **Format mismatch**: the watchdog tears down + reopens the output
   device at the new format. Audible ~50–100 ms gap; known limitation.
7. **Controller** is the single JSON-in/JSON-out dispatch. All three
   transports (socket, HTTP, stdin) go through it. Events fan out via
   `Controller::subscribe(cb)` → multiple subscribers (tokens).

## Wire protocol

Request: `{"op": "play", "url": "...", "id": <optional>}`.
Response: `{"ok": true, "id": <echoed>}` or `{"ok": false, "error": "..."}`.
Event (socket/stdin/SSE): `{"event": "stream_began", "state": "playing", ...}`.

Ops: `play`, `pause`, `resume`, `stop`, `seek`, `volume`, `status`,
`metadata`, `queue`, `queue_clear`, `queue_list`, `skip`.

HTTP routes: mirror the ops. `GET /status | /metadata | /queue |
/events`, `POST /play | /pause | /resume | /stop | /seek | /volume |
/queue | /queue_clear | /skip | /rpc`.

SSE: `GET /events` → `text/event-stream`; each event `data: <json>\n\n`;
`:\n\n` heartbeat every 15 s idle.

## Metadata shape

All decoders return `nlohmann::json` with lowercased keys, multi-value
fields as JSON arrays (even single-value, for uniformity). Canonical
fields: `title`, `artist`, `album`, `albumartist`, `tracknumber`,
`discnumber`, `date`, `genre`, `replaygain_*`, `unsyncedlyrics`,
`codec` (scalar), `album_art: {mime, data_b64}`.

FLAC and Opus apply Cog's two renames: `"lyrics"`/`"unsynced lyrics"`
→ `"unsyncedlyrics"`; `"comments:itunnorm"` → `"soundcheck"`. Opus
additionally surfaces raw R128 centibel-q8 values under
`r128_*_gain_q8` fields alongside formatted `replaygain_*` strings.

## Non-obvious gotchas

- `OutputNode::set_previous()` must be called **after**
  `OutputNode::open()`. `open()` internally calls `close()`, which
  resets the atomic `previous_` to null. This bit once during the
  gapless work.
- Node base class's `read_chunk` blocks when the buffer is empty and
  EOS isn't set. That runs in the miniaudio audio thread. In practice
  decode-faster-than-realtime keeps the buffer full; if a decoder ever
  stalls that could become a priority-inversion issue.
- `libFLAC`'s `FLAC__StreamDecoder` is a typedef of an anonymous
  struct and **cannot be forward-declared** — we store it as `void *`
  and `static_cast` inside the .cpp. `libopusfile`'s `OggOpusFile` has
  the same constraint.
- Homebrew's `flac` and `opusfile` formulas are keg-only; `meson.build`
  has a manual `find_library` + hard-coded `/opt/homebrew/opt/...`
  include path fallback because a bare `dependency('...')` won't find
  them without `PKG_CONFIG_PATH`. If similar build deps land later,
  follow the existing `flac_dep` / `opusfile_dep` pattern.
- Stdin console is **not** JSON. It's the legacy line-based grammar
  (`play <path>`, `seek 30`, …). Socket and HTTP are the real IPC.
- Volume is persisted in the Player across teardowns (a `volume` op
  sent before `play` is honoured when the output device opens). Don't
  regress this.

## Conventions

- C++17; tabs for indentation (existing files are tab-indented).
- No `// used by X` / `// added for Y` comments. No multi-paragraph
  docstrings. Terse comments only where the *why* is non-obvious.
- Never mock at test boundaries — smoke tests hit the real daemon, the
  real audio device, and real files (the user has an extensive music
  library and expects tests to use it).
- Commit messages: imperative subject, explanatory body, trailer
  `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`.
  Commits land directly on `main` unless the user says otherwise.

## Verification habit

After non-trivial changes:

1. `meson compile -C build` — must succeed.
2. Smoke-test a real FLAC and a real Opus end-to-end (user has both
   cached paths in memory: the Beatles Revolver FLAC and
   `/Users/kevin/dallas.opus`). Include `volume`, `play`, `status`,
   `seek`, `stop`. Ask the user to listen if audio behaviour changed.
3. For gapless work: `Sun King → Mean Mr. Mustard` from Abbey Road
   '09 Stereo Box is the canonical gapless-pair test (same 44.1 kHz
   format; the album transitions directly between the two).
4. For SSE or socket work: drive via `curl -N /events` or
   `nc -U /tmp/tuxedo-$UID.sock`, not just the log.

## Known MVP gaps

- **Format-mismatched gapless** falls back to device reopen (audible
  gap). A miniaudio `ma_data_converter`-backed `ConverterNode` would
  close this gap.
- **MP3 / Ogg Vorbis metadata**: miniaudio is tag-less. A dedicated
  libvorbisfile decoder (Vorbis comments) and a libid3tag-based MP3
  reader would surface tags there too.
- **Linux bring-up** is untested — `meson.build` has the ALSA/Pulse
  paths wired but no one has run it on Linux yet.
- **HTTP SSE over TLS**: we bind 127.0.0.1 only; put a reverse proxy
  in front for remote access.
- **License resolution**: pending. Don't redistribute tuxedo until
  sorted.

## Related local repos

- `/Users/kevin/src/Cog` — upstream Cog. Useful reference for porting
  further decoders (`Cog/Plugins/*`), sources, and DSP nodes. Cog's
  GitHub org is **losnoco** (not "losno").
