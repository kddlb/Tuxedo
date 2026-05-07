project(
  'tuxedo',
  ['c', 'cpp'],
  version: '0.0.1',
  default_options: [
    'c_std=c11',
    'cpp_std=c++17',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

thread_dep = dependency('threads')

# libFLAC — try pkg-config first, fall back to Homebrew's keg-only layout.
flac_dep = dependency('flac', required: false)
if not flac_dep.found()
  brew_flac = '/opt/homebrew/opt/flac'
  flac_lib = cxx.find_library('FLAC', dirs: [brew_flac + '/lib'], required: false)
  if flac_lib.found()
    flac_dep = declare_dependency(
      include_directories: include_directories(brew_flac + '/include', is_system: true),
      dependencies: [flac_lib],
    )
  else
    error('libFLAC not found — install via `brew install flac` or set PKG_CONFIG_PATH')
  endif
endif

# libopusfile — same Homebrew-keg-only pattern as libFLAC.
opusfile_dep = dependency('opusfile', required: false)
if not opusfile_dep.found()
  brew_opus = '/opt/homebrew/opt/opusfile'
  opusfile_lib = cxx.find_library('opusfile', dirs: [brew_opus + '/lib'], required: false)
  if opusfile_lib.found()
    # opusfile's headers live under opus/ in the Homebrew bottle; add both roots.
    opusfile_dep = declare_dependency(
      include_directories: [
        include_directories(brew_opus + '/include/opus', is_system: true),
        include_directories('/opt/homebrew/opt/opus/include/opus', is_system: true),
        include_directories('/opt/homebrew/opt/libogg/include', is_system: true),
      ],
      dependencies: [opusfile_lib],
    )
  else
    error('libopusfile not found — install via `brew install opusfile` or set PKG_CONFIG_PATH')
  endif
endif

# libvorbisfile — same Homebrew-keg-only pattern. Pulls in libvorbis
# and libogg for symbols.
vorbisfile_dep = dependency('vorbisfile', required: false)
if not vorbisfile_dep.found()
  brew_vorbis = '/opt/homebrew/opt/libvorbis'
  vorbisfile_lib = cxx.find_library('vorbisfile', dirs: [brew_vorbis + '/lib'], required: false)
  vorbis_lib = cxx.find_library('vorbis', dirs: [brew_vorbis + '/lib'], required: false)
  ogg_lib = cxx.find_library('ogg', dirs: ['/opt/homebrew/opt/libogg/lib'], required: false)
  if vorbisfile_lib.found() and vorbis_lib.found() and ogg_lib.found()
    vorbisfile_dep = declare_dependency(
      include_directories: [
        include_directories(brew_vorbis + '/include', is_system: true),
        include_directories('/opt/homebrew/opt/libogg/include', is_system: true),
      ],
      dependencies: [vorbisfile_lib, vorbis_lib, ogg_lib],
    )
  else
    error('libvorbisfile not found — install via `brew install libvorbis` or set PKG_CONFIG_PATH')
  endif
endif

# libid3tag — Homebrew keg-only, no pkg-config file in older versions.
id3tag_dep = dependency('id3tag', required: false)
if not id3tag_dep.found()
  brew_id3tag = '/opt/homebrew/opt/libid3tag'
  id3tag_lib = cxx.find_library('id3tag', dirs: [brew_id3tag + '/lib'], required: false)
  if id3tag_lib.found()
    id3tag_dep = declare_dependency(
      include_directories: include_directories(brew_id3tag + '/include', is_system: true),
      dependencies: [id3tag_lib],
    )
  else
    error('libid3tag not found — install via `brew install libid3tag` or set PKG_CONFIG_PATH')
  endif
endif

# libmpcdec — Musepack decoder library.
mpcdec_dep = dependency('libmpcdec', required: false)
if not mpcdec_dep.found()
  mpcdec_lib = cxx.find_library('mpcdec', required: false)
  if mpcdec_lib.found()
    mpcdec_dep = declare_dependency(
      dependencies: [mpcdec_lib],
    )
  else
    error('libmpcdec not found — install via your distro package manager or Homebrew')
  endif
endif

# TagLib — offline metadata/properties reader for broad local-file coverage.
taglib_dep = dependency('taglib', required: false)
if not taglib_dep.found()
  brew_taglib = '/opt/homebrew/opt/taglib'
  taglib_lib = cxx.find_library('tag', dirs: [brew_taglib + '/lib'], required: false)
  if taglib_lib.found()
    taglib_dep = declare_dependency(
      include_directories: include_directories(brew_taglib + '/include/taglib', is_system: true),
      dependencies: [taglib_lib],
    )
  else
    error('taglib not found — install via `brew install taglib` or your distro package manager')
  endif
endif

libarchive_dep = dependency('libarchive', required: false)
if not libarchive_dep.found()
  brew_libarchive = '/opt/homebrew/opt/libarchive'
  archive_lib = cxx.find_library('archive', dirs: [brew_libarchive + '/lib'], required: false)
  if archive_lib.found()
    libarchive_dep = declare_dependency(
      include_directories: include_directories(brew_libarchive + '/include', is_system: true),
      dependencies: [archive_lib],
    )
  else
    error('libarchive not found — install via `brew install libarchive` or your distro package manager')
  endif
endif

# Single-header vendored libraries.
miniaudio_inc = include_directories('vendor/miniaudio')
minimp3_inc = include_directories('vendor/minimp3', is_system: true)
json_inc = include_directories('vendor/json', is_system: true)
httplib_inc = include_directories('vendor/httplib', is_system: true)

deps = [thread_dep, flac_dep, opusfile_dep, vorbisfile_dep, id3tag_dep, mpcdec_dep, taglib_dep, libarchive_dep]
curl_dep = dependency('libcurl')
avformat_dep = dependency('libavformat')
avcodec_dep = dependency('libavcodec')
avutil_dep = dependency('libavutil')
swresample_dep = dependency('libswresample')
deps += [curl_dep, avformat_dep, avcodec_dep, avutil_dep, swresample_dep]

host = host_machine.system()
if host == 'darwin'
  deps += [
    dependency('appleframeworks', modules: [
      'CoreFoundation', 'CoreAudio', 'AudioToolbox', 'AudioUnit',
    ]),
  ]
elif host == 'linux'
  # miniaudio picks its backend at runtime; link dl/m/pthread
  deps += [cc.find_library('dl'), cc.find_library('m')]
  alsa = dependency('alsa', required: false)
  pulse = dependency('libpulse', required: false)
  if alsa.found()
    deps += alsa
  endif
  if pulse.found()
    deps += pulse
  endif
endif

core_inc = include_directories('src')

core_sources = files(
  'src/core/archive_url.cpp',
  'src/core/audio_chunk.cpp',
  'src/core/channel_layout.cpp',
  'src/core/cue_sheet.cpp',
  'src/core/media_probe.cpp',
  'src/core/metadata_query.cpp',
  'src/core/playlist_parser.cpp',
  'src/core/player.cpp',
  'src/core/replaygain.cpp',
  'src/core/chain/node.cpp',
  'src/core/chain/input_node.cpp',
  'src/core/chain/output_node.cpp',
  'src/core/chain/dsp_node.cpp',
  'src/core/chain/downmix.cpp',
  'src/core/chain/dsp_downmix_node.cpp',
  'src/core/chain/dsp_fader_node.cpp',
  'src/core/chain/faded_buffer.cpp',
  'src/core/chain/converter_node.cpp',
  'src/core/chain/buffer_chain.cpp',
  'src/plugin/registry.cpp',
  'src/plugin/input/archive_source.cpp',
  'src/plugin/input/cue_decoder.cpp',
  'src/plugin/input/ffmpeg_decoder.cpp',
  'src/plugin/input/file_source.cpp',
  'src/plugin/input/flac_decoder.cpp',
  'src/plugin/input/hls_decoder.cpp',
  'src/plugin/input/hls_memory_source.cpp',
  'src/plugin/input/hls_playlist.cpp',
  'src/plugin/input/hls_segment_manager.cpp',
  'src/plugin/input/http_source.cpp',
  'src/plugin/input/miniaudio_decoder.cpp',
  'src/plugin/input/musepack_decoder.cpp',
  'src/plugin/input/mp3_decoder.cpp',
  'vendor/minimp3/minimp3_impl.c',
  'src/plugin/input/opus_decoder.cpp',
  'src/plugin/input/silence_decoder.cpp',
  'src/plugin/input/silence_source.cpp',
  'src/plugin/input/vorbis_common.cpp',
  'src/plugin/input/vorbis_decoder.cpp',
  'src/plugin/output/miniaudio_backend.cpp',
  'src/ipc/controller.cpp',
  'src/ipc/http_server.cpp',
  'src/ipc/socket_server.cpp',
  'src/ipc/stdin_control.cpp',
)

tuxedod = executable(
  'tuxedod',
  core_sources + files('src/daemon/main.cpp'),
  include_directories: [core_inc, miniaudio_inc, minimp3_inc, json_inc, httplib_inc],
  dependencies: deps,
  install: true,
)
