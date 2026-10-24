#pragma once
#include "core.hpp"

namespace mora {
    // Small user-scoped state that survives a restart: volume, requested quality, repeat, motion
    // and the last commands. It never contains cookies, stream URLs or model keys, and invalid or
    // corrupt values fall back to defaults instead of breaking the player.
    struct Settings {
        double volume = .65;
        std::string quality = "lossless";
        bool repeat = false, motion = true;
        bool guideDone = false;
        std::wstring userName;
        std::string personaId = "amu";
        int outputDeviceId = -1; // -1 follows the Windows default output device
        std::vector<std::wstring> history;

        static Settings load(const std::filesystem::path& directory);
        bool save(const std::filesystem::path& directory) const;
    };
} // namespace mora
