#pragma once

#include <clap/clap.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace mote
{
inline constexpr char pluginId[] = "com.charlieculbert.mote";
enum Parameter : clap_id { rate = 0, octaves = 5, gate = 6, latch = 8, direction = 9 };
inline constexpr size_t stateValueCount = 10;
struct ParameterInfo { clap_id id; const char* name; double min, max, initial; };
inline constexpr std::array<ParameterInfo, 5> parameters {{
    { rate, "rate", 0, 7, 1 }, { octaves, "octaves", 1, 4, 1 }, { gate, "gate", 5, 95, 65 },
    { latch, "hold", 0, 1, 0 }, { direction, "order", 0, 2, 0 }
}};
using Values = std::array<double, stateValueCount>;
inline const ParameterInfo* findParameter(clap_id id) noexcept
{
    for (const auto& p : parameters) if (p.id == id) return &p;
    return nullptr;
}
inline constexpr std::array<double, 8> beatDivisions { 1, .5, 1.0 / 3, .25, 1.0 / 6, .125, 1.0 / 12, .0625 };
inline constexpr std::array<const char*, 8> divisionNames { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64" };
inline constexpr std::array<const char*, 3> directionNames { "Up", "Down", "Up/down" };
inline double clampParameter(clap_id id, double value) noexcept
{
    const auto* p = findParameter(id);
    if (!p) return 0;
    return std::isfinite(value) ? std::round(std::clamp(value, p->min, p->max)) : p->initial;
}
const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);
} // namespace mote
