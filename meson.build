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

# miniaudio — single-header, compiled into its own TU in src/output
miniaudio_inc = include_directories('vendor/miniaudio')

deps = [thread_dep, flac_dep]

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
  'src/core/chain/buffer_chain.cpp',
  'src/plugin/registry.cpp',
  'src/plugin/input/file_source.cpp',
  'src/plugin/input/flac_decoder.cpp',
  'src/plugin/input/miniaudio_decoder.cpp',
  'src/plugin/output/miniaudio_backend.cpp',
  'src/ipc/stdin_control.cpp',
)

tuxedod = executable(
  'tuxedod',
  core_sources + files('src/daemon/main.cpp'),
  include_directories: [core_inc, miniaudio_inc],
  dependencies: deps,
  install: true,
)
