if get_option('labnag').allowed()
  wayland_client = dependency('wayland-client')
  wayland_cursor = dependency('wayland-cursor')

  nag_sources = files(
    'labnag.c',
    'pool-buffer.c',
  )

  wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

  protocols = [
    wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    '../protocols/wlr-layer-shell-unstable-v1.xml',
  ]

  foreach xml : protocols
    nag_sources += custom_target(
      xml.underscorify() + '_c',
      input: xml,
      output: '@BASENAME@-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
    nag_sources += custom_target(
      xml.underscorify() + '_client_h',
      input: xml,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
  endforeach

  if host_machine.system() in ['freebsd', 'openbsd']
    # For signalfd()
    epoll_dep = dependency('epoll-shim')
  else
    epoll_dep = []
  endif

  executable(
    'labnag',
    nag_sources,
    dependencies: [
      cairo,
      pangocairo,
      glib,
      wayland_client,
      wayland_cursor,
      wlroots,
      server_protos,
      epoll_dep,
      xkbcommon,
    ],
    include_directories: [labwc_inc],
    install: true,
  )
endif

clients = files('lab-sensible-terminal')
install_data(clients, install_dir: get_option('bindir'))

if get_option('material-test-client').enabled()
  material_client_sources = files('singularity-material-test.c')
  material_wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
  material_client_protocols = [
    material_wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    '../protocols/wlr-layer-shell-unstable-v1.xml',
    '../protocols/singularity-blur-unstable-v1.xml',
  ]

  foreach xml : material_client_protocols
    material_client_sources += custom_target(
      'material_' + xml.underscorify() + '_c',
      input: xml,
      output: '@BASENAME@-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
    material_client_sources += custom_target(
      'material_' + xml.underscorify() + '_client_h',
      input: xml,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
  endforeach

  executable(
    'singularity-material-test',
    material_client_sources,
    dependencies: [dependency('wayland-client')],
    install: false,
  )
endif
