# LicheeRV Nano: 1080p/30 FPS direct CVITEK SDK capture

This directory contains a dedicated **capture benchmark**, not a video server. It uses the vendor's `SAMPLE_*` routines **only to initialize GC4653/VI/ISP**, then uses CVITEK `CVI_VPSS_GetChnFrame()` and `CVI_VPSS_ReleaseChnFrame()` directly. The stream is 1920 x 1080 NV21. It does not use OpenCV, MMF `mmf_vi_frame_pop`, VENC, RTSP or MJPEG.

It explicitly creates and attaches a private VB pool to VPSS group 0, channel 0. This prevents the previously observed automatic VB pool selection issue (`Can't acquire VB BLK for VPSS`). The program creates its own pools at start; do **not** run the external VPSS pool fix utility with it.

## Build (Linux host with fully built Sipeed SDK)

Use the SDK/tree that matches the board image. You must have completed `build_middleware` and have working `MW_*` paths / cross compiler; merely downloading a cross compiler is not enough to link CVITEK libraries and GC4653 ISP support. Recommended vendor source: [sipeed/LicheeRV-Nano-Build](https://github.com/sipeed/LicheeRV-Nano-Build).

```sh
# In your local clone of CactysFedya/licheeRV-Nano:
# Replace /srv/LicheeRV-Nano-Build with YOUR actual SDK checkout.
SDK=/srv/LicheeRV-Nano-Build
mkdir -p "$SDK/middleware/v2/sample/lichee_capture30"
cp native-sdk/lichee_capture30/main.c native-sdk/lichee_capture30/Makefile \
  "$SDK/middleware/v2/sample/lichee_capture30/"

cd "$SDK"
source build/cvisetup.sh
# Select the board's matching defconfig; build_middleware must be completed.
cd middleware/v2/sample/lichee_capture30
make
file ./lichee_capture30
```

If `Makefile.param` or the required CVITEK static libraries are missing, complete the official middleware setup first. This project is not a generic host-Linux program and cannot be compiled solely with GCC headers.

## Board run

The existing board configuration `/mnt/data/sensor_cfg.ini` must specify `GCORE_GC4653_MIPI_4M_30FPS_10BIT`. Stop any existing `test_mmf`, `CSIStream`, `sensor_test` or other CSI users before running. Do not kill unrelated processes, and do not run two camera apps at once.

```sh
# On Mac / Linux host, after cross compilation:
scp ./lichee_capture30 root@10.22.149.1:/root/

# On the board, in SSH:
chmod +x /root/lichee_capture30
/root/lichee_capture30 --frames 300 --warmup 30
```

Expected output shape (illustrative; actual performance must be measured on the board):

```text
[CONFIG] sensor=2560x1440 -> VPSS=1920x1080 NV21, target=300 frames after 30 warmup
[VB] VI pool: ...; VPSS pool: id=... ...
[FRAME] 1920x1080 format=... stride=.../... length=.../... pts=...
[PROGRESS] frames=100 elapsed=... fps=...
[RESULT] resolution=1920x1080 format=NV21 received=300 expected=300 fps=... errors=0 wrong_format=0 duplicate_pts=0 elapsed=...
[RESULT] PASS or FAIL
```

`fps` is `(N-1)/(time_of_last_frame - time_of_first_frame)` after 30 warm-up frames. `PASS` requires 300 frames, >=29.0 measured FPS, no SDK get/release errors, correct 1080p NV21 attributes and no repeated `(PTS, frame number)` pairs. There is no output file or stream: the point is to validate direct delivery. The sensor/VI seeing 30 FPS by itself is insufficient.

For live driver counters in a separate SSH session:

```sh
cat /proc/cvitek/vpss
cat /proc/cvitek/vb
```

If initialization reports group exists, stop other media applications and reset the device if necessary. Never manipulate VPSS configuration concurrently with another running capture pipeline. Firmware 2023 MMF and 2026 kernel builds may have compatibility differences; this program is **not hardware-verified** until the target binary is built and run on the physical board.
