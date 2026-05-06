# Cog porting todo

This is the living tracking file for Cog-derived, non-GUI features in Tuxedo. Future porting work should update this checklist as features land.

Legend:

- `[x]` Ported in Tuxedo
- `[ ]` Not yet ported, or only partially ported and still needs follow-up

GUI-only Cog features are intentionally omitted. Items are grouped by subsystem rather than mirroring the Cog filesystem exactly.

## Core playback architecture

- [x] `AudioChunk` (`Cog/Audio/Chain/AudioChunk.*` -> `src/core/audio_chunk.*`)
- [x] `Node` (`Cog/Audio/Chain/Node.*` -> `src/core/chain/node.*`)
- [x] `InputNode` (`Cog/Audio/Chain/InputNode.*` -> `src/core/chain/input_node.*`)
- [x] `ConverterNode` (`Cog/Audio/Chain/ConverterNode.*` -> `src/core/chain/converter_node.*`)
- [x] `OutputNode` (`Cog/Audio/Chain/OutputNode.*` -> `src/core/chain/output_node.*`)
- [x] `BufferChain` (`Cog/Audio/Chain/BufferChain.*` -> `src/core/chain/buffer_chain.*`)
- [x] `AudioPlayer`-style orchestrator (`Cog/Audio/AudioPlayer.*` -> `src/core/player.*`)
- [x] Queue/history controls such as previous, skip, queue jump, shuffle, and repeat (`Cog/Audio/AudioPlayer.*` -> `src/core/player.*`, `src/ipc/controller.cpp`)
- [x] ReplayGain application in the playback path (Cog uses fader-node logic; Tuxedo applies gain in `src/core/replaygain.*` and `src/core/player.*`)
- [x] `DSPNode` base (`Cog/Audio/Chain/DSPNode.*` -> `src/core/chain/dsp_node.*`)
- [ ] `ChunkList` (`Cog/Audio/Chain/ChunkList.*`; no Tuxedo equivalent)
- [ ] `VisualizationNode` (`Cog/Audio/Chain/VisualizationNode.*`; no Tuxedo equivalent)
- [ ] HDCD processing support (`Cog/Audio/ThirdParty/hdcd/`; no Tuxedo equivalent found)

## Output backends

- [x] Output backend abstraction (`Cog/Audio/Output/*`, `Cog/Audio/PluginController.*` -> `src/plugin/output_backend.hpp`, `src/plugin/output/miniaudio_backend.*`)
- [x] Live output-device rendering path (`Cog/Audio/Output/*` -> `src/plugin/output/miniaudio_backend.*`, `src/core/chain/output_node.*`)
- [ ] `OutputCoreAudio` (`Cog/Audio/Output/OutputCoreAudio.*`; no Tuxedo equivalent)
- [ ] `OutputAVFoundation` (`Cog/Audio/Output/OutputAVFoundation.*`; no Tuxedo equivalent)

## Sources, playlists, and containers

- [x] `FileSource` (`Cog/Plugins/FileSource/*` -> `src/plugin/input/file_source.*`)
- [x] `HTTPSource` (`Cog/Plugins/HTTPSource/*` -> `src/plugin/input/http_source.*`)
- [x] M3U / M3U8 playlist expansion (`Cog/Plugins/M3u/*` -> `src/core/playlist_parser.*`, `src/ipc/controller.cpp`)
- [x] PLS playlist expansion (`Cog/Plugins/Pls/*` -> `src/core/playlist_parser.*`, `src/ipc/controller.cpp`)
- [x] Cue sheet virtual-track support (`Cog/Plugins/CueSheet/*` -> `src/core/cue_sheet.*`, `src/plugin/input/cue_decoder.*`, `src/core/playlist_parser.*`, `src/core/media_probe.*`)
- [x] Archive-backed sources (`Cog/Plugins/ArchiveSource/*` -> `src/plugin/input/archive_source.*`, `src/core/archive_url.*`)
- [x] `SilenceDecoder` (`Cog/Plugins/SilenceDecoder/*` -> `src/plugin/input/silence_source.*`, `src/plugin/input/silence_decoder.*`; playback open failures now fall back to `silence://10` in `src/core/chain/buffer_chain.cpp`)
- [ ] Playlist/model layer beyond the daemon queue (`Cog/Playlist/PlaylistController.*`, `PlaylistLoader.*`, `XmlContainer.*`; no Tuxedo equivalent)

## Decoders and format support

- [x] FLAC decode (`Cog/Plugins/Flac/*` -> `src/plugin/input/flac_decoder.*`)
- [x] Ogg Vorbis decode (`Cog/Plugins/Vorbis/*` -> `src/plugin/input/vorbis_decoder.*`)
- [x] Opus decode (`Cog/Plugins/Opus/*` -> `src/plugin/input/opus_decoder.*`)
- [x] MP3 decode (`Cog/Plugins/minimp3/*` -> `src/plugin/input/mp3_decoder.*`)
- [x] WAV decode / fallback path (Cog WAV support via plugin stack -> `src/plugin/input/miniaudio_decoder.*`)
- [x] FFmpeg fallback decode/container support (`Cog/Plugins/FFMPEG/*` -> `src/plugin/input/ffmpeg_decoder.*`)
- [ ] `CoreAudioDecoder` (`Cog/Plugins/CoreAudio/*`; Tuxedo currently relies on FFmpeg fallback instead of a dedicated CoreAudio decoder)
- [ ] `QuicktimeDecoder` (`Cog/Plugins/Quicktime/*`; no Tuxedo equivalent)
- [x] `MusepackDecoder` (`Cog/Plugins/Musepack/*` -> `src/plugin/input/musepack_decoder.*`)
- [ ] `WavPackDecoder` (`Cog/Plugins/WavPack/*`; no Tuxedo equivalent)
- [ ] `ShortenDecoder` (`Cog/Plugins/Shorten/*`; no Tuxedo equivalent)
- [ ] `Dumb` tracker decoder/container (`Cog/Plugins/Dumb/*`; no Tuxedo equivalent)
- [ ] `modplay` tracker decoder (`Cog/Plugins/modplay/*`; no Tuxedo equivalent)
- [ ] `OpenMPT` tracker decoder (`Cog/Plugins/OpenMPT/*`; no Tuxedo equivalent)
- [ ] `playptmod` tracker decoder (`Cog/Plugins/playptmod/*`; no Tuxedo equivalent)
- [ ] `GameDecoder` / GME (`Cog/Plugins/GME/*`; no Tuxedo equivalent)
- [ ] `libvgmPlayer` (`Cog/Plugins/libvgmPlayer/*`; no Tuxedo equivalent)
- [ ] `SidDecoder` / `SidContainer` (`Cog/Plugins/sidplay/*`; no Tuxedo equivalent)
- [ ] MIDI decoder stack (`Cog/Plugins/MIDI/*`; no Tuxedo equivalent)
- [ ] `AdPlug` decoder (`Cog/Plugins/AdPlug/*`; no Tuxedo equivalent)
- [ ] `HighlyComplete` decoder (`Cog/Plugins/HighlyComplete/*`; no Tuxedo equivalent)
- [ ] `Hively` decoder (`Cog/Plugins/Hively/*`; no Tuxedo equivalent)
- [ ] `Syntrax` decoder (`Cog/Plugins/Syntrax/*`; no Tuxedo equivalent)
- [ ] `Organya` decoder (`Cog/Plugins/Organya/*`; no Tuxedo equivalent)
- [ ] `APLDecoder` (`Cog/Plugins/APL/*`; no Tuxedo equivalent)
- [ ] `BASSMODS` decoder (`Cog/Plugins/BASSMODS/*`; no Tuxedo equivalent)

## Metadata and library infrastructure

- [x] Decoder-owned metadata extraction (`Cog/Audio/AudioMetadataReader.*`, per-plugin readers -> per-decoder metadata in `src/plugin/input/*`)
- [x] Canonicalized tag output with album art and ReplayGain fields (`Cog` tag readers -> `src/plugin/input/vorbis_common.*`, `src/plugin/input/mp3_decoder.*`, `src/plugin/input/ffmpeg_decoder.*`)
- [x] Generic `AudioMetadataReader` facade (`Cog/Audio/AudioMetadataReader.*` -> `src/core/metadata_query.*`, `src/ipc/controller.cpp`)
- [ ] Generic `AudioMetadataWriter` facade (`Cog/Audio/AudioMetadataWriter.*`; no Tuxedo equivalent)
- [x] Generic `AudioPropertiesReader` facade (`Cog/Audio/AudioPropertiesReader.*` -> `src/core/metadata_query.*`, `src/ipc/controller.cpp`)
- [ ] TagLib-backed metadata reader/writer (`Cog/Plugins/TagLib/*`; metadata/properties reader path ported in `src/core/metadata_query.*`, writer still missing)
- [x] Cue sheet metadata reader (`Cog/Plugins/CueSheet/CueSheetMetadataReader.*` -> cue-aware metadata/properties in `src/core/cue_sheet.*`, `src/plugin/input/cue_decoder.*`, `src/core/metadata_query.*`)
- [ ] Persistent catalog / store layer (`Cog/Utils/SQLiteStore.*`, `RedundantPlaylistDataStore.*`; no Tuxedo equivalent)

## DSP and signal processing

- [x] `DSPFaderNode` and fade buffer helpers (`Cog/Audio/Chain/DSP/DSPFaderNode.*`, `FadedBuffer.*` -> `src/core/chain/dsp_fader_node.*`, `src/core/chain/faded_buffer.*`; fade state now lives in the DSP node and is applied on the final callback buffer to avoid buffered-ahead artifacts`)
- [ ] `DSPEqualizerNode` (`Cog/Audio/Chain/DSP/DSPEqualizerNode.*`; no Tuxedo equivalent)
- [ ] `DSPDownmixNode` + `Downmix` (`Cog/Audio/Chain/DSP/DSPDownmixNode.*`, `Downmix.*`; no Tuxedo equivalent)
- [ ] `DSPFSurroundNode` + `FSurroundFilter` (`Cog/Audio/Chain/DSP/DSPFSurroundNode.*`, `FSurroundFilter.*`; no Tuxedo equivalent)
- [ ] `DSPHRTFNode` + `HeadphoneFilter` (`Cog/Audio/Chain/DSP/DSPHRTFNode.*`, `HeadphoneFilter.*`; no Tuxedo equivalent)
- [ ] `DSPRubberbandNode` (`Cog/Audio/Chain/DSP/DSPRubberbandNode.*`; no Tuxedo equivalent)
- [ ] `DSPSignalsmithStretchNode` (`Cog/Audio/Chain/DSP/DSPSignalsmithStretchNode.*`; no Tuxedo equivalent)

## Non-GUI control surfaces and services

- [ ] AppleScript automation dictionary (`Cog/Cog.sdef`; no Tuxedo equivalent)
- [ ] Media-key / shortcut integration (`Cog/Preferences/Shortcuts.h`, `Cog/Shortcuts.h`; no Tuxedo equivalent)
- [ ] Last.fm scrobbling (`Cog/Scrobbler/*`; no Tuxedo equivalent)
- [ ] Spotlight-backed playlist/search integration (`Cog/Spotlight/*`; no Tuxedo equivalent)
- [ ] Feedback/reporting socket layer (`Cog/Feedback/*`; no Tuxedo equivalent)

## Notes for future updates

- Treat this file as the source of truth for what has and has not been ported from Cog.
- Prefer current Tuxedo code over older summary docs when updating status.
- When a Tuxedo feature is only a functional replacement for a Cog module, keep the item checked but note the implementation difference inline.
