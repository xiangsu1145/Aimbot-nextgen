// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::valorant_recoil — Valorant two-stage smart recoil engine (无畏契约双阶段智能压枪)
//
//  Design:
//    - Stage 1 (狂徒前5发，幻影前6发): 二阶贝塞尔曲线平滑过渡，独立调节 stage1Px
//    - Stage 2 (紧接着压3发): 三阶贝塞尔曲线衔接，消除大跳变顿挫，独立调节 stage2Px
//    - Stage 3 (之后): 垂直完全保持住，不再额外下压 (dy = 0)
//    - 回正速度可调: 手动设置回正速度 (px/s)
//    - 阶段时间根据武器射速自动精确换算 (狂徒 9.75rps, 幻影 11.0rps)
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>

namespace aimbotng {
namespace valorant_recoil {

enum class Weapon {
    Vandal = 0,   // 狂徒 (9.75 rps, 阶段1: 5发, 阶段2: 3发, 8发后保持)
    Phantom = 1,  // 幻影 (11.0 rps, 阶段1: 6发, 阶段2: 3发, 9发后保持)
    Custom = 2,   // 自定义
};

struct Config {
    Weapon weapon = Weapon::Vandal;
    float  stage1Px = 66.0f;      // 第一阶段独立下压行程 (px, 默认0.6x)
    float  stage2Px = 90.0f;      // 第二阶段独立下压行程 (px, 默认0.6x)
    int    stage1Shots = 5;       // 第一阶段发数 (狂徒默认5，幻影默认6)
    int    stage2Shots = 3;       // 第二阶段发数 (默认3发)
    float  resetMs = 300.0f;      // 停火重置窗口 (ms)
    bool   returnHome = true;     // 停火平滑回中
    float  returnSpeed = 220.0f;  // 停火回正速度 (px/s)
};

struct Status {
    bool  firing = false;
    int   currentShot = 0;
    int   phase = 0;              // 1 = 阶段一, 2 = 阶段二, 3 = 锁死保持
    float burstDurationSec = 0.0f;
    float currentY = 0.0f;
    float totalTargetY = 0.0f;
};

void setConfig(const Config& cfg);
Config currentConfig();

bool tick(float dt, bool firing, bool enabled, float& accX, float& accY, float& dx, float& dy);

Status currentStatus();
void reset();

}  // namespace valorant_recoil
}  // namespace aimbotng
