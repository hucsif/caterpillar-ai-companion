/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <esp_timer.h>

namespace stackchan {

/**
 * @brief 替换 GetHAL().millis() 的轻量时间工具
 *        用于 avatar 修饰器和装饰器的定时
 */
inline uint32_t avatar_millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

}  // namespace stackchan
