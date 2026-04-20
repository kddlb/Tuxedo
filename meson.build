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

# Single-header vendored libraries.
miniaudio_inc = include_directories('vendor/miniaudio')
json_inc = include_directories('vendor/json', is_system: true)
httplib_inc = include_directories('vendor/httplib', is_system: true)

deps = [thread_dep, flac_dep, opusfile_dep]

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
  'src/core/audio_chunk.cpp',
  'src/core/player.cpp',
  'src/core/chain/node.cpp',
  'src/core/chain/input_node.cpp',
  'src/core/chain/output_node.cpp',
  'src/core/chain/converter_node.cpp',
  'src/core/chain/buffer_chain.cpp',
  'src/plugin/registry.cpp',
  'src/plugin/input/file_source.cpp',
  'src/plugin/input/flac_decoder.cpp',
  'src/plugin/input/miniaudio_decoder.cpp',
  'src/plugin/input/opus_decoder.cpp',
  'src/plugin/output/miniaudio_backend.cpp',
  'src/ipc/controller.cpp',
  'src/ipc/http_server.cpp',
  'src/ipc/socket_server.cpp',
  'src/ipc/stdin_control.cpp',
)

tuxedod = executable(
  'tuxedod',
  core_sources + files('src/daemon/main.cpp'),
  include_directories: [core_inc, miniaudio_inc, json_inc, httplib_inc],
  dependencies: deps,
  install: true,
)
