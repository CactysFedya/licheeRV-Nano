#!/usr/bin/env python3
"""Patch the Sipeed SDK's test_mmf RTSP camera mode, not its working mode 6.

Source revision validated against:
https://github.com/sipeed/LicheeRV-Nano-Build/blob/main/middleware/v2/sample/test_mmf/sample_vio.c

Patch is deliberately bounded to _test_vi_venc_h265_rtsp and fails closed if
upstream changes the expected code. It is idempotent and creates a backup.
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import sys

START = "static int _test_vi_venc_h265_rtsp(void)"
END = "static int _test_multiple_vi(void)"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count == 0 and new in text:
        print("Already patched:", label)
        return text
    if count != 1:
        raise RuntimeError(
            f"{label}: expected exactly 1 old occurrence, found {count}; "
            "SDK version differs; refusing unsafe patch"
        )
    print("Patched:", label)
    return text.replace(old, new, 1)


def patch(path: pathlib.Path, dry_run: bool) -> None:
    content = path.read_text(encoding="utf-8")
    if content.count(START) != 1 or content.count(END) != 1:
        raise RuntimeError("Expected MMF mode 10 boundaries not found")
    before, body_and_after = content.split(START, 1)
    body, after = body_and_after.split(END, 1)
    body = START + body

    body = replace_once(
        body,
        "rtsp_server_init(NULL, 8554)",
        'rtsp_server_init("0.0.0.0", 8554)',
        "bind RTSP port 8554 to all network interfaces",
    )

    body = replace_once(
        body,
        '''\tif (0 != mmf_add_vi_channel(vi_ch, img_w, img_h, img_fmt)) {
\t\tDEBUG("mmf_add_vi_channel failed!\\r\\n");
\t\tmmf_deinit();
\t\treturn -1;
\t}''',
        '''\tif (0 != mmf_add_vi_channel(vi_ch, img_w, img_h, img_fmt)) {
\t\tfprintf(stderr, "[native-rtsp] mmf_add_vi_channel failed\\n");
\t\tmmf_deinit();
\t\treturn -1;
\t}''',
        "make VI channel failures visible without DEBUG_EN",
    )

    body = replace_once(
        body,
        '''\tuint64_t start = _get_time_us();
\tuint64_t last_loop_us = start;
\twhile (!exit_flag) {''',
        '''\tuint64_t start = _get_time_us();
\tuint64_t last_loop_us = start;
\tunsigned long long frame_count = 0;
\tunsigned long long vi_error_count = 0;
\tfprintf(stderr,
\t\t"[native-rtsp] listening on 0.0.0.0:8554; use rtsp://BOARD_IP:8554/live/camera\\n");
\tfprintf(stderr, "[native-rtsp] entering VI capture loop, waiting for first frame\\n");
\twhile (!exit_flag) {''',
        "print actual RTSP endpoint and capture state",
    )

    body = replace_once(
        body,
        '''\t\tif (mmf_vi_frame_pop(vi_ch, &data, &data_size, &width, &height, &format)) {
\t\t\tcontinue;
\t\t}''',
        '''\t\tif (mmf_vi_frame_pop(vi_ch, &data, &data_size, &width, &height, &format)) {
\t\t\t++vi_error_count;
\t\t\tif (vi_error_count == 1 || vi_error_count % 100 == 0) {
\t\t\t\tfprintf(stderr, "[native-rtsp] VI pop failed (%llu times)\\n",
\t\t\t\t\tvi_error_count);
\t\t\t}
\t\t\tusleep(10000);
\t\t\tcontinue;
\t\t}
\t\t++frame_count;
\t\tif (frame_count == 1 || frame_count % 100 == 0) {
\t\t\tfprintf(stderr,
\t\t\t\t"[native-rtsp] frame=%llu size=%dx%d bytes=%d pixel_fmt=%d\\n",
\t\t\t\tframe_count, width, height, data_size, format);
\t\t}''',
        "log first and every 100th frame, plus VI failures",
    )

    body = replace_once(
        body,
        '''\t\tstart = _get_time_us();
\t\tmmf_vi_frame_free(vi_ch);
\t\tDEBUG("use %ld us\\r\\n", _get_time_us() - start);

\t\tstart = _get_time_us();
\t\tmmf_h265_stream_t stream;''',
        '''\t\t/* Keep the source frame alive until VENC has returned the stream. */
\t\tstart = _get_time_us();
\t\tmmf_h265_stream_t stream;''',
        "do not release VI frame before VENC pop",
    )

    body = replace_once(
        body,
        '''\t\tif (mmf_enc_h265_free(ch)) {
\t\t\tprintf("mmf_enc_h265_free failed\\n");
\t\t\tgoto _exit;
\t\t}
\t\tDEBUG("use %ld us\\r\\n", _get_time_us() - start);''',
        '''\t\tif (mmf_enc_h265_free(ch)) {
\t\t\tfprintf(stderr, "[native-rtsp] mmf_enc_h265_free failed\\n");
\t\t\tgoto _exit;
\t\t}
\t\t/* Match working mode 6: VENC push -> pop -> free -> VI free. */
\t\tmmf_vi_frame_free(vi_ch);
\t\tDEBUG("use %ld us\\r\\n", _get_time_us() - start);''',
        "release VI frame only after VENC stream is freed",
    )

    body = replace_once(
        body,
        '''\tif (0 != rtsp_server_deinit()) {
\t\tprintf("rtsp server deinit\\n");
\t\treturn 0;
\t}''',
        '''\tif (0 != rtsp_server_deinit()) {
\t\tfprintf(stderr, "[native-rtsp] rtsp_server_deinit failed\\n");
\t\treturn -1;
\t}''',
        "show RTSP cleanup failures",
    )

    patched = before + body + END + after
    if content == patched:
        print("No changes necessary:", path)
        return
    if dry_run:
        print("Dry run OK:", path)
        return
    backup = path.with_suffix(path.suffix + ".orig")
    if not backup.exists():
        shutil.copy2(path, backup)
    path.write_text(patched, encoding="utf-8")
    print("Wrote:", path)
    print("Original backup:", backup)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sdk_root", type=pathlib.Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    source = args.sdk_root.resolve() / "middleware/v2/sample/test_mmf/sample_vio.c"
    if not source.is_file():
        parser.error(f"Source not found: {source}")
    try:
        patch(source, args.dry_run)
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
