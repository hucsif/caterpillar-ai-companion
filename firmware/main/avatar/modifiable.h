/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "framework/avatar.h"
#include "utils/object_pool.h"

namespace stackchan {

/**
 * @brief 简化的 Modifiable 基类，仅暴露 avatar/表情接口
 *
 * 相比 StackChan 原始版本，此版本去除了：
 *   - motion::Motion& motion()    （舵机运动）
 *   - addon::NeonLight 左右霓虹灯
 *
 * 修饰器（Modifier）通过此接口访问 avatar 系统。
 */
class Modifiable {
public:
    virtual ~Modifiable() = default;

    virtual avatar::Avatar& avatar() = 0;

    virtual bool hasAvatar() = 0;
};

/**
 * @brief Modifier 基类
 *
 */
class Modifier : public Poolable {
public:
    virtual void _update(Modifiable& stackchan)
    {
    }
};

}  // namespace stackchan
