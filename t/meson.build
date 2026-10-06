test_deps = [
  dep_cmocka,
  input,
  glib,
  xml2,
  wlroots,
]

test_lib = static_library(
  'test_lib',
  sources: files(
    '../src/config/accel.c',
    '../src/common/parse-double.c',
    '../src/common/buf.c',
    '../src/common/mem.c',
    '../src/common/string-helpers.c',
    '../src/common/xml.c',
    '../src/common/parse-bool.c',
  ),
  include_directories: [labwc_inc],
  dependencies: test_deps,
)

tests = [
  'accel',
  'buf-simple',
  'gesture',
  'pressure-curve',
  'str',
  'xml',
]

foreach t : tests
  test(
    'test_@0@'.format(t),
    executable(
      'test_@0@'.format(t),
      sources: t == 'gesture' ? [
        '@0@.c'.format(t),
        '../src/config/gesturebind.c',
      ] : t == 'pressure-curve' ? [
        '@0@.c'.format(t),
        '../src/config/tablet-tool.c',
      ] : '@0@.c'.format(t),
      include_directories: [labwc_inc],
      link_with: [test_lib],
      dependencies: t in ['gesture', 'pressure-curve'] ? test_deps + [math] : test_deps,
    ),
    is_parallel: false,
  )
endforeach
