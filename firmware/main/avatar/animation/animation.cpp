/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "animation.h"
#include "../utils/avatar_hal.h"

using namespace stackchan::animation;

// Keyframe::apply() 原本依赖 GetStackChan() 来驱动 avatar/motion/neon_light。
// 在 stackchan-mcp 的简化移植中，motion 和 neon_light 暂不可用，
// DanceModifier 也未包含在此移植中，因此 provide 空实现。
// 如果将来需要 animation 驱动 avatar，可以重新实现 apply()。
void Keyframe::apply()
{
    // TODO: 实现基于 avatar::Avatar 的 apply 逻辑
}

void Timeline::start()
{
    if (_keyframe_sequence.empty()) {
        _status = Status::Finished;
        return;
    }
    _current_index = 0;
    _status        = Status::Playing;
    _start_time    = avatar_millis();
    _apply_current_keyframe();
}

void Timeline::stop()
{
    _status        = Status::Idle;
    _current_index = 0;
}

void Timeline::pause()
{
    if (_status == Status::Playing) {
        _status       = Status::Paused;
        _elapsed_time = avatar_millis() - _start_time;
    }
}

void Timeline::resume()
{
    if (_status == Status::Paused) {
        _status     = Status::Playing;
        _start_time = avatar_millis() - _elapsed_time;
    }
}

void Timeline::update()
{
    if (_status != Status::Playing) {
        return;
    }

    if (_keyframe_sequence.empty()) {
        _status = Status::Finished;
        return;
    }

    uint32_t now = avatar_millis();
    if (now - _start_time >= _keyframe_sequence[_current_index].durationMs) {
        _current_index++;
        if (_current_index >= _keyframe_sequence.size()) {
            if (_loop) {
                _current_index = 0;
            } else {
                _status = Status::Finished;
                return;
            }
        }
        _start_time = now;
        _apply_current_keyframe();
    }
}

void Timeline::_apply_current_keyframe()
{
    if (_current_index < _keyframe_sequence.size()) {
        _keyframe_sequence[_current_index].apply();
    }
}
