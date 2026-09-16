# Astrial H15: put the System Electronics tools on the PATH.
# Installed by the sysele-tools package, alongside the symlinks in ${bindir}:
# this covers the login shells, the symlinks cover everything else.

case ":${PATH}:" in
    *:/opt/sysele/bin:*) ;;
    *) PATH="/opt/sysele/bin:${PATH}" ;;
esac
export PATH
