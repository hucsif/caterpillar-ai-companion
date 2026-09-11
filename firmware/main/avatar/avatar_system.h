/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * StackChan Avatar System — 汇总头文件
 *
 * 使用方法：
 *   #include "avatar/avatar_system.h"
 *
 * 这会引入：
 *   1. 核心框架：Avatar, Element, Feature, Emotion, Decorator 等基类
 *   2. Default 皮肤：DefaultAvatar, DefaultEyes, DefaultMouth, DefaultSpeechBubble
 *   3. 装饰器：Heart, Angry, Sweat, Shy, Dizzy
 *   4. 修饰器计时工具：avatar_millis()
 */
#pragma once

// 核心框架
#include "framework/avatar.h"
#include "framework/element.h"
#include "framework/emotion.h"
#include "framework/feature.h"
#include "framework/key_elements.h"
#include "framework/speech_bubble.h"
#include "framework/decorator.h"

// Default 皮肤
#include "skins/default.h"

// 装饰器
#include "decorators/decorators.h"

// 修饰器
#include "modifiers/modifiers.h"

// 工具
#include "utils/avatar_hal.h"
#include "utils/object_pool.h"
#include "utils/random.h"

// 简化的 Modifiable 接口（已通过 modifiers/modifiers.h → ../modifiable.h 引入）
// #include "avatar_modifiable.h"
