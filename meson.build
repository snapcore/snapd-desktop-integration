project('snapd-desktop-integration', 'c', version: '0.9beta2')

if get_option('gnotify')
    add_global_arguments('-DUSE_GNOTIFY', language: 'c')
endif

gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gtk_dep = dependency('gtk4', version: '>= 4.0')
snapd_glib_dep = dependency('snapd-glib-2', required: false, version: '>= 1.60')
if not snapd_glib_dep.found()
    snapd_glib_dep = dependency('snapd-glib', version: '>= 1.60')
endif
libnotify_dep = dependency('libnotify', version: '>= 0.7.7')
libsoup_dep = dependency ('libsoup-3.0')
json_glib_dep = dependency ('json-glib-1.0')

add_global_arguments('-DGETTEXT_PACKAGE="@0@"'.format (meson.project_name()), language:'c')

subdir('data')
subdir('po')
subdir('src')
subdir('tests')
