#!/system/bin/sh
# Magisk late_start service. TCP and discovery listen on Quest network interfaces.
MODDIR=${0%/*}
"$MODDIR/bin/qpro_streamer" 27182 200 &
