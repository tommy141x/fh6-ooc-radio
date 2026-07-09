#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fh6 {

struct HotkeysConfig {
    int kb_skip          = 0;
    int pad_skip         = 0;
    int kb_source        = 0;
    int pad_source       = 0;
    int kb_playpause     = 0;
    int pad_playpause    = 0;
    int kb_prev          = 0;
    int pad_prev         = 0;
    int kb_next_station  = 0;
    int pad_next_station = 0;
};

struct PlaybackConfig {
    std::string race_start_playback = "next"; // "next" | "restart" | "ignore" | "off"
    bool volume_normalization       = false;
    bool equalizer_enabled          = false;
    std::array<float, 5> equalizer_bands{}; // 60 / 250 / 1000 / 4000 / 12000 Hz, [-6, +6] dB
    bool force_stereo_audio = true;
    // Pre-spawn the next track's pipeline so transitions (skip / end-of-track)
    // are instant.
    bool prebuffer_next_track = true;

    HotkeysConfig hotkeys;
};

struct GeneralConfig {
    uint32_t ring_buffer_mb    = 4;
    std::string default_source = "ooc_radio";
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
};

struct AudioConfig {
    float output_gain = 1.0f;
};

// OOC Radio (radio.oocradio.com): fixed direct stream + authenticated
// api.oocradio.com/v1 metadata poll for now-playing/live-presenter info.
struct OocRadioConfig {
    bool enabled = true;
    std::string api_key; // "oocr_live_..." from https://radio.oocradio.com/ developer page
};

struct Config {
    GeneralConfig general;
    AudioConfig audio;
    OocRadioConfig ooc_radio;
    PlaybackConfig playback;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
