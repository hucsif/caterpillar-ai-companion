/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include "../utils/avatar_hal.h"
#include <cstdint>

namespace stackchan {

/**
 * @brief 说话修饰器 — 控制嘴巴开合模拟说话
 *
 * 注意：此版本已移除 motion（舵机）相关功能，
 *       因为简化的 Modifiable 接口不包含 motion()。
 *       如需头部微动功能，请在实现完整 Modifiable 的板级代码中添加。
 */
class SpeakingModifier : public Modifier {
public:
    /**
     * @param destroyAfterMs 持续说话时间（0 为永久，直到手动移除）
     * @param mouthIntervalMs 嘴巴开合频率（默认 180ms）
     */
    SpeakingModifier(uint32_t destroyAfterMs = 0, uint32_t mouthIntervalMs = 180)
        : _mouth_interval_ms(mouthIntervalMs)
    {
        uint32_t now = avatar_millis();

        // 销毁计时
        if (destroyAfterMs > 0) {
            _destroy_at   = now + destroyAfterMs;
            _has_lifetime = true;
        }

        // 嘴巴计时
        _next_mouth_tick = now + _mouth_interval_ms;
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar()) {
            return;
        }

        uint32_t now = avatar_millis();

        // 检查销毁逻辑
        if (_has_lifetime && now >= _destroy_at) {
            stackchan.avatar().mouth().setWeight(0);  // 闭嘴
            requestDestroy();
            return;
        }

        // 嘴巴开合动画
        if (now >= _next_mouth_tick) {
            _next_mouth_tick = now + _mouth_interval_ms;
            animate_mouth(stackchan.avatar());
        }
    }

private:
    void animate_mouth(avatar::Avatar& avatar)
    {
        _is_mouth_open = !_is_mouth_open;
        auto& random   = Random::getInstance();

        int weight = _is_mouth_open ? random.getInt(_open_min_weight, _open_max_weight)
                                    : random.getInt(_close_min_weight, _close_max_weight);

        avatar.mouth().setWeight(weight);
    }

    // 配置常量
    const int _open_min_weight  = 40;
    const int _open_max_weight  = 80;
    const int _close_min_weight = 0;
    const int _close_max_weight = 20;

    // 计时状态
    uint32_t _destroy_at      = 0;
    uint32_t _next_mouth_tick = 0;
    uint32_t _mouth_interval_ms;

    bool _has_lifetime  = false;
    bool _is_mouth_open = false;
};

}  // namespace stackchan
