#include "audio.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#include <numeric>
#include <propvarutil.h>

namespace mora {
    std::vector<OutputDevice> outputDevices() {
        std::vector<OutputDevice> devices{{-1, L"Windows 默认输出"}};
        UINT count = waveOutGetNumDevs();
        for (UINT id = 0; id < count; ++id) {
            WAVEOUTCAPSW caps{};
            if (waveOutGetDevCapsW(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
                devices.push_back({static_cast<int>(id), caps.szPname});
        }
        return devices;
    }
    Audio::Audio() : worker([this] { run(); }) {}
    Audio::~Audio() {
        send({Op::Exit});
        worker.join();
    }
    void Audio::send(Command c) {
        {
            std::lock_guard lock(mutex);
            if (c.op == Op::Play || c.op == Op::Stop || c.op == Op::Exit)
                commands.clear();
            if (commands.size() < 16)
                commands.push_back(std::move(c));
        }
        signal.notify_one();
    }
    void Audio::play(std::wstring url) {
        send({Op::Play, std::move(url)});
    }
    void Audio::stop() {
        send({Op::Stop});
    }
    void Audio::pause() {
        send({Op::Pause});
    }
    void Audio::setPaused(bool value) {
        send({Op::PauseSet, {}, value ? 1.0 : 0.0});
    }
    void Audio::seek(double seconds) {
        send({Op::Seek, {}, seconds});
    }
    void Audio::volume(double value) {
        send({Op::Volume, {}, std::clamp(value, 0.0, 1.0)});
    }
    bool Audio::selectOutputDevice(int id) {
        auto devices = outputDevices();
        if (std::none_of(devices.begin(), devices.end(), [=](const OutputDevice& d) {
                return d.id == id;
            }))
            return false;
        {
            std::lock_guard lock(mutex);
            selectedOutput = id;
        }
        send({Op::OutputDevice, {}, static_cast<double>(id)});
        return true;
    }
    int Audio::outputDevice() const {
        std::lock_guard lock(mutex);
        return selectedOutput;
    }
    AudioState Audio::state() const {
        std::lock_guard lock(mutex);
        return snapshot;
    }
    void Audio::run() {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        Com<IMFSourceReader> reader;
        HWAVEOUT output = nullptr;
        struct Slot {
            WAVEHDR header{};
            Bytes data;
            bool prepared = false;
        };
        std::array<Slot, 4> slots;
        bool active = false, paused = false, eof = false, quit = false;
        double base = 0, duration = 0, volume = .65;
        UINT rate = 44100, channels = 2, bits = 16, validBits = 16;
        int outputId = -1;
        Bytes outputFormat;
        std::array<double, 24> spectrum{};
        auto cleanup = [&] {
            if (output) {
                waveOutReset(output);
                for (auto& s : slots) {
                    if (s.prepared)
                        waveOutUnprepareHeader(output, &s.header, sizeof(WAVEHDR));
                    s.prepared = false;
                    s.header = {};
                }
                waveOutClose(output);
                output = nullptr;
            }
            reader.put();
            outputFormat.clear();
            active = paused = eof = false;
        };
        auto setVolume = [&] {
            if (output) {
                DWORD v = (DWORD)(volume * 65535);
                waveOutSetVolume(output, v | (v << 16));
            }
        };
        auto fail = [&](std::wstring error) {
            cleanup();
            std::lock_guard lock(mutex);
            snapshot.playing = false;
            snapshot.error = std::move(error);
        };
        if (FAILED(com) || FAILED(mf)) {
            fail(L"Windows Media Foundation 不可用，请安装系统媒体功能包");
        }
        while (!quit) {
            std::deque<Command> pending;
            {
                std::unique_lock lock(mutex);
                if (commands.empty() && !active)
                    signal.wait_for(lock, std::chrono::milliseconds(80));
                pending.swap(commands);
            }
            for (auto& cmd : pending) {
                try {
                    switch (cmd.op) {
                    case Op::Exit:
                        quit = true;
                        break;
                    case Op::Stop:
                        cleanup();
                        {
                            std::lock_guard lock(mutex);
                            snapshot.playing = false;
                            snapshot.finished = false;
                            snapshot.position = 0;
                            snapshot.spectrum = {};
                        }
                        break;
                    case Op::Play: {
                        cleanup();
                        {
                            std::lock_guard lock(mutex);
                            outputId = selectedOutput;
                            snapshot = AudioState{};
                            snapshot.volume = volume;
                        }
                        Com<IMFAttributes> attrs;
                        hrcheck(MFCreateAttributes(attrs.put(), 3), "Decoder attributes");
                        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
                        hrcheck(
                            MFCreateSourceReaderFromURL(cmd.url.c_str(), attrs.get(), reader.put()),
                            "打开音频失败");
                        hrcheck(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE),
                                "Disable streams");
                        hrcheck(
                            reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE),
                            "Select audio");
                        Com<IMFMediaType> type;
                        MFCreateMediaType(type.put());
                        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                        hrcheck(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                            nullptr, type.get()),
                                "系统不支持该音频格式");
                        Com<IMFMediaType> current;
                        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                    current.put());
                        WAVEFORMATEX* format = nullptr;
                        UINT bytes = 0;
                        hrcheck(
                            MFCreateWaveFormatExFromMFMediaType(current.get(), &format, &bytes,
                                                                MFWaveFormatExConvertFlag_Normal),
                            "PCM format");
                        rate = format->nSamplesPerSec;
                        channels = format->nChannels;
                        bits = format->wBitsPerSample;
                        validBits = bits;
                        current->GetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, &validBits);
                        if (bits != 16 && bits != 24 && bits != 32) {
                            CoTaskMemFree(format);
                            throw std::runtime_error("系统返回了不支持的 PCM 位深");
                        }
                        outputFormat.assign(reinterpret_cast<BYTE*>(format),
                                            reinterpret_cast<BYTE*>(format) + bytes);
                        auto result = waveOutOpen(&output, outputId < 0 ? WAVE_MAPPER : outputId,
                                                  format, 0, 0, CALLBACK_NULL);
                        CoTaskMemFree(format);
                        if (result != MMSYSERR_NOERROR)
                            throw std::runtime_error("音频输出设备不可用");
                        setVolume();
                        PROPVARIANT value;
                        PropVariantInit(&value);
                        duration = 0;
                        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                                       MF_PD_DURATION, &value)) &&
                            value.vt == VT_UI8)
                            duration = value.uhVal.QuadPart / 1e7;
                        PropVariantClear(&value);
                        base = 0;
                        active = true;
                        eof = false;
                        paused = false;
                        break;
                    }
                    case Op::Pause:
                        if (output) {
                            paused = !paused;
                            if (paused)
                                waveOutPause(output);
                            else
                                waveOutRestart(output);
                        }
                        break;
                    case Op::PauseSet:
                        if (output) {
                            paused = cmd.value > 0;
                            if (paused)
                                waveOutPause(output);
                            else
                                waveOutRestart(output);
                        }
                        break;
                case Op::Volume:
                    volume = cmd.value;
                    setVolume();
                    {
                        std::lock_guard lock(mutex);
                        snapshot.volume = volume;
                    }
                    break;
                    case Op::Seek:
                        if (reader && output) {
                            waveOutReset(output);
                            for (auto& s : slots) {
                                if (s.prepared)
                                    waveOutUnprepareHeader(output, &s.header, sizeof(WAVEHDR));
                                s.prepared = false;
                                s.header = {};
                            }
                            reader->Flush(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
                            PROPVARIANT value;
                            PropVariantInit(&value);
                            value.vt = VT_I8;
                            value.hVal.QuadPart =
                                (LONGLONG)(std::clamp(cmd.value, 0.0,
                                                      std::max(0.0, duration - .05)) *
                                           1e7);
                            hrcheck(reader->SetCurrentPosition(GUID_NULL, value),
                                    "当前音频不支持跳转");
                            base = value.hVal.QuadPart / 1e7;
                            eof = false;
                            if (paused)
                                waveOutPause(output);
                        }
                        break;
                    case Op::OutputDevice:
                        outputId = static_cast<int>(cmd.value);
                        if (output && reader && !outputFormat.empty()) {
                            // Reopen at the last reported position so device selection takes
                            // effect during playback without restarting the track.
                            double resume = 0;
                            {
                                std::lock_guard lock(mutex);
                                resume = snapshot.position;
                            }
                            waveOutReset(output);
                            for (auto& s : slots) {
                                if (s.prepared)
                                    waveOutUnprepareHeader(output, &s.header, sizeof(WAVEHDR));
                                s.prepared = false;
                                s.header = {};
                            }
                            waveOutClose(output);
                            output = nullptr;
                            auto result = waveOutOpen(
                                &output, outputId < 0 ? WAVE_MAPPER : outputId,
                                reinterpret_cast<WAVEFORMATEX*>(outputFormat.data()), 0, 0,
                                CALLBACK_NULL);
                            if (result != MMSYSERR_NOERROR)
                                throw std::runtime_error("音频输出设备不可用");
                            PROPVARIANT point;
                            PropVariantInit(&point);
                            point.vt = VT_I8;
                            point.hVal.QuadPart = static_cast<LONGLONG>(resume * 1e7);
                            reader->Flush(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
                            hrcheck(reader->SetCurrentPosition(GUID_NULL, point),
                                    "切换音频设备时恢复进度失败");
                            base = resume;
                            eof = false;
                            setVolume();
                            if (paused)
                                waveOutPause(output);
                        }
                        break;
                    }
                } catch (const std::exception& e) {
                    fail(sanitize(unutf8(e.what())));
                }
            }
            if (quit)
                break;
            if (active && output) {
                try {
                    int inFlight = 0;
                    for (auto& slot : slots) {
                        if (slot.prepared && !(slot.header.dwFlags & WHDR_DONE)) {
                            ++inFlight;
                            continue;
                        }
                        if (slot.prepared) {
                            waveOutUnprepareHeader(output, &slot.header, sizeof(WAVEHDR));
                            slot.prepared = false;
                            slot.header = {};
                        }
                        if (eof || paused)
                            continue;
                        DWORD flags = 0;
                        LONGLONG stamp = 0;
                        Com<IMFSample> sample;
                        hrcheck(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                                   &flags, &stamp, sample.put()),
                                "读取音频中断");
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                            eof = true;
                        if (!sample)
                            continue;
                        Com<IMFMediaBuffer> buffer;
                        hrcheck(sample->ConvertToContiguousBuffer(buffer.put()), "PCM buffer");
                        BYTE* data = nullptr;
                        DWORD size = 0;
                        hrcheck(buffer->Lock(&data, nullptr, &size), "PCM data");
                        if (size > 1024 * 1024) {
                            buffer->Unlock();
                            throw std::runtime_error("音频缓冲超过内存预算");
                        }
                        slot.data.assign(data, data + size);
                        buffer->Unlock();
                        size_t frames = size / ((bits / 8) * channels),
                               n = std::min(size_t(1024), frames);
                        auto pcm = [&](size_t frame) {
                            const uint8_t* p = slot.data.data() + frame * (bits / 8) * channels;
                            if (bits == 16) {
                                int16_t v;
                                memcpy(&v, p, 2);
                                return v / 32768.0;
                            }
                            if (bits == 24) {
                                int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) |
                                            (int32_t(int8_t(p[2])) << 16);
                                return v / 8388608.0;
                            }
                            int32_t v;
                            memcpy(&v, p, 4);
                            return v / 2147483648.0;
                        };
                        if (n) {
                            for (int band = 0; band < 24; ++band) {
                                double f = 60 * pow(150.0, band / 23.0),
                                       omega = 6.283185307 * f / rate, coeff = 2 * cos(omega),
                                       q0 = 0, q1 = 0, q2 = 0;
                                for (size_t i = 0; i < n; ++i) {
                                    double v = pcm(i) * (.5 - .5 * cos(6.283185307 * i / n));
                                    q0 = coeff * q1 - q2 + v;
                                    q2 = q1;
                                    q1 = q0;
                                }
                                double energy =
                                    sqrt(std::max(0.0, q1 * q1 + q2 * q2 - coeff * q1 * q2)) / n;
                                spectrum[band] =
                                    std::clamp(log10(1 + energy * 120) / 1.5, 0.0, 1.0);
                            }
                        }
                        slot.header.lpData = (LPSTR)slot.data.data();
                        slot.header.dwBufferLength = size;
                        if (waveOutPrepareHeader(output, &slot.header, sizeof(WAVEHDR)) !=
                            MMSYSERR_NOERROR)
                            throw std::runtime_error("音频设备缓冲失败");
                        slot.prepared = true;
                        if (waveOutWrite(output, &slot.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
                            throw std::runtime_error("音频输出失败");
                        ++inFlight;
                    }
                    MMTIME time{};
                    time.wType = TIME_SAMPLES;
                    waveOutGetPosition(output, &time, sizeof(time));
                    double pos = base + (time.wType == TIME_SAMPLES ? double(time.u.sample) / rate
                                                                    : double(time.u.ms) / 1000);
                    {
                        std::lock_guard lock(mutex);
                        snapshot.playing = true;
                        snapshot.paused = paused;
                        snapshot.position = pos;
                        snapshot.duration = duration;
                        snapshot.volume = volume;
                        snapshot.sampleRate = rate;
                        snapshot.sampleBits = validBits;
                        for (int i = 0; i < 24; ++i)
                            snapshot.spectrum[i] = paused ? 0 : spectrum[i];
                    }
                    if (eof && inFlight == 0) {
                        cleanup();
                        std::lock_guard lock(mutex);
                        snapshot.playing = false;
                        snapshot.finished = true;
                        snapshot.spectrum = {};
                    }
                } catch (const std::exception& e) {
                    fail(sanitize(unutf8(e.what())));
                }
                Sleep(10);
            }
        }
        cleanup();
        if (SUCCEEDED(mf))
            MFShutdown();
        if (SUCCEEDED(com))
            CoUninitialize();
    }
} // namespace mora
