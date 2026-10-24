#pragma once
#include "audio.hpp"
#include "terminal.hpp"
namespace mora {
    struct Options {
        bool demo = false, motion = true;
        int seconds = 0;
        int64_t playId = 0;
        std::wstring report, snapshot;
        std::filesystem::path localAudio;
    };
    int runApp(const Options& options);
} // namespace mora
