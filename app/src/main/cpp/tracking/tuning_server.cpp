#include "tuning_server.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "tracking/kalman_tracker.h"
#include "ui/gui/sections/aim_section.h"
#include "ui/gui/sections/settings_section.h"

#define TAG "AimbotTune"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace aimbotng {
namespace tuning {

namespace {

struct TelemetrySnapshot {
    int   targetId = -1;
    bool  isLost = false;
    float rdx = 0.0f;
    float rdy = 0.0f;
    float dist = 0.0f;
    float pidX = 0.0f;
    float pidY = 0.0f;
    float propX = 0.0f;
    float propY = 0.0f;
    float trimX = 0.0f;
    float trimY = 0.0f;
    float derivX = 0.0f;
    float derivY = 0.0f;
    float ffX = 0.0f;
    float ffY = 0.0f;
    float velX = 0.0f;
    float velY = 0.0f;
    /// Age of the detections this telemetry describes, in ms — wall clock from
    /// the frame's composition to now. Negative when unmeasurable.
    float ageMs = -1.0f;
};

std::mutex g_telemetryMutex;
TelemetrySnapshot g_telemetry;
std::atomic<bool> g_logLive{false};

std::atomic<bool> g_running{false};
pthread_t g_serverThread = 0;
int g_serverFd = -1;

struct ClientContext {
    int fd;
    bool stream;
};
std::mutex g_clientsMutex;
std::vector<ClientContext> g_clients;

std::string formatGet() {
    auto& aim = ui::sections::g_pageAim;
    auto& sett = ui::sections::g_pageSettings;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "kp=%.4f\n"
             "ki=%.4f\n"
             "kd=%.4f\n"
             "kf=%.4f\n"
             "smooth=%.4f\n"
             "deadzone=%.4f\n"
             "delay=%.4f\n"
             "terminate=%d\n"
             "predict=%d\n",
             aim.kp.value,
             aim.ki.value,
             aim.kd.value,
             aim.ffGain.value,
             aim.outSmooth.value,
             aim.deadzone.value,
             aim.aimDelayFrames.value,
             static_cast<int>(sett.trackTerminate.value + 0.5f),
             static_cast<int>(sett.trackPredictHoldFrames.value + 0.5f));
    return std::string(buf);
}

std::string formatStatus() {
    TelemetrySnapshot s;
    {
        std::lock_guard<std::mutex> lock(g_telemetryMutex);
        s = g_telemetry;
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
             "STATUS: id=%d lost=%d dist=%.2f dx=%.2f dy=%.2f | "
             "P=(%.3f,%.3f) I=(%.3f,%.3f) D=(%.3f,%.3f) FF=(%.3f,%.3f) | "
             "tot=(%.3f,%.3f) v=(%.2f,%.2f) age=%.1f\n",
             s.targetId, s.isLost ? 1 : 0, s.dist, s.rdx, s.rdy,
             s.propX, s.propY,
             s.trimX, s.trimY,
             s.derivX, s.derivY,
             s.ffX, s.ffY,
             s.pidX, s.pidY,
             s.velX, s.velY,
             s.ageMs);
    return std::string(buf);
}

std::string handleCommand(const std::string& cmdLine, bool& streamMode) {
    // Strip trailing \r / \n
    std::string line = cmdLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    while (!line.empty() && line.front() == ' ') {
        line.erase(line.begin());
    }
    if (line.empty()) return "";

    if (line == "HELP") {
        return "Commands:\n"
               "  GET\n"
               "  SET <kp|ki|kd|kf|smooth|deadzone|delay|terminate|predict> <value>\n"
               "  STATUS\n"
               "  STREAM 1|0\n"
               "  LOG 1|0\n"
               "  HELP\n";
    }

    if (line == "GET") {
        return formatGet();
    }

    if (line == "STATUS") {
        return formatStatus();
    }

    if (line.rfind("STREAM", 0) == 0) {
        int v = 0;
        if (sscanf(line.c_str(), "STREAM %d", &v) == 1) {
            streamMode = (v != 0);
            return streamMode ? "OK: STREAM enabled\n" : "OK: STREAM disabled\n";
        }
        return "ERR: Usage STREAM 1|0\n";
    }

    if (line.rfind("LOG", 0) == 0) {
        int v = 0;
        if (sscanf(line.c_str(), "LOG %d", &v) == 1) {
            g_logLive = (v != 0);
            return g_logLive ? "OK: LOG enabled\n" : "OK: LOG disabled\n";
        }
        return "ERR: Usage LOG 1|0\n";
    }

    if (line.rfind("SET", 0) == 0) {
        char key[64];
        float val = 0.0f;
        if (sscanf(line.c_str(), "SET %63s %f", key, &val) == 2) {
            auto& aim = ui::sections::g_pageAim;
            auto& sett = ui::sections::g_pageSettings;

            if (strcmp(key, "kp") == 0) {
                aim.kp.value = std::clamp(val, 0.0f, 1.0f);
            } else if (strcmp(key, "ki") == 0) {
                aim.ki.value = std::clamp(val, 0.0f, 1.0f);
            } else if (strcmp(key, "kd") == 0) {
                aim.kd.value = std::clamp(val, 0.0f, 2.0f);
            } else if (strcmp(key, "kf") == 0) {
                aim.ffGain.value = std::clamp(val, 0.0f, 1.0f);
            } else if (strcmp(key, "smooth") == 0) {
                aim.outSmooth.value = std::clamp(val, 0.0f, 1.0f);
            } else if (strcmp(key, "deadzone") == 0) {
                aim.deadzone.value = std::clamp(val, 0.0f, 1.0f);
            } else if (strcmp(key, "delay") == 0) {
                aim.aimDelayFrames.value = std::clamp(val, 0.0f, 10.0f);
            } else if (strcmp(key, "terminate") == 0) {
                sett.trackTerminate.value = std::clamp(val, 0.0f, 30.0f);
                tracking::trackerConfig().terminateFrames = static_cast<int>(sett.trackTerminate.value + 0.5f);
            } else if (strcmp(key, "predict") == 0) {
                sett.trackPredictHoldFrames.value = std::clamp(val, 0.0f, 30.0f);
                tracking::trackerConfig().predictHoldFrames = static_cast<int>(sett.trackPredictHoldFrames.value + 0.5f);
            } else {
                return "ERR: Unknown parameter " + std::string(key) + "\n";
            }
            LOGI("tuning: SET %s = %f", key, val);
            char resp[128];
            snprintf(resp, sizeof(resp), "OK: %s=%.4f\n", key, val);
            return std::string(resp);
        }
        return "ERR: Usage SET <param> <value>\n";
    }

    return "ERR: Unknown command. Type HELP for list.\n";
}

void* serverWorker(void* arg) {
    const int port = reinterpret_cast<intptr_t>(arg);
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        LOGE("tuning: socket() failed");
        return nullptr;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("tuning: bind(%d) failed", port);
        close(serverFd);
        return nullptr;
    }

    if (listen(serverFd, 5) < 0) {
        LOGE("tuning: listen failed");
        close(serverFd);
        return nullptr;
    }

    g_serverFd = serverFd;
    LOGI("tuning: TCP server listening on port %d", port);

    int streamTick = 0;
    while (g_running.load()) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(serverFd, &readFds);
        int maxFd = serverFd;

        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            for (const auto& c : g_clients) {
                FD_SET(c.fd, &readFds);
                if (c.fd > maxFd) maxFd = c.fd;
            }
        }

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  // 50ms interval

        int activity = select(maxFd + 1, &readFds, nullptr, nullptr, &tv);
        if (activity < 0) {
            if (!g_running.load()) break;
            continue;
        }

        // New client connection
        if (FD_ISSET(serverFd, &readFds)) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientFd >= 0) {
                // Non-blocking
                int flags = fcntl(clientFd, F_GETFL, 0);
                fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

                std::lock_guard<std::mutex> lock(g_clientsMutex);
                g_clients.push_back({clientFd, false});
                const char* welcome = "AimbotNextgen Tuning Server v1.0\nType HELP for commands.\n> ";
                send(clientFd, welcome, strlen(welcome), MSG_NOSIGNAL);
                LOGI("tuning: new client connected fd=%d", clientFd);
            }
        }

        // Handle existing clients
        std::vector<int> deadFds;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            for (auto& c : g_clients) {
                if (FD_ISSET(c.fd, &readFds)) {
                    char buf[512];
                    ssize_t n = recv(c.fd, buf, sizeof(buf) - 1, 0);
                    if (n <= 0) {
                        deadFds.push_back(c.fd);
                        continue;
                    }
                    buf[n] = '\0';
                    std::string resp = handleCommand(std::string(buf), c.stream);
                    if (!resp.empty()) {
                        send(c.fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
                    }
                    send(c.fd, "> ", 2, MSG_NOSIGNAL);
                }
            }

            // Stream telemetry at ~20Hz if requested
            if (++streamTick >= 1) { // every 50ms
                streamTick = 0;
                std::string statusStr;
                bool hasStreamer = false;
                for (const auto& c : g_clients) {
                    if (c.stream) { hasStreamer = true; break; }
                }
                if (hasStreamer) {
                    statusStr = formatStatus();
                    for (const auto& c : g_clients) {
                        if (c.stream) {
                            ssize_t s = send(c.fd, statusStr.c_str(), statusStr.size(), MSG_NOSIGNAL);
                            if (s < 0) deadFds.push_back(c.fd);
                        }
                    }
                }
            }

            // Remove disconnected clients
            if (!deadFds.empty()) {
                g_clients.erase(
                    std::remove_if(g_clients.begin(), g_clients.end(), [&](const ClientContext& c) {
                        for (int df : deadFds) {
                            if (c.fd == df) {
                                close(c.fd);
                                LOGI("tuning: client disconnected fd=%d", df);
                                return true;
                            }
                        }
                        return false;
                    }),
                    g_clients.end());
            }
        }
    }

    // Clean up
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        for (auto& c : g_clients) {
            close(c.fd);
        }
        g_clients.clear();
    }
    if (g_serverFd >= 0) {
        close(g_serverFd);
        g_serverFd = -1;
    }
    LOGI("tuning: server stopped");
    return nullptr;
}

}  // namespace

void startServer(int port) {
    if (g_running.load()) return;
    g_running.store(true);
    if (pthread_create(&g_serverThread, nullptr, serverWorker, reinterpret_cast<void*>(static_cast<intptr_t>(port))) != 0) {
        LOGE("tuning: pthread_create failed");
        g_running.store(false);
    }
}

void stopServer() {
    if (!g_running.load()) return;
    g_running.store(false);
    if (g_serverThread) {
        pthread_join(g_serverThread, nullptr);
        g_serverThread = 0;
    }
}

void updateTelemetry(int targetId, bool isLost, float rdx, float rdy, float dist,
                     float pidX, float pidY,
                     float propX, float propY,
                     float trimX, float trimY,
                     float derivX, float derivY,
                     float ffX, float ffY,
                     float velX, float velY,
                     float ageMs) {
    std::lock_guard<std::mutex> lock(g_telemetryMutex);
    g_telemetry.targetId = targetId;
    g_telemetry.isLost = isLost;
    g_telemetry.rdx = rdx;
    g_telemetry.rdy = rdy;
    g_telemetry.dist = dist;
    g_telemetry.pidX = pidX;
    g_telemetry.pidY = pidY;
    g_telemetry.propX = propX;
    g_telemetry.propY = propY;
    g_telemetry.trimX = trimX;
    g_telemetry.trimY = trimY;
    g_telemetry.derivX = derivX;
    g_telemetry.derivY = derivY;
    g_telemetry.ffX = ffX;
    g_telemetry.ffY = ffY;
    g_telemetry.velX = velX;
    g_telemetry.velY = velY;
    g_telemetry.ageMs = ageMs;
}

bool isLogLiveEnabled() {
    return g_logLive.load();
}

}  // namespace tuning
}  // namespace aimbotng
