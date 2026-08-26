# Astrial H15 customizations
# System Electronics

# System Electronics board tools. Development image only: a production image
# leaves this line out and the whole /opt/sysele tree disappears with it.
IMAGE_INSTALL:append = " sysele-tools"
