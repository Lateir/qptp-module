#!/system/bin/sh
# Magisk's default extraction makes ordinary files non-executable.
set_perm "$MODPATH/bin/qpro_streamer" 0 0 0755
