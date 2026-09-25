#!/system/bin/sh
# Magisk late_start service. Native sampler listens only on Quest loopback.
MODDIR=${0%/*}
"$MODDIR/bin/qpro_streamer" 27182 200 &
