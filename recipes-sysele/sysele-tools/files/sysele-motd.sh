# Astrial H15 login banner. Installed by the sysele-tools package.
# Kept as a profile.d snippet rather than /etc/motd so that it does not
# collide with the /etc/motd shipped by base-files.

[ -r /opt/sysele/build-info ] && cat /opt/sysele/build-info
