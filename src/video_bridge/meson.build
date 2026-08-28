project('android-debian-video-codec-bridge', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=3', 'werror=true'])

inc = include_directories('include')
protocol = static_library('advc-protocol',
  'src/protocol.c',
  'src/capabilities_json.c',
  'src/codec_discovery.c',
  'src/broker.c',
  'src/client.c',
  'src/glibc_import.c',
  'src/ahb_prime_mapper.c',
  'src/dmabuf_ingress.c',
  'src/session_engine.c',
  'src/transport_policy.c',
  'src/byte_range.c',
  include_directories: inc)

executable('advc-ctl',
  'tools/advc_ctl.c',
  include_directories: inc,
  link_with: protocol)

if host_machine.system() == 'android'
  android_cc = meson.get_compiler('c')
  android_dep = android_cc.find_library('android', required: true)
  mediandk_dep = android_cc.find_library('mediandk', required: true)
  static_library('advc-encode-surface',
    'android/encode_surface_core.c',
    'android/encode_surface_ndk.c',
    'android/encode_surface_egl.c',
    'android/encode_surface_vulkan.c',
    'android/surface_dmabuf_probe.c',
    'android/android_prime_mapper.c',
    'android/stable_mapper_metadata.c',
    include_directories: include_directories('android'),
    link_with: protocol,
    dependencies: [android_dep, mediandk_dep,
      android_cc.find_library('EGL', required: true),
      android_cc.find_library('GLESv2', required: true),
      android_cc.find_library('vulkan', required: true)])
endif

if host_machine.system() != 'android'
  test_c_args = ['-UNDEBUG']
  protocol_test = executable('protocol_test',
    'tests/protocol_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('protocol', protocol_test)

  codec_discovery_test = executable('codec_discovery_test',
    'tests/codec_discovery_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('codec-discovery', codec_discovery_test)

  session_engine_test = executable('session_engine_test',
    'tests/session_engine_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('session-engine', session_engine_test)

  client_test = executable('client_test',
    'tests/client_test.c',
    'tools/advc_annexb.c',
    include_directories: [inc, include_directories('tools')],
    c_args: test_c_args,
    link_with: protocol)
  test('client', client_test)

  bitstream_test = executable('bitstream_test',
    'tests/bitstream_test.c',
    'tools/advc_bitstream.c',
    include_directories: include_directories('tools'),
    c_args: test_c_args)
  test('bitstream', bitstream_test)

  android_codec_backend_test = executable('android_codec_backend_test',
    'tests/android_codec_backend_test.c',
    'android/android_codec_backend.c',
    'tests/android_ahb_decode_stub.c',
    'tests/encode_surface_backend_stub.c',
    'src/byte_range.c',
    include_directories: [
      inc,
      include_directories('android'),
      include_directories('tests/android_stubs'),
    ],
    c_args: test_c_args)
  test('android-codec-backend', android_codec_backend_test)

  encode_surface_test = executable('encode_surface_test',
    'tests/encode_surface_test.c',
    'android/encode_surface_core.c',
    include_directories: include_directories('android'),
    c_args: test_c_args)
  test('encode-surface', encode_surface_test)

  glibc_import_test = executable('glibc_import_test',
    'tests/glibc_import_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('glibc-import', glibc_import_test)

  dmabuf_ingress_test = executable('dmabuf_ingress_test',
    'tests/dmabuf_ingress_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('dmabuf-ingress', dmabuf_ingress_test)

  ahb_prime_mapper_test = executable('ahb_prime_mapper_test',
    'tests/ahb_prime_mapper_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('ahb-prime-mapper', ahb_prime_mapper_test)

  transport_policy_test = executable('transport_policy_test',
    'tests/transport_policy_test.c',
    include_directories: inc,
    c_args: test_c_args,
    link_with: protocol)
  test('transport-policy', transport_policy_test)

  stable_mapper_metadata_test = executable('stable_mapper_metadata_test',
    'tests/stable_mapper_metadata_test.c',
    'android/stable_mapper_metadata.c',
    include_directories: [inc, include_directories('android')],
    c_args: test_c_args)
  test('stable-mapper-metadata', stable_mapper_metadata_test)

  executable('advc-decode-smoke',
    'tools/advc_decode_smoke.c',
    'tools/advc_annexb.c',
    include_directories: [inc, include_directories('tools')],
    link_with: protocol)

  executable('advc-encode-smoke',
    'tools/advc_encode_smoke.c',
    'tools/advc_bitstream.c',
    include_directories: [inc, include_directories('tools')],
    link_with: protocol)
endif
