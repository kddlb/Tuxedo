# Tuxedo

Cross-platform headless audio engine. Architecture derived from
[Cog](https://github.com/losnoco/Cog) (macOS audio player): the layering
(Player → BufferChain → Input/Output Nodes), the plugin interfaces
(Source / Decoder / Container / MetadataReader), the `AudioChunk`
abstraction, and the `PlaybackStatus` enum are all modelled on Cog's
`Audio/` subsystem.

Implementation is fresh C++17; the Objective-C / Cocoa / CoreAudio /
AudioUnit layer is replaced with portable equivalents (miniaudio for
device output, thread primitives from the C++ standard library).

## Licensing

To be determined. Cog is currently GPL-licensed; relicensing of
derived architectural ideas is being evaluated separately. Until that
is resolved, treat this repository as **all rights reserved** and do
not redistribute.

## Vendored third-party code

- `vendor/miniaudio/miniaudio.h` — miniaudio 0.11.22
  (<https://github.com/mackron/miniaudio>), dual-licensed MIT-0 /
  public domain.
