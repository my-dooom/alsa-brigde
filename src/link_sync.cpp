#include "link_sync.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Log only in debug builds (NDEBUG not defined).
#ifndef NDEBUG
#  define LINK_LOG(msg) do { std::cerr << msg; } while (0)
#else
#  define LINK_LOG(msg) do {} while (0)
#endif

namespace {

std::atomic<float> g_bpm{0.0f};
std::atomic<bool>  g_connected{false};
std::atomic<bool>  g_stop{false};
std::thread        g_thread;
pid_t              g_carabiner_pid{-1};

// ---------------------------------------------------------------------------
// Carabiner auto-launch
// ---------------------------------------------------------------------------

// Resolve the path to the Carabiner binary.
// Priority:
//   1. CARABINER_BIN env variable
//   2. bin/Carabiner  relative to the directory of the running executable
//   3. Return empty string → caller falls back to assuming external Carabiner
static std::string find_carabiner_bin()
{
    // 1. Environment override
    const char* env = std::getenv("CARABINER_BIN");
    if (env && env[0] != '\0') return env;

    // 2. Next to the executable in bin/Carabiner, or one level up (repo root)
    char exe_path[PATH_MAX]{};
    if (::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) > 0) {
        std::string dir(exe_path);
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            dir.resize(slash);
            for (const std::string& candidate : {
                     dir + "/bin/Carabiner",   // build/bin/Carabiner
                     dir + "/../bin/Carabiner",    // repo root / bin / Carabiner
                 }) {
                struct stat st{};
                if (::stat(candidate.c_str(), &st) == 0 && (st.st_mode & S_IXUSR)) {
                    return candidate;
                }
            }
        }
    }
    return {};
}

// Fork and exec Carabiner. Returns the child PID, or -1 on failure.
static pid_t launch_carabiner(const std::string& bin_path, int port)
{
    std::string port_str = std::to_string(port);
    pid_t pid = ::fork();
    if (pid < 0) {
        LINK_LOG("[link_sync] fork failed: " << std::strerror(errno) << "\n");
        return -1;
    }
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null to keep audio_bridge output clean
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        const char* argv[] = {bin_path.c_str(), "--port", port_str.c_str(), nullptr};
        ::execv(bin_path.c_str(), const_cast<char* const*>(argv));
        // exec only returns on error
        std::_Exit(1);
    }
    return pid;
}

static void stop_carabiner()
{
    if (g_carabiner_pid <= 0) return;
    ::kill(g_carabiner_pid, SIGTERM);
    // Give it up to 2 s to exit cleanly, then force kill.
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        if (::waitpid(g_carabiner_pid, &status, WNOHANG) == g_carabiner_pid) {
            g_carabiner_pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(g_carabiner_pid, SIGKILL);
    ::waitpid(g_carabiner_pid, nullptr, 0);
    g_carabiner_pid = -1;
}

static int connect_to_carabiner(int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Scan a single Carabiner status line for ":bpm <float>".
// Returns 0.0f when not found or value is out of the Link-supported range.
static float parse_bpm(const char* line)
{
    const char* p = std::strstr(line, ":bpm ");
    if (!p) return 0.0f;
    p += 5;
    char* end = nullptr;
    float bpm = std::strtof(p, &end);
    if (end == p || bpm < 20.0f || bpm > 999.0f) return 0.0f;
    return bpm;
}

static void sync_thread(int port)
{
    constexpr int kMaxBackoffMs = 8000;
    int backoff_ms = 500;

    while (!g_stop.load(std::memory_order_relaxed)) {
        int fd = connect_to_carabiner(port);
        if (fd < 0) {
            for (int elapsed = 0;
                 elapsed < backoff_ms && !g_stop.load(std::memory_order_relaxed);
                 elapsed += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
            continue;
        }

        backoff_ms = 500;
        g_connected.store(true, std::memory_order_relaxed);
        LINK_LOG("[link_sync] connected to Carabiner on port " << port << "\n");

        // Request an immediate status update; Carabiner will also push
        // unsolicited updates whenever BPM or peer count changes.
        const char* init = "status\n";
        ::send(fd, init, std::strlen(init), 0);

        char buf[512];
        int  buf_len = 0;

        while (!g_stop.load(std::memory_order_relaxed)) {
            int n = static_cast<int>(
                ::recv(fd, buf + buf_len,
                       static_cast<size_t>(static_cast<int>(sizeof(buf)) - buf_len - 1), 0));
            if (n <= 0) break;
            buf_len += n;

            // Process every complete newline-terminated message.
            char* start = buf;
            char* nl;
            while ((nl = static_cast<char*>(
                        std::memchr(start, '\n',
                                    static_cast<size_t>(buf + buf_len - start)))) != nullptr) {
                *nl = '\0';
                float bpm = parse_bpm(start);
                if (bpm > 0.0f) {
                    g_bpm.store(bpm, std::memory_order_relaxed);
                }
                start = nl + 1;
            }

            // Shift any partial line back to the front of the buffer.
            int remaining = static_cast<int>(buf + buf_len - start);
            if (remaining > 0 && start != buf) {
                std::memmove(buf, start, static_cast<size_t>(remaining));
            }
            buf_len = remaining;

            // If a full buffer arrived with no newline, discard it to avoid
            // an infinite stall on malformed input.
            if (buf_len >= static_cast<int>(sizeof(buf)) - 1) {
                buf_len = 0;
            }
        }

        ::close(fd);
        g_connected.store(false, std::memory_order_relaxed);
        g_bpm.store(0.0f, std::memory_order_relaxed);
        LINK_LOG("[link_sync] disconnected from Carabiner, retrying...\n");
    }
}

} // namespace

void link_sync_start(int carabiner_port)
{
    // Attempt to auto-launch Carabiner if a binary is found.
    const std::string bin = find_carabiner_bin();
    if (!bin.empty()) {
        LINK_LOG("[link_sync] launching Carabiner: " << bin << "\n");
        g_carabiner_pid = launch_carabiner(bin, carabiner_port);
        if (g_carabiner_pid > 0) {
            // Give Carabiner ~400 ms to bind its TCP port before we connect.
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    } else {
        LINK_LOG("[link_sync] no Carabiner binary found, assuming external instance\n");
    }

    g_stop.store(false, std::memory_order_relaxed);
    g_thread = std::thread(sync_thread, carabiner_port);
}

void link_sync_stop()
{
    g_stop.store(true, std::memory_order_relaxed);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    stop_carabiner();
}

float link_sync_get_bpm()
{
    return g_bpm.load(std::memory_order_relaxed);
}

bool link_sync_connected()
{
    return g_connected.load(std::memory_order_relaxed);
}
