# Native SG2002 video path (without OpenCV)

## Without a new MMF binary: stock mode 6 → FIFO → TCP

The custom TCP mode 14 connects to the Mac, but the first observed frame pop
failed and no decoded frames have been confirmed. To distinguish native SDK
compatibility from a transport issue, this independent experiment uses the
**original firmware binary** `/mnt/system/usr/bin/test_mmf 6`, which was
previously able to record H.265. It replaces the regular output file with a
named FIFO, consumed by `nc`. There is **no video data written to storage**.

After a clean board restart, with other camera programs stopped:

```sh
sh /root/stream_stock_h265_fifo.sh
```

Mac (second terminal):

```sh
ffplay -f hevc tcp://10.22.149.1:8555
```

Upload [stream_stock_h265_fifo.sh](stream_stock_h265_fifo.sh) with `scp`
or fetch its raw GitHub URL. If it fails to start, inspect the printed
`/tmp/nano-h265.XXXXXX/camera.log`. The use of FIFO and netcat is an
**experimental diagnostic workaround**, not yet a tested end-to-end stream.
Netcat port 8555 has no authentication; only run on a trusted local network.

The Sipeed repository [issue #833](https://github.com/sipeed/LicheeRV-Nano-Build/issues/833)
reports VB/ION allocations retained after MMF shutdown on some firmware.
This does not prove our board has that leak; a clean restart provides a more
reliable comparison between stock and newly compiled binaries.

---

## Mode 14: live H.265 directly over TCP (experimental)

Mode 10 RTSP currently initializes ISP but fails to pop video frames.
The new mode 14 reuses the mode-6 VI/VENC sequence, which previously recorded
a decodable H.265 file. It opens a simple TCP listener instead of using the
RTSP implementation. It only starts camera resources after a viewer connects.
Stock mode 6 and patched mode 10 remain available.

Source: [native-sdk/add_tcp_stream_mode.py](add_tcp_stream_mode.py).

Build **succeeded** on 2026-11-13: [Actions run 43481847834](https://github.com/CactysFedya/licheeRV-Nano/actions/runs/43481847834).
[Download `native-tcp-h265-sg2002-riscv64`](https://github.com/CactysFedya/licheeRV-Nano/actions/runs/43481847834/artifacts/14064738812).
The archive contains `test_mmf_tcp`, a 12,415,504-byte statically linked RISC-V ELF.
SHA-256 of executable: `ac04b4360e55d8dee9e9f32e0bd8a715f674d7663d91a450f4139fb893f4987f`.
**Only the cross-build is verified; hardware streaming has not been validated.**

Mac: upload:

```bash
scp ~/Downloads/test_mmf_tcp root@10.22.149.1:/root/
ssh root@10.22.149.1
```

Board: with all other camera processes stopped:

```sh
chmod +x /root/test_mmf_tcp
/root/test_mmf_tcp 14
```

Wait for a message that TCP is listening on 0.0.0.0:8555.

Mac, new terminal:

```bash
ffplay -f hevc tcp://10.22.149.1:8555
```

This is raw H.265 without RTSP metadata, timestamps, authentication or TLS.
Use it only on a trusted private network. One client is supported.
Working live color, FPS and hardware stability remain unverified until tested.

---

## Build result — 2026-11-13

**Native RTSP executable cross-compiled successfully** in
[GitHub Actions run 43448362110](https://github.com/CactysFedya/licheeRV-Nano/actions/runs/43448362110).

- [Artifact download](https://github.com/CactysFedya/licheeRV-Nano/actions/runs/43448362110/artifacts/14048672873): `native-rtsp-sg2002-riscv64.zip`, containing `test_mmf_fixed`.
- Binary: 12,408,504 bytes, ELF 64-bit UCB RISC-V / RVC, double-float ABI, **statically linked**.
- SHA-256: `91f3f476b145fe59893a5104ae085e7bb1f28ff0009aed7086ef00fa5470a9d7`.
- Compiled from Sipeed SDK default branch and patch at commit `0cb255a109589612efd0ccc9758800a0f7b23044`.
- **Not yet run or tested on the physical LicheeRV Nano**. A successful cross-build does not establish RTSP availability, video color, or FPS.

### Deploy compiled file (Mac → board)

Unzip `native-rtsp-sg2002-riscv64.zip`, then from the folder containing `test_mmf_fixed`:

```bash
scp ./test_mmf_fixed root@10.22.149.1:/root/test_mmf_fixed
```

On the board (no other process using the camera):

```sh
chmod +x /root/test_mmf_fixed
cd /root/native-test
/root/test_mmf_fixed 10 2>&1 | tee /root/native-test/rtsp-fixed.log
```

From Mac (while binary runs):

```bash
ffplay -rtsp_transport tcp rtsp://10.22.149.1:8554/live/camera
```

The stock binary at `/mnt/system/usr/bin/test_mmf` remains untouched.
Full SDK cross-build is now manual-only in Actions to avoid accidental repeated large builds.

---

## Current implementation: patched native RTSP (2026-11-13)

**Source change committed, not yet compiled or verified on the user's board.**
Do not run more stock tests to diagnose already working H.265 recording.
The stock `test_mmf 6` produced a decodable 2560x1440/25fps file on this board.

The new patch scripts live here:

- `fix_mmf_rtsp.py`: guarded, idempotent patch to the Sipeed SDK;
- `build-native-rtsp.sh`: builds `test_mmf_fixed` with configured SDK libraries;
- [Native RTSP GitHub Actions](https://github.com/CactysFedya/licheeRV-Nano/actions/workflows/native-rtsp.yml): fast patch check on push, **manual full-build** option.

Fixes relative to stock mode 10:

1. Bind RTSP on `0.0.0.0:8554`, not just the interface auto-selected by Sipeed (board previously displayed `10.22.150.1` while SSH used `10.22.149.1`).
2. Hold the original VI frame until after VENC pop and free, as in the working mode 6.
3. Log first and every 100th VI frame, VI errors, and socket endpoint.
4. Fix upstream RTSP server handling for `/live` and `/live/camera`; the stock DESCRIBE and SETUP functions disagreed, and SETUP could dereference an empty filename.

### Option A: GitHub Actions (no local SDK setup)

Open [Native SG2002 RTSP (MMF)](https://github.com/CactysFedya/licheeRV-Nano/actions/workflows/native-rtsp.yml),
select **Run workflow**, and leave `full_build=true`. It will attempt the official
SG2002 C906 SDK middleware build and upload an artifact named
`native-rtsp-sg2002-riscv64` **only if the job succeeds**. This extensive
build is not yet proven to finish on a GitHub-hosted runner; inspect the
Actions job result. The fast CI check only validates patch applicability,
not runtime compatibility.

### Option B: existing Linux build server with prepared official SDK

From the local clone of this repository on **Linux** (not the Mac or the board):

```bash
git pull --ff-only
bash native-sdk/build-native-rtsp.sh /path/to/LicheeRV-Nano-Build
```

The SDK must already have `build/.config` and built middleware libraries.
For an unconfigured SDK, follow the [official MMF build instructions](https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/8_mmf_development_guide.html)
first. Current upstream README for a C906 target uses
`defconfig sg2002_licheervnano_sd`. Run `build_middleware` once before
the script; do not try to compile the complete SDK on the 128 MiB board.
The resulting executable is `native-sdk/out/test_mmf_fixed`.

### Install the successfully built binary

Copy the artifact or server-built `test_mmf_fixed` from the **Mac**:

```bash
scp ~/Downloads/test_mmf_fixed root@10.22.149.1:/root/test_mmf_fixed
```

**On the board**, with other camera programs stopped:

```sh
chmod +x /root/test_mmf_fixed
cd /root/native-test
/root/test_mmf_fixed 10 2>&1 | tee /root/native-test/rtsp-fixed.log
```

**On the Mac**, in a second terminal while the board process is alive:

```bash
ffplay -rtsp_transport tcp rtsp://10.22.149.1:8554/live/camera
```

Do not overwrite stock `/mnt/system/usr/bin/test_mmf`. On failure,
retain the startup log and see whether it reports `frame=1`, repeated
`VI pop failed`, encoder errors, or a socket initialization error.
On success, inspect actual image **color**, not just `yuv420p` metadata.
A functional RTSP session and observed frame rate still need device tests.

---


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


## Troubleshooting: VPSS group already exists (0xc0068004)

Observed on board after starting \`test_mmf 1\`:

\`\`\`
[_mmf_vpss_init_new]-1150: CVI_VPSS_CreateGrp(grp:0) failed with 0xc0068004, retry!
\`\`\`

This is **CVI_ERR_VPSS_EXIST** (error code \`EN_ERR_EXIST = 4\`), as defined in [cvi_errno.h](https://github.com/sipeed/LicheeRV-Nano-Build/blob/main/osdrv/interdrv/v2/include/common/uapi/linux/cvi_errno.h). The [VPSS driver source](https://github.com/sipeed/LicheeRV-Nano-Build/blob/main/osdrv/interdrv/v2/vpss/common/vpss.c) returns this error when \`vpssCtx[VpssGrp]\` is already occupied.

This is a resource conflict, **not evidence that GC4653 or the sensor pin mapping is wrong**. Potential causes: a prior camera process still owns VPSS group 0, or driver state was not cleaned up after termination.

Recovery:
1. Stop every previous camera test with Ctrl+C in its SSH session (do not run two at once).
2. On board, run \`ps | grep -E '[t]est_mmf|[C]SIStream|[l]ichee_color_stream|[s]ensor_test'\` and \`netstat -lnt | grep 8554\`.
3. If an old \`test_mmf\` still runs and you own it, stop it gracefully; restart from a clean state. Avoid killing unrelated processes.
4. If none remains but VPSS group 0 stays allocated, reboot normally (\`reboot\`) to reset kernel/media state; SSH will disconnect. After reconnect, start **only** \`/mnt/system/usr/bin/test_mmf 10\` (do not run mode 1 concurrently).
5. Observe whether it prints \`rtsp://<ip>:8554/live\` and keeps running, then on Mac run \`ffplay -rtsp_transport tcp rtsp://10.22.149.1:8554/live\` (or actual reachable board IP). If refused, check \`netstat -lnt | grep 8554\`, board IP, and server stdout.

Note: \`Connection refused\` observed **before** starting RTSP mode 10 is expected and doesn't diagnose the newly started service. RTSP address printed by the sample may pick a different USB network interface; select an actually reachable address.

This troubleshooting does **not** imply live RTSP has been verified; validation requires playback and recorded FPS.
