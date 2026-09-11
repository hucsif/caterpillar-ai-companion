/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "element.h"
#include "../utils/object_pool.h"

namespace stackchan::avatar {

/**
 * @brief Decorator base class
 *
 */
class Decorator : public Poolable, public Element {
public:
};

}  // namespace stackchan::avatar
