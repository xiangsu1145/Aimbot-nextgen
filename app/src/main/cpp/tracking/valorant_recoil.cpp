// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::valorant_recoil — Valorant two-stage smart recoil engine
// ─────────────────────────────────────────────────────────────────────────────
#include "tracking/valorant_recoil.h"

#include <android/log.h>
#include <cmath>
#include <algorithm>

#define LOG_TAG "AimbotValorantRecoil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace aimbotng {
namespace valorant_recoil {

static Config s_cfg;
static Status s_status;

static float s_burstTime = 0.0f;
static float s_releaseTime = 0.0f;
static float s_currentY = 0.0f;
static bool  s_wasFiring = false;

static float getShotInterval(Weapon w) {
    switch (w) {
        case Weapon::Vandal:  return 1.0f / 9.75f;   // ~0.10256s
        case Weapon::Phantom: return 1.0f / 11.0f;   // ~0.09091s
        case Weapon::Custom:  return 1.0f / 10.0f;   // ~0.10000s
    }
    return 1.0f / 10.0f;
}

static int getDefaultStage1Shots(Weapon w) {
    switch (w) {
        case Weapon::Vandal:  return 5;  // 狂徒前5发
        case Weapon::Phantom: return 6;  // 幻影前6发
        case Weapon::Custom:  return 5;
    }
    return 5;
}

void setConfig(const Config& cfg) {
    s_cfg = cfg;
    if (s_cfg.weapon == Weapon::Vandal) {
        s_cfg.stage1Shots = 5;
        s_cfg.stage2Shots = 3;
    } else if (s_cfg.weapon == Weapon::Phantom) {
        s_cfg.stage1Shots = 6;
        s_cfg.stage2Shots = 3;
    }
}

Config currentConfig() {
    return s_cfg;
}

Status currentStatus() {
    return s_status;
}

void reset() {
    s_burstTime = 0.0f;
    s_releaseTime = 0.0f;
    s_currentY = 0.0f;
    s_wasFiring = false;
    s_status = Status{};
}

bool tick(float dt, bool firing, bool enabled, float& accX, float& accY, float& dx, float& dy) {
    dx = 0.0f;
    dy = 0.0f;
    accX = 0.0f;

    if (!enabled) {
        if (s_currentY > 0.0f) {
            reset();
        }
        accY = 0.0f;
        return false;
    }

    const float shotInterval = getShotInterval(s_cfg.weapon);
    const float resetDuration = std::max(0.05f, s_cfg.resetMs / 1000.0f);
    const int s1Shots = (s_cfg.weapon == Weapon::Custom) ? std::max(2, s_cfg.stage1Shots) : getDefaultStage1Shots(s_cfg.weapon);
    const int s2Shots = std::max(1, s_cfg.stage2Shots);

    // 时间分段 (根据枪的射速严格推导)
    // 阶段1时间: 从第1发到第 s1Shots 发 (即 s1Shots - 1 个射击间隔)
    const float tP1 = std::max(0.05f, (s1Shots - 1) * shotInterval);
    // 阶段2时间: 紧接压 s2Shots 发
    const float tP2 = tP1 + s2Shots * shotInterval;

    if (firing) {
        if (!s_wasFiring) {
            if (s_releaseTime > 0.0f && s_releaseTime < resetDuration) {
                // 停火时间在重置窗口内：平滑延续当前部分后坐力
                float recovered = std::clamp(s_releaseTime / resetDuration, 0.0f, 1.0f);
                s_burstTime = s_burstTime * (1.0f - recovered);
            } else {
                s_burstTime = 0.0f;
                s_currentY = 0.0f;
            }
            s_releaseTime = 0.0f;
            s_wasFiring = true;
            LOGD("valorant_recoil: burst start weapon=%d s1Px=%.1f s2Px=%.1f",
                 static_cast<int>(s_cfg.weapon), s_cfg.stage1Px, s_cfg.stage2Px);
        }

        s_burstTime += dt;
        const int currentShot = std::clamp(static_cast<int>(s_burstTime / shotInterval) + 1, 1, 30);

        float targetY = 0.0f;
        int phase = 1;

        if (s_burstTime <= tP1) {
            // 阶段一: 二阶贝塞尔曲线 (Quadratic Bezier)
            // P0 = 0.0, P1 = 0.15, P2 = 1.0
            // B2(u) = 2*(1-u)*u*P1 + u^2
            // 保证第1发绝对精准，第2发平滑下压，阶段一结束平滑过渡
            phase = 1;
            const float u = std::clamp(s_burstTime / tP1, 0.0f, 1.0f);
            const float b2 = 2.0f * (1.0f - u) * u * 0.15f + u * u;
            targetY = s_cfg.stage1Px * b2;
        } else if (s_burstTime <= tP2) {
            // 阶段二: 三阶贝塞尔曲线 (Cubic Bezier)
            // P0 = 0.0, P1 = 0.32, P2 = 1.0, P3 = 1.0
            // B3(v) = 3*(1-v)^2 * v * P1 + 3*(1-v) * v^2 * P2 + v^3 * P3
            // P1 匹配阶段一一阶导数速度，P2=P3=1.0 使进入阶段三前速度平滑归零，杜绝顿挫与突兀跳变
            phase = 2;
            const float v = std::clamp((s_burstTime - tP1) / (tP2 - tP1), 0.0f, 1.0f);
            const float p1 = 0.32f;
            const float p2 = 1.0f;
            const float oneMinusV = 1.0f - v;
            const float b3 = 3.0f * oneMinusV * oneMinusV * v * p1
                           + 3.0f * oneMinusV * v * v * p2
                           + v * v * v;
            targetY = s_cfg.stage1Px + s_cfg.stage2Px * b3;
        } else {
            // 阶段三: 完全保持住 (垂直锁定，绝对不再继续增加下压!)
            phase = 3;
            targetY = s_cfg.stage1Px + s_cfg.stage2Px;
        }

        dy = targetY - s_currentY;
        s_currentY = targetY;
        accY = s_currentY;

        s_status.firing = true;
        s_status.currentShot = currentShot;
        s_status.phase = phase;
        s_status.burstDurationSec = s_burstTime;
        s_status.currentY = s_currentY;
        s_status.totalTargetY = s_cfg.stage1Px + s_cfg.stage2Px;

        return true;
    } else {
        // 停火状态
        if (s_wasFiring) {
            s_wasFiring = false;
            s_releaseTime = 0.0f;
        }

        s_releaseTime += dt;
        s_status.firing = false;

        if (s_releaseTime >= resetDuration) {
            s_burstTime = 0.0f;
            s_status.currentShot = 0;
            s_status.phase = 0;
        }

        // 停火平滑回中 (速度由用户设置的 returnSpeed 控制)
        if (s_cfg.returnHome && s_currentY > 0.5f) {
            const float speed = std::clamp(s_cfg.returnSpeed, 20.0f, 1200.0f);
            const float stepY = std::min(s_currentY, speed * dt);
            s_currentY -= stepY;
            dy = -stepY; // 反向拉回
            accY = s_currentY;
            s_status.currentY = s_currentY;
            return true;
        } else {
            s_currentY = 0.0f;
            accY = 0.0f;
            s_status.currentY = 0.0f;
            return false;
        }
    }
}

}  // namespace valorant_recoil
}  // namespace aimbotng
