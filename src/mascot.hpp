#pragma once
#include "core.hpp"
namespace mora {
    inline constexpr int mascotWidth = 48, mascotHeight = 50;
    std::optional<RGB> mascotPixel(int x, int y, bool blink);
} // namespace mora
