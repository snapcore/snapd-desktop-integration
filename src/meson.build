
snapd_desktop_integration = executable(
  'snapd-desktop-integration',
  'main.c',
  'ds-theme-set.c',
  'ds-theme-watcher.c',
  'ds-snapd-helper.c',
  dependencies: [gtk_dep, snapd_glib_dep, libnotify_dep],
  install: true,
)
