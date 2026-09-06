#pragma once
#include "Plugin.h"

namespace mote
{
struct Preset { const char* key; const char* name; Values values; };
inline constexpr std::array presets {
    Preset { "up", "Up", { 1, 0, 0, 0, 0, 1, 65, 0, 0, 0 } },
    Preset { "down", "Down", { 1, 0, 0, 0, 0, 1, 65, 0, 0, 1 } },
    Preset { "up-down", "Up/down", { 1, 0, 0, 0, 0, 1, 65, 0, 0, 2 } }
};
} // namespace mote
