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

install_data('io.snapcraft.SnapDesktopIntegration.desktop', install_dir: 'share/applications')
install_data('snapd-desktop-integration.svg', install_dir: 'share/icons/hicolor/scalable/apps')
