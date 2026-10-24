#include "mascot.hpp"
#include "../assets/amu/amu_sprite.hpp"

namespace mora {
    static_assert(mascotWidth == amu_asset::width && mascotHeight == amu_asset::height);
    std::optional<RGB> mascotPixel(int x, int y, bool blink) {
        // The reference portrait is intentionally static. Do not fake facial animation.
        (void)blink;
        if (x < 0 || x >= mascotWidth || y < 0 || y >= mascotHeight)
            return {};
        auto index = amu_asset::pixels[y * mascotWidth + x];
        if (index == 255)
            return {};
        const auto& color = amu_asset::palette[index];
        return RGB{color[0], color[1], color[2]};
    }
} // namespace mora
