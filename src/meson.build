labwc_sources = files(
  'a11y.c',
  'action.c',
  'background-effect.c',
  'custom-shaders.c',
  'buffer.c',
  'color-filter.c',
  'debug.c',
  'desktop.c',
  'dnd.c',
  'edges.c',
  'idle.c',
  'interactive.c',
  'layers.c',
  'magnifier.c',
  'main.c',
  'motion.c',
  'node.c',
  'output.c',
  'output-config.c',
  'output-color.c',
  'output-state.c',
  'output-virtual.c',
  'overlay.c',
  'placement.c',
  'regions.c',
  'resistance.c',
  'resize-outlines.c',
  'seat.c',
  'server.c',
  'session-lock.c',
  'show-desktop.c',
  'singularity-splash.c',
  'snap-constraints.c',
  'snap.c',
  'tearing.c',
  'theme.c',
  'view.c',
  'view-animation.c',
  'view-impl-common.c',
  'window-rules.c',
  'workspaces.c',
  'xdg.c',
  'xdg-popup.c',
)

if have_xwayland
  labwc_sources += files(
    'xwayland.c',
    'xwayland-scale.c',
    'xwayland-unmanaged.c',
  )
endif

if have_libsfdo
  labwc_sources += files(
    'desktop-entry.c',
  )
endif

subdir('common')
subdir('config')
subdir('cycle')
subdir('decorations')
subdir('foreign-toplevel')
subdir('img')
subdir('input')
subdir('menu')
subdir('protocols')
subdir('scaled-buffer')
subdir('ssd')
