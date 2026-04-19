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
./build/tuxedod
```

Commands are read line-by-line from stdin:

```
play /path/to/file.flac
pause
resume
seek 30.5
stop
status
volume 0.5
quit
```

Events are emitted on stdout as `event: <name> <args...>` lines.

## Supported formats (MVP)

MP3, FLAC, WAV, Ogg Vorbis — via miniaudio's built-in decoders. Cog's
wider decoder matrix (Opus, WavPack, Musepack, GME, VGMStream, SID,
OpenMPT, MIDI, …) will be ported plugin-by-plugin as the decoder ABI
stabilises.

## Status

Pre-alpha. See `NOTICE.md` for provenance and licensing.
