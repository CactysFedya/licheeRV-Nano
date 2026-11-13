#!/bin/sh
# Read-only health/capability probe for Sipeed LicheeRV Nano SG2002.
# Does not open the camera or modify system configuration.
set -u
echo "=== SYSTEM ==="
uname -a
cat /etc/os-release 2>/dev/null | head -8 || true
echo "=== CAMERA SENSOR CONFIG ==="
if [ -r /mnt/data/sensor_cfg.ini ]; then
  grep -E '^[[:space:]]*(name|bus_id|sns_i2c_addr|mipi_dev|lane_id|pn_swap|mclk_en|mclk|fps)[[:space:]]*=' /mnt/data/sensor_cfg.ini || true
else
  echo "No readable /mnt/data/sensor_cfg.ini"
fi
echo "=== SDK BINARIES ==="
for f in \
  /mnt/system/usr/bin/sensor_test \
  /mnt/system/usr/bin/sample_vio \
  /mnt/system/usr/bin/sample_venc \
  /mnt/system/usr/bin/test_mmf \
  /mnt/system/usr/bin/sample_framediffmotion \
  /root/lichee-ai/bin/CSIStream; do
  if [ -f "$f" ]; then ls -lh "$f"; else echo "missing: $f"; fi
done
echo "=== MEDIA DEVICES ==="
ls -l /dev/cvi* 2>/dev/null || true
echo "=== MEMORY ==="
grep -E 'MemTotal|MemAvailable|CmaTotal|CmaFree' /proc/meminfo || true
echo "=== TEMPERATURE (C) ==="
if [ -r /sys/class/thermal/thermal_zone0/temp ]; then
  awk '{printf "%.2f\n", $1/1000}' /sys/class/thermal/thermal_zone0/temp
fi
echo "=== POTENTIAL CAMERA USERS ==="
ps | grep -E '[C]SIStream|[c]olor_stream|[s]ensor_test|[s]ample_vio|[t]est_mmf' || true
echo "=== SDK SHARED LIBS (first matches) ==="
find /mnt/system -type f \( -name 'libvpu.so*' -o -name 'libsys.so*' -o -name 'libcvi_ive.so*' -o -name 'libvenc.so*' \) 2>/dev/null | head -30 || true
echo "=== PROBE COMPLETE: no camera opened ==="
