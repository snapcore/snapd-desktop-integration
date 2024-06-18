conf = configuration_data()
conf.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
configure_file(output: 'config.h',
               configuration: conf)

snapd_desktop_integration = executable(
  'snapd-desktop-integration',
  'main.c',
  'sdi-notify.c',
  'sdi-snap.c',
  'sdi-refresh-dialog.c',
  'sdi-refresh-monitor.c',
  'sdi-theme-monitor.c',
  'sdi-helpers.c',
  resources, login_src, login_session_src, unity_launcher_src,
  dependencies: [gtk_dep, snapd_glib_dep, libnotify_dep, unity_dep],
  install: true,
  link_args: ['-rdynamic'],
)

daemon_builddir = meson.current_build_dir()
