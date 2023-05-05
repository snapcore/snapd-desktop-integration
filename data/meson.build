subdir('resources')

gnome = import('gnome')

# The returned source would be passed to another target
login_src = gnome.gdbus_codegen('org.freedesktop.login1',
  sources: 'org.freedesktop.login1.dbus.xml',
  interface_prefix : 'org.freedesktop'
)

login_session_src = gnome.gdbus_codegen('org.freedesktop.login1.Session',
  sources: 'org.freedesktop.login1.session.dbus.xml',
  interface_prefix : 'org.freedesktop.login1.session'
)

gdbus_src = gnome.gdbus_codegen('io.snapcraft.SnapDesktopIntegration',
  sources: 'io.snapcraft.SnapDesktopIntegration.dbus.xml',
  interface_prefix : 'io.snapcraft.',
  docbook : 'SnapDesktopIntegration-interface-doc'
)

snap_changes_src = gnome.gdbus_codegen('io.snapcraft.SnapChanges',
  sources: 'io.snapcraft.SnapChanges.dbus.xml',
  interface_prefix : 'io.snapcraft.',
  docbook : 'SnapChanges-interface-doc'
)
