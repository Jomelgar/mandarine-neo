// Copyright 2024 Mandarine Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "oboe_sink.h"

#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <oboe/Oboe.h>

#include "audio_core/audio_types.h"
#include "common/logging/log.h"

namespace AudioCore {

class OboeSink::Impl : public oboe::AudioStreamCallback {
public:
    Impl() = default;

    ~Impl() override {
        stop();
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /* oboeStream */, void* audio_data,
                                          int32_t num_frames) override {
        auto* output_buffer = static_cast<s16*>(audio_data);
        if (m_callback) {
            m_callback(output_buffer, static_cast<std::size_t>(num_frames));
        }
        return oboe::DataCallbackResult::Continue;
    }

    void onErrorBeforeClose(oboe::AudioStream* /* oboeStream */, oboe::Result error) override {
        LOG_WARNING(Audio_Sink, "AudioStream error before close: {}", oboe::convertToText(error));
    }

    void onErrorAfterClose(oboe::AudioStream* oboe_stream, oboe::Result error) override {
        LOG_WARNING(Audio_Sink, "AudioStream closed after error: {}, restarting",
                    oboe::convertToText(error));

        // Avoid deadlocking with stop()/start() if they are already closing the stream.
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock.owns_lock()) {
            LOG_WARNING(Audio_Sink, "Skipping audio restart while the sink is being changed");
            return;
        }

        // Oboe has already closed the stream, so drop our reference before reopening it.
        if (m_stream && m_stream.get() == oboe_stream) {
            m_stream.reset();
        }

        // Avoid tight restart loops when a device is repeatedly disconnecting.
        const auto now = std::chrono::steady_clock::now();
        if (now - m_last_restart < std::chrono::milliseconds{500}) {
            LOG_WARNING(Audio_Sink, "Ignoring too frequent audio restart");
            return;
        }
        m_last_restart = now;

        OpenAndStartLocked();
    }

    bool start() {
        std::lock_guard lock{m_mutex};
        CloseLocked();
        return OpenAndStartLocked();
    }

    void stop() {
        std::lock_guard lock{m_mutex};
        CloseLocked();
    }

    int32_t GetSampleRate() const {
        std::lock_guard lock{m_mutex};
        return m_sample_rate;
    }

    void SetCallback(std::function<void(s16*, std::size_t)> cb) {
        std::lock_guard lock{m_mutex};
        m_callback = std::move(cb);
    }

    bool IsActive() const {
        std::lock_guard lock{m_mutex};
        if (!m_stream) {
            return false;
        }
        const auto state = m_stream->getState();
        return state == oboe::StreamState::Started || state == oboe::StreamState::Starting;
    }

    void SetDeviceId(int32_t device_id) {
        std::lock_guard lock{m_mutex};
        m_device_id = device_id;
    }

private:
    struct StreamConfig {
        oboe::AudioApi audio_api;
        oboe::SharingMode sharing_mode;
        oboe::PerformanceMode performance_mode;
    };

    bool OpenAndStartLocked() {
        static constexpr std::array configs{
            StreamConfig{oboe::AudioApi::AAudio, oboe::SharingMode::Exclusive,
                         oboe::PerformanceMode::LowLatency},
            StreamConfig{oboe::AudioApi::AAudio, oboe::SharingMode::Shared,
                         oboe::PerformanceMode::LowLatency},
            StreamConfig{oboe::AudioApi::OpenSLES, oboe::SharingMode::Shared,
                         oboe::PerformanceMode::LowLatency},
            StreamConfig{oboe::AudioApi::OpenSLES, oboe::SharingMode::Shared,
                         oboe::PerformanceMode::None},
        };

        for (const auto& config : configs) {
            if (TryOpenAndStartLocked(config)) {
                return true;
            }
        }

        LOG_CRITICAL(Audio_Sink, "Unable to open any Oboe output stream");
        return false;
    }

    bool TryOpenAndStartLocked(const StreamConfig& config) {
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
            ->setAudioApi(config.audio_api)
            ->setSharingMode(config.sharing_mode)
            ->setPerformanceMode(config.performance_mode)
            ->setUsage(oboe::Usage::Game)
            ->setContentType(oboe::ContentType::Music)
            ->setFormat(oboe::AudioFormat::I16)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setSampleRate(m_sample_rate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::High)
            ->setChannelCount(2)
            ->setCallback(this);

        if (m_device_id.has_value()) {
            builder.setDeviceId(*m_device_id);
        }

        std::shared_ptr<oboe::AudioStream> stream;
        const auto open_result = builder.openStream(stream);
        if (open_result != oboe::Result::OK) {
            LOG_WARNING(Audio_Sink, "Failed to open Oboe stream (api {}, sharing {}): {}",
                        static_cast<int>(config.audio_api), static_cast<int>(config.sharing_mode),
                        oboe::convertToText(open_result));
            return false;
        }

        const auto start_result = stream->start();
        if (start_result != oboe::Result::OK) {
            LOG_WARNING(Audio_Sink, "Failed to start Oboe stream: {}",
                        oboe::convertToText(start_result));
            stream->close();
            return false;
        }

        m_stream = std::move(stream);
        m_sample_rate = m_stream->getSampleRate();
        LOG_INFO(Audio_Sink, "Oboe stream started (api {}, sharing {}, rate {}, device {})",
                 static_cast<int>(m_stream->getAudioApi()),
                 static_cast<int>(m_stream->getSharingMode()), m_stream->getSampleRate(),
                 m_stream->getDeviceId());
        return true;
    }

    void CloseLocked() {
        if (!m_stream) {
            return;
        }

        if (m_stream->getState() != oboe::StreamState::Closed) {
            const auto stop_result = m_stream->stop();
            if (stop_result != oboe::Result::OK) {
                LOG_WARNING(Audio_Sink, "Error stopping Oboe stream: {}",
                            oboe::convertToText(stop_result));
            }

            const auto close_result = m_stream->close();
            if (close_result != oboe::Result::OK) {
                LOG_WARNING(Audio_Sink, "Error closing Oboe stream: {}",
                            oboe::convertToText(close_result));
            }
        }

        m_stream.reset();
    }

    mutable std::mutex m_mutex;
    std::shared_ptr<oboe::AudioStream> m_stream;
    std::function<void(s16*, std::size_t)> m_callback;
    std::optional<int32_t> m_device_id;
    int32_t m_sample_rate = native_sample_rate;
    std::chrono::steady_clock::time_point m_last_restart{};
};

OboeSink::OboeSink(std::string_view device_id) : impl(std::make_unique<Impl>()) {
    if (device_id != auto_device_name && !device_id.empty()) {
        try {
            impl->SetDeviceId(std::stoi(std::string(device_id)));
        } catch (const std::exception&) {
            LOG_WARNING(Audio_Sink, "Invalid Oboe device id '{}', using default output", device_id);
        }
    }
}

OboeSink::~OboeSink() {
    // Ensures resources are freed up when OboeSink is destroyed
    if (impl) {
        impl->stop();
    }
}

unsigned int OboeSink::GetNativeSampleRate() const {
    return impl->GetSampleRate();
}

void OboeSink::SetCallback(std::function<void(s16*, std::size_t)> cb) {
    impl->SetCallback(std::move(cb));
    if (!impl->start()) {
        LOG_CRITICAL(Audio_Sink, "Failed to start Oboe output stream");
    }
}

bool OboeSink::IsActive() const {
    return impl->IsActive();
}

std::vector<std::string> ListOboeSinkDevices() {
    return {auto_device_name};
}

} // namespace AudioCore
