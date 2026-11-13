# Native SG2002 video path (without OpenCV)

This project will use the **Sipeed/CVITEK MMF SDK** instead of opencv-mobile for camera capture and video processing.

## Hardware verified in previous board tests

- LicheeRV Nano, Buildroot, RISC-V, measured 128 MiB RAM.
- GC4653 MIPI CSI, working SDK configuration in `/mnt/data/sensor_cfg.ini`:
  `bus_id=4`, `sns_i2c_addr=29`, `lane_id=4,3,2,-1,-1`, `mclk_en=1`, `mclk=1`.
- `/mnt/system/usr/bin/sensor_test` successfully captured color NV21 raw frames at 2560x1440.
- `sample_venc` with `--testMode=2 --bindmode=1 --sensorEn=1` was **not** a working live encoder setup; do not reuse it without debugging.
- The opencv-mobile-based color streamer (ports 7778) fails with `CVI_VI_GetChnFrame failed c00e8040`. Do not use as the baseline.
- A third-party OpenCV MJPEG demo did show live monochrome frames, but with ~8 FPS. Keep it as a fallback only.

## Official source references

- [Sipeed LicheeRV-Nano-Build](https://github.com/sipeed/LicheeRV-Nano-Build)
- [Sipeed MMF development guide](https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/8_mmf_development_guide.html)
- [Native VIO sample](https://github.com/sipeed/LicheeRV-Nano-Build/tree/main/middleware/v2/sample/vio)
- [Sensor test / GC4653 config](https://github.com/sipeed/LicheeRV-Nano-Build/tree/main/middleware/v2/sample/sensor_test)
- [MMF capture + RTSP examples](https://github.com/sipeed/LicheeRV-Nano-Build/tree/main/middleware/v2/sample/test_mmf)
- [IVE frame-difference source](https://github.com/sipeed/LicheeRV-Nano-Build/blob/main/middleware/v2/sample/ive/src/sample_framediffmotion.c)

## Planned native pipeline

```
GC4653 -> MIPI CSI -> VI/ISP -> VPSS
                              |-> VENC H.264/H.265 -> RTSP/stream
                              |-> YUV (luma + chroma) -> IVE/C algorithms
```

First deliverable: **stable, color, hardware-encoded stream with true measured FPS**. Then run edge-video detectors on scaled frames: object motion/intrusion, smoke, and displacement of elongated objects. OpenCV can be optional on the development computer for experiments, but is not a runtime dependency of capture, encoding or basic IVE image processing.

### Phase A — inspect the existing board (no new dependencies)

On the **board** and with camera applications stopped:

```sh
sh /root/inspect-board.sh
/mnt/system/usr/bin/sample_vio -h
```

`sample_vio -h` should print available native sample modes; some are for a connected display, so don't select a random mode before examining its usage.

We already have a native YUV color frame at `/root/sample_0.yuv`. On **Mac**, if it is still present:

```sh
scp root@10.22.149.1:/root/sample_0.yuv ~/Desktop/
ffmpeg -hide_banner -f rawvideo -pixel_format nv21 \
  -video_size 2560x1440 -i ~/Desktop/sample_0.yuv \
  -frames:v 1 -y ~/Desktop/native_color.png
open ~/Desktop/native_color.png
```

This proves SDK color frame extraction, **not yet live streaming**.

### Phase B — build stock MMF examples on Linux (server, not on 128 MiB board)

Sipeed's [official guide](https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/8_mmf_development_guide.html) documents SDK checkout, host toolchain, `source build/cvisetup.sh`, `defconfig cv1812cp_licheerv_nano_sd`, and `build_middleware`. Start from their documented tag `v4.1.0-licheervnano`; verify compatibility with the actual board firmware and sensor config before running.

Then test building:

```sh
cd middleware/v2/sample/vio && make
cd ../test_mmf && make
cd ../ive && make
```

The `test_mmf` target and its RTSP server dependencies are **not yet verified to build** on our server/firmware. Do not assume that a successful build guarantees runtime compatibility.

The official `middleware/v2/sample/test_mmf/sample_vio_main.c` declares:
- `test_mmf 1`: camera (VI) capture;
- `test_mmf 10`: VI -> H.265 encoder -> RTSP;
- `test_mmf 12`: VI -> H.265 encoder + region -> RTSP.

Source `sample_vio.c` prints `rtsp://<board-ip>:8554/live` in mode 10. After successful native build and deployment, only **one** process may own the CSI camera. Try a short test, monitor temperature, and inspect actual stdout before relying on the stream:

```sh
# BOARD: use the newly built binary, only if copied and confirmed
./test_mmf 10
```

```sh
# MAC: while the native test_mmf RTSP server is running
ffplay -rtsp_transport tcp rtsp://10.22.149.1:8554/live
```

### Phase C — native image processing

Sipeed provides `sample_framediffmotion.c`, calling `CVI_IVE_FrameDiffMotion`. It processes two **U8C1** (grayscale) input images and uses subtraction -> threshold -> erosion -> dilation. It is a useful first building block for motion detection. Video can remain color for streaming and object analysis; only the luminance plane is needed for this detector.

The smoke and displacement detectors will need additional temporal/shape algorithms and validation; IVE alone does not supply ready-made smoke detection or metric 5 cm displacement. Camera calibration / known scene geometry is required to infer metric displacement.

## Constraints and cautions

- This MMF API works directly with hardware. Incorrect buffer ownership, format assumptions or initialization can destabilize the board. Base code on official samples and release each VI/VPSS/VENC frame correctly.
- Our observed memory is only 128 MiB available as total RAM; build on a Linux server and prefer scaled analysis frames.
- The official `sample_venc` live-stream command attempted earlier has not produced a file. Start from `test_mmf 10`, which explicitly handles capture, encoding and RTSP, rather than guessing sample_venc flags.
- Avoid running `CSIStream`, `sensor_test`, `test_mmf`, or other VI users simultaneously.
- No claim of working RTSP/25 FPS until measured on the hardware.

## First acceptance criteria

1. Native VI returns distinct color frames with no `CVI_VI_GetChnFrame` errors.
2. VENC produces decodable H.264/H.265 stream; browser/player and Mac receive it.
3. FPS, input dimensions, decode quality, and SoC temperature are logged.
4. IVE motion test works on two actual frames and later from live Y channel.
