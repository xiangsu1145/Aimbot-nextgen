#pragma once

#include <cstdint>

namespace aimbotng {
namespace tuning {

/// Start the background TCP parameter tuning server.
void startServer(int port = 18086);

/// Stop the TCP tuning server.
void stopServer();

/// Update real-time PID & aim telemetry from the control loop.
void updateTelemetry(int targetId, bool isLost, float rdx, float rdy, float dist,
                     float pidX, float pidY,
                     float propX, float propY,
                     float trimX, float trimY,
                     float derivX, float derivY,
                     float ffX, float ffY,
                     float velX, float velY,
                     float ageMs = -1.0f);

/// Check if continuous verbose logcat output is enabled via TCP.
bool isLogLiveEnabled();

}  // namespace tuning
}  // namespace aimbotng
