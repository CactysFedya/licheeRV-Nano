// Independent, minimal color MJPEG server for SG2002 / LicheeRV Nano.
// Uses opencv-mobile's CVITEK CSI VideoCapture backend.
// License: MIT (this file).
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t interrupted = 0;
static void on_signal(int) { interrupted = 1; }

struct Options {
    int port = 7778;            // Avoid conflict with existing CSIStream (:7777).
    int width = 640;
    int height = 480;
    int quality = 70;           // Faster than original quality=90 in many conditions.
    int capture_fps = 12;       // Soft cap; not a guaranteed achieved FPS.
    int stream_fps = 12;
    int duration = 20;          // Safe finite first run; 0 means until Ctrl+C.
    int thermal_limit = 0;     // Disabled; optional thermal cutoff in degrees C.
};

static bool parse_int(const char* arg, int& result) {
    if (!arg || !*arg) return false;
    char* end = nullptr;
    errno = 0;
    long n = std::strtol(arg, &end, 10);
    if (errno || end == arg || *end != '\0' || n < -1 || n > 65535) return false;
    result = static_cast<int>(n);
    return true;
}

static void usage(const char* name) {
    std::cout << "Usage: " << name << " [options]\n"
              << "  --port N           HTTP port (default 7778)\n"
              << "  --width N          Output width (default 640)\n"
              << "  --height N         Output height (default 480)\n"
              << "  --quality N        JPEG quality 1..100 (default 70)\n"
              << "  --capture-fps N    Capture rate cap (default 12)\n"
              << "  --stream-fps N     Stream rate cap (default 12)\n"
              << "  --duration N       Stop after N seconds; 0=unlimited (default 20)\n"
              << "  --thermal-limit N  Stop at N C SoC temperature; 0=off\n";
}

static bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string key(argv[i]);
        if (key == "--help" || key == "-h") { usage(argv[0]); std::exit(0); }
        if (i + 1 >= argc) return false;
        int value = 0;
        if (!parse_int(argv[++i], value)) return false;
        if (key == "--port") o.port = value;
        else if (key == "--width") o.width = value;
        else if (key == "--height") o.height = value;
        else if (key == "--quality") o.quality = value;
        else if (key == "--capture-fps") o.capture_fps = value;
        else if (key == "--stream-fps") o.stream_fps = value;
        else if (key == "--duration") o.duration = value;
        else if (key == "--thermal-limit") o.thermal_limit = value;
        else return false;
    }
    return o.port > 0 && o.width > 0 && o.width <= 4096 &&
           o.height > 0 && o.height <= 4096 && o.quality >= 1 && o.quality <= 100 &&
           o.capture_fps >= 1 && o.capture_fps <= 60 &&
           o.stream_fps >= 1 && o.stream_fps <= 60 &&
           o.duration >= 0 && o.thermal_limit >= 0;
}

static int read_soc_temp_millideg() {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    int t = -1;
    if (f >> t) return t;
    return -1;
}

static bool send_all(int fd, const void* bytes, size_t length) {
    const char* p = static_cast<const char*>(bytes);
    while (length && !interrupted) {
        const ssize_t n = ::send(fd, p, length, MSG_NOSIGNAL);
        if (n > 0) { p += n; length -= static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return length == 0;
}
static bool send_text(int fd, const std::string& s) { return send_all(fd, s.data(), s.size()); }

struct Shared {
    std::mutex m;
    cv::Mat newest;
    uint64_t seq = 0;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> grabbed{0};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> failed{0};
};

static bool expired(const Options& o, Clock::time_point start) {
    if (interrupted) return true;
    if (o.duration == 0) return false;
    return Clock::now() - start >= std::chrono::seconds(o.duration);
}

static void capture_loop(cv::VideoCapture& camera, Shared& shared,
                         const Options& o, Clock::time_point start) {
    const auto period = std::chrono::microseconds(1000000 / o.capture_fps);
    auto t_print = Clock::now();
    auto t_heat = Clock::now();
    uint64_t count_print = 0;
    double capture_ms_sum = 0;
    while (!shared.stop && !expired(o, start)) {
        const auto t_begin = Clock::now();
        cv::Mat frame;
        bool ok = false;
        try { camera >> frame; ok = !frame.empty(); }
        catch (const cv::Exception& e) { std::cerr << "Capture exception: " << e.what() << '\n'; }
        const auto t_captured = Clock::now();
        if (ok && !frame.empty()) {
            if (frame.channels() != 3) {
                ++shared.failed;
                std::cerr << "Expected BGR 3 channels; got " << frame.channels()
                          << ". Refusing to fake color.\n";
                shared.stop = true;
                break;
            }
            try {
                if (frame.cols != o.width || frame.rows != o.height) {
                    cv::Mat scaled;
                    cv::resize(frame, scaled, cv::Size(o.width, o.height), 0, 0, cv::INTER_AREA);
                    frame = scaled;
                }
                {
                    std::lock_guard<std::mutex> lock(shared.m);
                    shared.newest = frame.clone();
                    ++shared.seq;
                }
                ++shared.grabbed;
                ++count_print;
                capture_ms_sum += std::chrono::duration<double, std::milli>(t_captured - t_begin).count();
            } catch (const cv::Exception& e) {
                ++shared.failed;
                std::cerr << "Frame processing exception: " << e.what() << '\n';
            }
        } else {
            ++shared.failed;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto now = Clock::now();
        if (now - t_print >= std::chrono::seconds(5)) {
            const double seconds = std::chrono::duration<double>(now - t_print).count();
            std::cout << "[CAPTURE] fps=" << count_print / seconds
                      << " avg_read_ms=" << (count_print ? capture_ms_sum / count_print : 0)
                      << " delivered=" << shared.grabbed.load()
                      << " errors=" << shared.failed.load() << std::endl;
            count_print = 0;
            capture_ms_sum = 0;
            t_print = now;
        }
        if (now - t_heat >= std::chrono::seconds(2)) {
            int t = read_soc_temp_millideg();
            if (t >= 0) {
                std::cout << "[TEMP] " << t / 1000.0 << " C" << std::endl;
                if (o.thermal_limit && t >= 1000 * o.thermal_limit) {
                    std::cerr << "Thermal cutoff reached; stopping stream.\n";
                    shared.stop = true;
                }
            }
            t_heat = now;
        }
        const auto used = Clock::now() - t_begin;
        if (used < period) std::this_thread::sleep_for(period - used);
    }
    shared.stop = true;
}

static void serve_client(int fd, Shared& shared, const Options& o,
                         Clock::time_point start) {
    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char request[1024]{};
    const ssize_t got = recv(fd, request, sizeof(request)-1, 0);
    if (got <= 0) return;
    const std::string req(request, static_cast<size_t>(got));
    if (req.find("GET /status") == 0) {
        std::ostringstream json;
        json << "{\"capture_frames\":" << shared.grabbed.load()
             << ",\"stream_frames\":" << shared.sent.load()
             << ",\"capture_errors\":" << shared.failed.load() << "}\n";
        const auto body = json.str();
        std::ostringstream headers;
        headers << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n";
        send_text(fd, headers.str());
        send_text(fd, body);
        return;
    }
    if (req.find("GET / ") != 0) {
        send_text(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    if (!send_text(fd, "HTTP/1.1 200 OK\r\n"
                    "Cache-Control: no-cache, no-store\r\n"
                    "Pragma: no-cache\r\nConnection: close\r\n"
                    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n")) return;
    const auto period = std::chrono::microseconds(1000000 / o.stream_fps);
    uint64_t last_seq = 0;
    uint64_t count_print = 0;
    double jpeg_ms_sum = 0;
    auto t_print = Clock::now();
    const std::vector<int> jpeg_options{cv::IMWRITE_JPEG_QUALITY, o.quality};
    while (!shared.stop && !expired(o, start)) {
        const auto t_begin = Clock::now();
        cv::Mat image;
        {
            std::lock_guard<std::mutex> lock(shared.m);
            if (shared.seq != last_seq && !shared.newest.empty()) {
                last_seq = shared.seq;
                image = shared.newest.clone();
            }
        }
        if (image.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        std::vector<unsigned char> jpeg;
        bool ok = false;
        try { ok = cv::imencode(".jpg", image, jpeg, jpeg_options); }
        catch (const cv::Exception& e) { std::cerr << "JPEG error: " << e.what() << '\n'; }
        const auto t_encoded = Clock::now();
        if (!ok || jpeg.empty()) { ++shared.failed; continue; }
        std::ostringstream h;
        h << "--frame\r\nContent-Type: image/jpeg\r\n"
          << "Content-Length: " << jpeg.size() << "\r\n\r\n";
        if (!send_text(fd, h.str()) || !send_all(fd, jpeg.data(), jpeg.size()) || !send_text(fd, "\r\n")) break;
        ++shared.sent;
        ++count_print;
        jpeg_ms_sum += std::chrono::duration<double, std::milli>(t_encoded - t_begin).count();
        const auto now = Clock::now();
        if (now - t_print >= std::chrono::seconds(5)) {
            const double seconds = std::chrono::duration<double>(now - t_print).count();
            std::cout << "[STREAM] fps=" << count_print / seconds
                      << " avg_jpeg_ms=" << (count_print ? jpeg_ms_sum/count_print : 0)
                      << " last_jpeg_bytes=" << jpeg.size() << std::endl;
            count_print = 0;
            jpeg_ms_sum = 0;
            t_print = now;
        }
        const auto used = Clock::now() - t_begin;
        if (used < period) std::this_thread::sleep_for(period - used);
    }
    std::cout << "[CLIENT] disconnected; delivered=" << shared.sent.load() << std::endl;
}

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) { usage(argv[0]); return 2; }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    std::cout << "CSI color MJPEG v1; " << o.width << 'x' << o.height
              << " quality=" << o.quality << " capture_limit=" << o.capture_fps
              << " stream_limit=" << o.stream_fps << " port=" << o.port
              << " duration=" << o.duration << " seconds\n";

    cv::VideoCapture cap;
    if (!cap.open(0)) {
        std::cerr << "ERROR: Cannot open CSI camera; ensure no other CSIStream/sensor_test is running.\n";
        return 1;
    }
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { std::perror("socket"); cap.release(); return 1; }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(o.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sock, 2) != 0) {
        std::perror("bind/listen"); close(sock); cap.release(); return 1;
    }
    Shared shared;
    const auto start = Clock::now();
    std::thread producer(capture_loop, std::ref(cap), std::ref(shared), std::cref(o), start);
    std::cout << "[HTTP] open http://<board-ip>:" << o.port << "/ (one client at a time)\n";

    while (!shared.stop && !expired(o, start)) {
        fd_set rfds;
        FD_ZERO(&rfds); FD_SET(sock, &rfds);
        timeval tv{0, 200000};
        const int ready = select(sock+1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        int fd = accept(sock, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (fd < 0) continue;
        std::cout << "[CLIENT] " << inet_ntoa(peer.sin_addr) << std::endl;
        serve_client(fd, shared, o, start);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    shared.stop = true;
    close(sock);
    producer.join();
    cap.release();
    std::cout << "[DONE] capture=" << shared.grabbed.load() << " streamed="
              << shared.sent.load() << " errors=" << shared.failed.load() << '\n';
    return 0;
}
