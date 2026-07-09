#include "fh6/sources/ooc_radio_source.hpp"
#include "fh6/log.hpp"
#include "fh6/net/http_get.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <algorithm>
#include <chrono>

namespace fh6::sources {

using namespace std::chrono_literals; // file scope: needed by poll_loop() below too

namespace {
using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

constexpr const char* kStreamUrl     = "https://radio.oocradio.com/";
constexpr const char* kNowPlayingUrl = "https://api.oocradio.com/v1/now-playing";
constexpr const char* kLiveUrl       = "https://api.oocradio.com/v1/live";
constexpr auto kNowPlayingInterval   = 5s;
constexpr int kLiveEveryNPolls       = 4; // ~20s at a 5s base interval

bool api_key_is_safe(const std::string& key) {
    return key.find_first_of("\r\n\"") == std::string::npos;
}

std::string json_str(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return {};
    try {
        return j[key].get<std::string>();
    } catch (...) {
        return {};
    }
}

uint64_t json_seconds_to_ms(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return 0;
    try {
        return static_cast<uint64_t>(j[key].get<double>() * 1000.0);
    } catch (...) {
        return 0;
    }
}

} // namespace

struct OocRadioSource::Pipe {
    worker::WorkerClient* worker = nullptr;
    uint32_t pipeline_id         = 0;

    HANDLE job         = nullptr;
    HANDLE proc_ff     = nullptr;
    HANDLE read_pipe   = nullptr;
    HANDLE stderr_pipe = nullptr;
    bool ended         = false;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (stderr_pipe) CloseHandle(stderr_pipe);
        if (worker && pipeline_id) worker->kill_pipeline(pipeline_id);
        subprocess::reap(proc_ff); // direct-mode child (no-op in worker mode)
        if (job) CloseHandle(job);
    }
};

OocRadioSource::OocRadioSource(OocRadioConfig cfg, std::filesystem::path ffmpeg_path,
                               worker::WorkerClient* worker)
    : ffmpeg_path_{std::move(ffmpeg_path)}, worker_{worker}, cfg_{std::move(cfg)} {
    meta_thread_ = std::jthread{[this](std::stop_token tok) { poll_loop(tok); }};
}

OocRadioSource::~OocRadioSource() {
    meta_thread_.request_stop();
    // meta_thread_ joins in its own destructor (declared last), after this
    // one runs -- request_stop() here just wakes it early instead of waiting
    // out its sleep.
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool OocRadioSource::initialize() { return cfg_.enabled; }

void OocRadioSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void OocRadioSource::set_config(const OocRadioConfig& c) {
    std::scoped_lock lk{meta_mu_};
    cfg_ = c;
}

void OocRadioSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void OocRadioSource::start_pipe_locked() {
    stop_pipe_locked();

    auto pipe     = std::make_unique<Pipe>();
    const auto ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();

    std::wstring ff_cmd = quote(ff) +
                          L" -hide_banner -loglevel warning "
                          L"-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 "
                          L"-i " +
                          quote(widen(kStreamUrl)) + L" ";
    if (volume_norm_.load(std::memory_order_acquire)) {
        ff_cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    }
    ff_cmd += L"-f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    if (worker_ && worker_->alive()) {
        if (auto result = worker_->spawn_pipeline({ff_cmd}, L"", true); result.ok) {
            pipe->worker      = worker_;
            pipe->pipeline_id = result.pipeline_id;
            pipe->read_pipe   = result.pcm_pipe;
            pipe->stderr_pipe = result.meta_pipe;
            pipe_             = std::move(pipe);

            state_.store(PlaybackState::buffering, std::memory_order_release);
            log::info("[ooc_radio] Started stream via worker: {}", kStreamUrl);
            return;
        }
        log::warn("[ooc_radio] worker spawn failed -- falling back to direct spawn");
    }

    // direct spawn fallback: only executes if the worker is dead/unavailable.
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[ooc_radio] CreateJobObject failed");
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;

    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) return;
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);

    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
        CloseHandle(ff_out_r);
        CloseHandle(ff_out_w);
        return;
    }
    SetHandleInformation(err_r, 0, HANDLE_FLAG_INHERIT);
    HANDLE nul_in = open_nul(GENERIC_READ);

    pipe->proc_ff     = spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_w);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    CloseHandle(err_w);

    if (!pipe->proc_ff) {
        log::warn("[ooc_radio] failed to launch ffmpeg -- {}",
                  describe_launch_failure(std::wstring{ff}, ec_ff, !ffmpeg_path_.empty()));
        CloseHandle(ff_out_r);
        CloseHandle(err_r);
        if (nul_in) CloseHandle(nul_in);
        return;
    }
    if (nul_in) CloseHandle(nul_in);

    pipe->read_pipe   = ff_out_r;
    pipe->stderr_pipe = err_r;
    pipe_             = std::move(pipe);

    state_.store(PlaybackState::buffering, std::memory_order_release);
    log::info("[ooc_radio] Started stream: {}", kStreamUrl);
}

void OocRadioSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void OocRadioSource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void OocRadioSource::pause() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void OocRadioSource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void OocRadioSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
}

void OocRadioSource::pump(RingBuffer& ring) {
    auto st = state_.load(std::memory_order_acquire);
    if (st != PlaybackState::playing && st != PlaybackState::buffering) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    // Drain stderr so a chatty ffmpeg (reconnect warnings etc.) can't fill the
    // pipe buffer and stall the process; we don't parse it for metadata --
    // that comes from the OOC Radio API poll instead.
    if (p->stderr_pipe) {
        DWORD avail = 0;
        char buf[2048];
        int safety = 0;
        while (safety++ < 16 &&
               PeekNamedPipe(p->stderr_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(p->stderr_pipe, buf, std::min<DWORD>(sizeof(buf), avail), &got,
                          nullptr) ||
                got == 0)
                break;
        }
    }

    if (!p->read_pipe) return;

    if (p->ended) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        p->ended = true;
        CloseHandle(p->read_pipe);
        p->read_pipe = nullptr;
        return;
    }

    if (avail == 0) {
        if (state_.load(std::memory_order_acquire) == PlaybackState::playing &&
            ring.readable() < 16384) {
            state_.store(PlaybackState::buffering, std::memory_order_release);
        }
        return;
    }

    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;

        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        want &= ~std::size_t{3}; // 4-byte align (16-bit stereo)
        if (!want) break;

        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            p->ended = true;
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
            return;
        }

        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);

        ring.write(buf, aligned);
        avail = avail > got ? avail - got : 0;

        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() >= 384 * 1024) { // ~2 seconds of pre-buffer
            state_.store(PlaybackState::playing, std::memory_order_release);
        }
    }
}

TrackInfo OocRadioSource::current_track() const {
    std::scoped_lock lk{meta_mu_};
    TrackInfo info;
    if (now_.valid) {
        info.title       = now_.title.empty() ? "OOC Radio" : now_.title;
        info.artist      = now_.artist;
        info.album       = now_.album;
        info.artwork_url = now_.art;
        info.duration_ms = now_.duration_ms;
    } else {
        info.title = "OOC Radio";
    }

    if (live_.is_live && !live_.presenter_name.empty()) {
        info.artist = info.artist.empty()
                        ? ("\xF0\x9F\x94\xB4 " + live_.presenter_name + " LIVE") // "🔴 "
                        : (info.artist + " \xC2\xB7 \xF0\x9F\x94\xB4 " + live_.presenter_name +
                           " LIVE"); // " · 🔴 "
        if (!live_.presenter_avatar.empty()) info.artwork_url = live_.presenter_avatar;
    }
    return info;
}

void OocRadioSource::poll_loop(std::stop_token tok) {
    int tick = 0;
    while (!tok.stop_requested()) {
        std::string key;
        {
            std::scoped_lock lk{meta_mu_};
            key = cfg_.api_key;
        }

        if (!key.empty()) {
            if (api_key_is_safe(key)) {
                fetch_now_playing(key);
                if (tick % kLiveEveryNPolls == 0) fetch_live(key);
            } else {
                log::error("[ooc_radio] api_key contains invalid characters; skipping poll");
            }
        }

        ++tick;
        for (auto slept = 0s; slept < kNowPlayingInterval && !tok.stop_requested(); slept += 1s) {
            std::this_thread::sleep_for(1s);
        }
    }
}

void OocRadioSource::fetch_now_playing(const std::string& api_key) {
    const auto auth = "Authorization: Bearer " + api_key;
    auto body       = net::http_get(kNowPlayingUrl, auth); // blocking -- meta_mu_ NOT held here
    if (!body) return; // keep last known-good values on failure/401/429

    try {
        const auto j = nlohmann::json::parse(*body);
        NowPlaying np;
        np.title       = json_str(j, "title");
        np.artist      = json_str(j, "artist");
        np.album       = json_str(j, "album");
        np.art         = json_str(j, "art");
        np.duration_ms = json_seconds_to_ms(j, "duration");
        np.valid       = true;

        std::scoped_lock lk{meta_mu_};
        now_ = std::move(np);
    } catch (const std::exception& e) {
        log::warn("[ooc_radio] failed to parse /now-playing response: {}", e.what());
    }
}

void OocRadioSource::fetch_live(const std::string& api_key) {
    const auto auth = "Authorization: Bearer " + api_key;
    auto body       = net::http_get(kLiveUrl, auth);
    if (!body) return;

    try {
        const auto j = nlohmann::json::parse(*body);
        LiveStatus ls;
        ls.is_live = j.contains("is_live") && !j["is_live"].is_null() && j["is_live"].get<bool>();
        if (ls.is_live && j.contains("presenter") && !j["presenter"].is_null()) {
            const auto& p       = j["presenter"];
            ls.presenter_name   = json_str(p, "display_name");
            ls.presenter_avatar = json_str(p, "avatar_url");
        }

        std::scoped_lock lk{meta_mu_};
        live_ = std::move(ls);
    } catch (const std::exception& e) {
        log::warn("[ooc_radio] failed to parse /live response: {}", e.what());
    }
}

} // namespace fh6::sources
