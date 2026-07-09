#include "fh6/config.hpp"
#include "fh6/log.hpp"

#include <toml.hpp>

#include <fstream>
#include <span>
#include <system_error>

namespace fh6 {

namespace {

template <class T> T pick(const toml::value& tbl, const char* key, T fallback) {
    try {
        if (!tbl.contains(key)) return fallback;
        return toml::find<T>(tbl, key);
    } catch (...) {
        return fallback;
    }
}

std::filesystem::path pick_path(const toml::value& tbl, const char* key) {
    auto s = pick<std::string>(tbl, key, "");
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

const toml::value& section(const toml::value& root, const char* key) {
    static const toml::value empty{toml::table{}};
    try {
        if (root.contains(key)) return root.at(key);
    } catch (...) {}
    return empty;
}

} // namespace

Config load_config(const std::filesystem::path& path) {
    Config cfg;
    if (!std::filesystem::exists(path)) {
        log::info("[config] no config.toml at {}; using defaults", path.string());
        return cfg;
    }

    toml::value root;
    try {
        root = toml::parse(path.string());
    } catch (const std::exception& e) {
        log::warn("[config] parse error in {}: {}", path.string(), e.what());
        return cfg;
    }

    const auto& g = section(root, "general");
    cfg.general.ring_buffer_mb =
        static_cast<uint32_t>(pick<int>(g, "ring_buffer_mb", cfg.general.ring_buffer_mb));
    cfg.general.default_source = pick<std::string>(g, "default_source", cfg.general.default_source);
    cfg.general.ffmpeg_path    = pick_path(g, "ffmpeg_path");

    const auto& oo        = section(root, "ooc_radio");
    cfg.ooc_radio.enabled = pick<bool>(oo, "enabled", cfg.ooc_radio.enabled);
    cfg.ooc_radio.api_key = pick<std::string>(oo, "api_key", cfg.ooc_radio.api_key);

    const auto& au = section(root, "audio");
    cfg.audio.output_gain =
        static_cast<float>(pick<double>(au, "output_gain", cfg.audio.output_gain));

    const auto& pb                       = section(root, "playback");
    const bool legacy_quick_station_skip = +pick<bool>(pb, "quick_station_skip", false);
    {
        auto rs = pick<std::string>(pb, "race_start_playback", cfg.playback.race_start_playback);
        if (rs == "next" || rs == "restart" || rs == "ignore" || rs == "off")
            cfg.playback.race_start_playback = std::move(rs);
    }
    cfg.playback.volume_normalization =
        pick<bool>(pb, "volume_normalization", cfg.playback.volume_normalization);
    cfg.playback.equalizer_enabled =
        pick<bool>(pb, "equalizer_enabled", cfg.playback.equalizer_enabled);
    cfg.playback.force_stereo_audio =
        pick<bool>(pb, "force_stereo_audio", cfg.playback.force_stereo_audio);
    cfg.playback.prebuffer_next_track =
        pick<bool>(pb, "prebuffer_next_track", cfg.playback.prebuffer_next_track);
    try {
        if (pb.contains("equalizer_bands")) {
            auto v = toml::find<std::vector<double>>(pb, "equalizer_bands");
            for (std::size_t i = 0; i < cfg.playback.equalizer_bands.size() && i < v.size(); ++i) {
                auto b = static_cast<float>(v[i]);
                if (b < -6.f) b = -6.f;
                if (b > 6.f) b = 6.f;
                cfg.playback.equalizer_bands[i] = b;
            }
        }
    } catch (...) {}

    const auto& hk                  = section(root, "hotkeys");
    cfg.playback.hotkeys.kb_skip    = pick<int>(hk, "kb_skip", cfg.playback.hotkeys.kb_skip);
    cfg.playback.hotkeys.pad_skip   = pick<int>(hk, "pad_skip", cfg.playback.hotkeys.pad_skip);
    cfg.playback.hotkeys.kb_source  = pick<int>(hk, "kb_source", cfg.playback.hotkeys.kb_source);
    cfg.playback.hotkeys.pad_source = pick<int>(hk, "pad_source", cfg.playback.hotkeys.pad_source);
    cfg.playback.hotkeys.kb_playpause =
        pick<int>(hk, "kb_playpause", cfg.playback.hotkeys.kb_playpause);
    cfg.playback.hotkeys.pad_playpause =
        pick<int>(hk, "pad_playpause", cfg.playback.hotkeys.pad_playpause);
    cfg.playback.hotkeys.kb_prev  = pick<int>(hk, "kb_prev", cfg.playback.hotkeys.kb_prev);
    cfg.playback.hotkeys.pad_prev = pick<int>(hk, "pad_prev", cfg.playback.hotkeys.pad_prev);
    cfg.playback.hotkeys.kb_next_station =
        pick<int>(hk, "kb_next_station", cfg.playback.hotkeys.kb_next_station);
    cfg.playback.hotkeys.pad_next_station =
        pick<int>(hk, "pad_next_station", cfg.playback.hotkeys.pad_next_station);

    const bool any_hotkey_bound =
        cfg.playback.hotkeys.kb_skip || cfg.playback.hotkeys.pad_skip ||
        cfg.playback.hotkeys.kb_source || cfg.playback.hotkeys.pad_source ||
        cfg.playback.hotkeys.kb_playpause || cfg.playback.hotkeys.pad_playpause ||
        cfg.playback.hotkeys.kb_prev || cfg.playback.hotkeys.pad_prev ||
        cfg.playback.hotkeys.kb_next_station || cfg.playback.hotkeys.pad_next_station;
    if (legacy_quick_station_skip && !any_hotkey_bound) {
        cfg.playback.hotkeys.kb_skip  = 0x9999; // legacy quick-skip sentinel
        cfg.playback.hotkeys.pad_skip = 0x9999; // legacy quick-skip sentinel
    }

    return cfg;
}

namespace {

// Hand-rolled emitter. toml11's serialiser output changes across majors
// and we want the file to stay diff-friendly for hand edits.
struct Emitter {
    std::string out;

    void header(const char* name) {
        if (!out.empty()) out += '\n';
        out += '[';
        out += name;
        out += "]\n";
    }
    void array_header(const char* name) {
        if (!out.empty()) out += '\n';
        out += "[[";
        out += name;
        out += "]]\n";
    }
    // Literal (single-quoted) strings don't process escapes, which is what we
    // want for Windows paths. Use them when the value contains \ or " and no ';
    // otherwise basic double-quoted with backslash escaping.
    void quoted(std::string_view str) {
        const bool has_bs = str.find('\\') != std::string_view::npos;
        const bool has_dq = str.find('"') != std::string_view::npos;
        const bool has_sq = str.find('\'') != std::string_view::npos;
        if ((has_bs || has_dq) && !has_sq) {
            out += '\'';
            out += str;
            out += '\'';
        } else {
            out += '"';
            for (char c : str) {
                if (c == '\\' || c == '"') out += '\\';
                out += c;
            }
            out += '"';
        }
    }
    void kv(std::string_view key, std::string_view str) {
        out += key;
        out += " = ";
        quoted(str);
        out += '\n';
    }
    void kv_paths(std::string_view key, const std::vector<std::filesystem::path>& v) {
        out += key;
        out += " = [";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            quoted(v[i].string());
        }
        out += "]\n";
    }
    void kv(std::string_view key, bool v) {
        out += key;
        out += " = ";
        out += v ? "true" : "false";
        out += '\n';
    }
    void kv(std::string_view key, int64_t v) {
        out += key;
        out += " = ";
        out += std::to_string(v);
        out += '\n';
    }
    void kv(std::string_view key, double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        out += key;
        out += " = ";
        out += buf;
        out += '\n';
    }
    void kv_path(std::string_view key, const std::filesystem::path& p) {
        kv(key, p.empty() ? std::string{} : p.string());
    }
    void kv_strs(std::string_view key, const std::vector<std::string>& v) {
        out += key;
        out += " = [";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            out += '"';
            out += v[i];
            out += '"';
        }
        out += "]\n";
    }
    void kv_floats(std::string_view key, std::span<const float> v) {
        out += key;
        out += " = [";
        char buf[32];
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v[i]));
            out += buf;
        }
        out += "]\n";
    }
};

} // namespace

void save_config(const std::filesystem::path& path, const Config& cfg) {
    Emitter e;
    e.header("general");
    e.kv("ring_buffer_mb", (int64_t)cfg.general.ring_buffer_mb);
    e.kv("default_source", cfg.general.default_source);
    e.kv_path("ffmpeg_path", cfg.general.ffmpeg_path);

    e.header("ooc_radio");
    e.kv("enabled", cfg.ooc_radio.enabled);
    e.kv("api_key", cfg.ooc_radio.api_key);

    e.header("audio");
    e.kv("output_gain", (double)cfg.audio.output_gain);

    e.header("playback");
    e.kv("race_start_playback", cfg.playback.race_start_playback);
    e.kv("volume_normalization", cfg.playback.volume_normalization);
    e.kv("equalizer_enabled", cfg.playback.equalizer_enabled);
    e.kv_floats("equalizer_bands", std::span<const float>{cfg.playback.equalizer_bands});
    e.kv("force_stereo_audio", cfg.playback.force_stereo_audio);
    e.kv("prebuffer_next_track", cfg.playback.prebuffer_next_track);

    e.header("hotkeys");
    e.kv("kb_skip", (int64_t)cfg.playback.hotkeys.kb_skip);
    e.kv("pad_skip", (int64_t)cfg.playback.hotkeys.pad_skip);
    e.kv("kb_source", (int64_t)cfg.playback.hotkeys.kb_source);
    e.kv("pad_source", (int64_t)cfg.playback.hotkeys.pad_source);
    e.kv("kb_playpause", (int64_t)cfg.playback.hotkeys.kb_playpause);
    e.kv("pad_playpause", (int64_t)cfg.playback.hotkeys.pad_playpause);
    e.kv("kb_prev", (int64_t)cfg.playback.hotkeys.kb_prev);
    e.kv("pad_prev", (int64_t)cfg.playback.hotkeys.pad_prev);
    e.kv("kb_next_station", (int64_t)cfg.playback.hotkeys.kb_next_station);
    e.kv("pad_next_station", (int64_t)cfg.playback.hotkeys.pad_next_station);

    auto tmp  = path;
    tmp      += ".tmp";
    {
        std::ofstream os{tmp, std::ios::binary | std::ios::trunc};
        if (!os) throw std::system_error{errno, std::system_category(), tmp.string()};
        os.write(e.out.data(), (std::streamsize)e.out.size());
        if (!os) throw std::system_error{errno, std::system_category(), tmp.string()};
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::system_error{ec};
}

} // namespace fh6
