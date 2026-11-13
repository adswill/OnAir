// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The recipes that come with OnAir: ready rules and scripts, every one editable. Disabled until the user switches them on.
#pragma once
#include "dect2/automation/rules.h"
#include <vector>

namespace dect2::automation {

std::vector<Recipe> builtinRecipes();

}  // namespace dect2::automation
