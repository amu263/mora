#pragma once
#include "platform.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mora {
    struct OutputDevice {
        int id = -1; // -1 follows the Windows default output device.
        std::wstring name;
    };
    std::vector<OutputDevice> outputDevices();
    struct AudioState {
        bool playing = false, paused = false, finished = false;
        double position = 0, duration = 0, volume = .65;
        int sampleRate = 0, sampleBits = 0;
        std::array<double, 24> spectrum{};
        std::wstring error;
    };
    class Audio {
        enum class Op { Play, Stop, Pause, PauseSet, Seek, Volume, OutputDevice, Exit };
        struct Command {
            Op op;
            std::wstring url;
            double value = 0;
        };
        mutable std::mutex mutex;
        std::condition_variable signal;
        std::deque<Command> commands;
        AudioState snapshot;
        int selectedOutput = -1;
        std::thread worker;
        void send(Command c);
        void run();

      public:
        Audio();
        ~Audio();
        Audio(const Audio&) = delete;
        void play(std::wstring url);
        void stop();
        void pause();
        void setPaused(bool value);
        void seek(double seconds);
        void volume(double value);
        // Valid IDs are returned by outputDevices(); -1 tracks the Windows default.
        bool selectOutputDevice(int id);
        int outputDevice() const;
        AudioState state() const;
    };
} // namespace mora
