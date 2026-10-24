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
        QueueMode queueMode = QueueMode::Sequential;
        bool guideDone = false;
        std::wstring userName;
        std::string personaId = "amu";
        int outputDeviceId = -1; // -1 follows the Windows default output device
        // Voluntary track comments. Off by default: every comment costs one model request.
        bool reactEnabled = false;
        int reactMinSeconds = 45; // gap between two comments
        int reactMaxChars = 25;   // requested reply length
        // Model context memory. 0 means "use what the endpoint reported, else the fallback".
        int contextLimitTokens = 0;
        bool autoCompress = true;
        int reviewChars = 1000; // requested length of the listen-history review
        std::vector<std::wstring> history;

        static Settings load(const std::filesystem::path& directory);
        bool save(const std::filesystem::path& directory) const;
    };
} // namespace mora
