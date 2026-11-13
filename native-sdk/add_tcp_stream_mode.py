#!/usr/bin/env python3
"""Add mode 14: live raw H.265 over TCP, based on the *working* mode 6 path.

The existing mode 6 and patched RTSP mode 10 are unchanged.
No OpenCV, MJPEG, RTSP server, or external TCP streaming libraries.
"""
import argparse
from pathlib import Path
import shutil
import sys

SOURCE_START = "static int _test_vi_venc_h265(void)"
SOURCE_END = "static int _test_rtsp_h264(void)"
MODE_NAME = "_test_vi_venc_h265_tcp"
MARKER = "/* NODRIX_TCP_H265_MODE14 */"


def once(src, old, new, where):
    count = src.count(old)
    if count != 1:
        raise ValueError("%s: need exactly 1 occurrence of target, found %d" % (where, count))
    return src.replace(old, new, 1)


def write_if_changed(path, old, new, dry_run):
    if new == old:
        print("No changes:", path)
        return
    if dry_run:
        print("Dry run OK:", path)
        return
    backup = path.with_suffix(path.suffix + ".before_tcp14")
    if not backup.exists():
        shutil.copy2(path, backup)
    path.write_text(new, encoding="utf8")
    print("Wrote:", path)


def patch_vio(src):
    if MARKER in src:
        print("Mode 14 already installed in sample_vio.c")
        return src
    if src.count(SOURCE_START) != 1 or src.count(SOURCE_END) != 1:
        raise ValueError("Cannot locate official mode 6 function")
    a = src.index(SOURCE_START)
    b = src.index(SOURCE_END, a)
    original = src[a:b]
    clone = original.replace(SOURCE_START, "static int %s(void)" % MODE_NAME, 1)

    # Start with the working mode 6, but wait for an actual TCP client BEFORE
    # initializing VI/ISP/VENC. This avoids any concurrent RTSP socket thread.
    init = r"""
    signal(SIGPIPE, SIG_IGN);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[tcp-h265] socket");
        return -1;
    }
    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(8555);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0 ||
        listen(server_fd, 1) < 0) {
        perror("[tcp-h265] bind/listen");
        close(server_fd);
        return -1;
    }
    fprintf(stderr, "[tcp-h265] listening on 0.0.0.0:8555; "
                    "connect Mac with: ffplay -f hevc tcp://BOARD_IP:8555\n");
    int client_fd = accept(server_fd, NULL, NULL);
    close(server_fd);
    if (client_fd < 0) {
        perror("[tcp-h265] accept");
        return -1;
    }
    struct timeval send_timeout = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
               &send_timeout, sizeof(send_timeout));
    fprintf(stderr, "[tcp-h265] client connected, starting working mode 6 camera path\n");
"""
    clone = once(
        clone,
        "{\n\tuint8_t *filebuf = NULL;",
        "{\n" + init + "\tuint8_t *filebuf = NULL;",
        "socket and accept",
    )

    # Replace ONLY mode-6 file-writing block with socket sending.
    begin = clone.index("\t\t\tstatic FILE *fp = NULL;")
    end = clone.index("\n\t\t}", begin)
    old_writer = clone[begin:end]
    writer = r"""
            static unsigned long long packet_count = 0;
            for (int i = 0; i < stream.count; ++i) {
                if (stream.data_size[i] > 0 &&
                    tcp_h265_send_all(client_fd,
                                      (const uint8_t *)stream.data[i],
                                      (size_t)stream.data_size[i]) != 0) {
                    perror("[tcp-h265] client disconnected or send failure");
                    send_failed = 1;
                    break;
                }
            }
            ++packet_count;
            if (packet_count == 1 || packet_count % 100 == 0) {
                fprintf(stderr, "[tcp-h265] encoded frames=%llu, chunks=%d\n",
                        packet_count, stream.count);
            }
"""
    clone = clone[:begin] + writer + clone[end:]
    clone = once(
        clone,
        "\t\tstart = _get_time_us();\n\t\t{\n",
        "\t\tstart = _get_time_us();\n\t\tint send_failed = 0;\n\t\t{\n",
        "declare send_failed outside writer block",
    )
    # Ensure all encoder/VI resources are released *before* breaking on send.
    clone = once(
        clone,
        "\t\tmmf_vi_frame_free(vi_ch);\n\t\tDEBUG(\"use %ld us\\r\\n\", _get_time_us() - start);",
        "\t\tmmf_vi_frame_free(vi_ch);\n"
        "\t\tif (send_failed) break;\n"
        "\t\tDEBUG(\"use %ld us\\r\\n\", _get_time_us() - start);",
        "release encoded/VI frame before handling socket disconnect",
    )
    clone = once(
        clone,
        "\tmmf_deinit();\n\treturn 0;\n}",
        "\tmmf_deinit();\n\tclose(client_fd);\n\treturn 0;\n}",
        "close socket on shutdown",
    )
    # Keep the original mode-6 capture behavior. Add visibility to VI failures.
    clone = once(
        clone,
        "uint64_t last_loop_us = start;\n\twhile (!exit_flag) {",
        "uint64_t last_loop_us = start;\n"
        "\tunsigned long long vi_failures = 0;\n"
        "\twhile (!exit_flag) {",
        "VI counter",
    )
    clone = once(
        clone,
        "\t\tif (mmf_vi_frame_pop(vi_ch, &data, &data_size, &width, &height, &format)) {\n"
        "\t\t\tcontinue;\n\t\t}",
        "\t\tif (mmf_vi_frame_pop(vi_ch, &data, &data_size, &width, &height, &format)) {\n"
        "\t\t\t++vi_failures;\n"
        "\t\t\tif (vi_failures == 1 || vi_failures % 10 == 0)\n"
        "\t\t\t\tfprintf(stderr, \"[tcp-h265] VI pop failed (%llu)\\n\", vi_failures);\n"
        "\t\t\tusleep(10000);\n"
        "\t\t\tcontinue;\n\t\t}",
        "VI diagnostics",
    )

    helper = r"""
/* NODRIX_TCP_H265_MODE14 */
static int tcp_h265_send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

"""
    # Headers are inserted only once, and mode-6 function remains untouched.
    includes = (
        "#include <sys/socket.h>\n"
        "#include <netinet/in.h>\n"
        "#include <arpa/inet.h>\n"
    )
    if "#include <sys/socket.h>" not in src:
        src = once(src, "#include <sys/time.h>\n",
                   "#include <sys/time.h>\n" + includes, "TCP headers")
        a = src.index(SOURCE_START)
        b = src.index(SOURCE_END, a)
    # Insert helper & mode 14 directly BEFORE mode 7, preserving 6 unchanged.
    src = src[:b] + helper + clone + "\n" + src[b:]
    wrapper = "\nint test_vi_venc_h265_tcp(void)\n{\n    return %s();\n}\n" % MODE_NAME
    pos = src.index("int test_vi_venc_h265(void)\n")
    src = src[:pos] + wrapper + "\n" + src[pos:]
    return src


def patch_main(src):
    if "test_vi_venc_h265_tcp" in src:
        print("Mode 14 already installed in sample_vio_main.c")
        return src
    src = once(src, '\tprintf("\\t 13) test i2c oled.\\n");',
               '\tprintf("\\t 13) test i2c oled.\\n");\n'
               '\tprintf("\\t 14) live H265 over TCP 8555 (based on mode 6).\\n");',
               "usage")
    src = once(src, "\textern int test_vi_venc_h265(void);",
               "\textern int test_vi_venc_h265(void);\n"
               "\textern int test_vi_venc_h265_tcp(void);",
               "extern function")
    src = once(src,
               "\tcase 13:\n\t\ts32Ret = test_i2c_oled();\n\t\tbreak;",
               "\tcase 13:\n\t\ts32Ret = test_i2c_oled();\n\t\tbreak;\n"
               "\tcase 14:\n\t\ts32Ret = test_vi_venc_h265_tcp();\n\t\tbreak;",
               "dispatch mode 14")
    return src


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sdk_root", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    folder = args.sdk_root / "middleware/v2/sample/test_mmf"
    vio = folder / "sample_vio.c"
    mainfile = folder / "sample_vio_main.c"
    for path in (vio, mainfile):
        if not path.is_file():
            parser.error("Missing SDK source: %s" % path)
    try:
        old_vio = vio.read_text(encoding="utf8")
        old_main = mainfile.read_text(encoding="utf8")
        new_vio = patch_vio(old_vio)
        new_main = patch_main(old_main)
        write_if_changed(vio, old_vio, new_vio, args.dry_run)
        write_if_changed(mainfile, old_main, new_main, args.dry_run)
    except (ValueError, OSError) as exc:
        print("Patch failed:", exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
