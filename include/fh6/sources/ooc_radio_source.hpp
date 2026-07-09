#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"
#include "fh6/worker/worker_client.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace fh6::sources {

// The one and only station: a fixed direct MP3 stream plus a periodic poll
// of OOC Radio's authenticated metadata API for richer now-playing/live info
// than plain ICY tags.
class OocRadioSource final : public IAudioSource {
public:
    OocRadioSource(OocRadioConfig cfg, std::filesystem::path ffmpeg_path,
                   worker::WorkerClient* worker = nullptr);
    ~OocRadioSource() override;

    std::string_view name() const noexcept override { return "ooc_radio"; }
    std::string_view display_name() const noexcept override { return "OOC Radio"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    bool skip_next() override { return false; } // continuous broadcast -- nothing to skip to

    void pump(RingBuffer& ring) override;
    void set_playback_options(const PlaybackConfig& opts) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return AuthState::none_required; }

    // no seek, no previous/next, no queue -- it's a live broadcast
    SourceCapabilities capabilities() const noexcept override { return {false, false, false}; }

    void set_config(const OocRadioConfig& c);
    void set_ffmpeg_path(std::filesystem::path p);

private:
    struct Pipe;
    void start_pipe_locked();
    void stop_pipe_locked();

    void poll_loop(std::stop_token tok);
    void fetch_now_playing(const std::string& api_key);
    void fetch_live(const std::string& api_key);

    std::filesystem::path ffmpeg_path_;
    std::unique_ptr<Pipe> pipe_;
    mutable std::mutex
        mu_; // guards pipe_/ffmpeg_path_ -- playback only, never touched by poll_loop
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{false};

    worker::WorkerClient* worker_ = nullptr;

    // --- metadata, filled by poll_loop() on its own thread -----------------
    struct NowPlaying {
        bool valid = false;
        std::string title, artist, album, art;
        uint64_t duration_ms = 0;
    };
    struct LiveStatus {
        bool is_live = false;
        std::string presenter_name;
        std::string presenter_avatar;
    };

    mutable std::mutex meta_mu_; // guards cfg_/now_/live_ -- never held across a blocking HTTP call
    OocRadioConfig cfg_;
    NowPlaying now_;
    LiveStatus live_;

    // Declared last so it stops (via stop_token) and joins before mu_/meta_mu_
    // and the state above are destroyed.
    std::jthread meta_thread_;
};

} // namespace fh6::sources
