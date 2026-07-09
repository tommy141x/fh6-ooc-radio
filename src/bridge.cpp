// Bridge entry point. Wires up config, the single OOC Radio source, and the
// FMOD DSP, then parks the thread the DLL spawned us on. No dashboard/HTTP
// layer -- this mod is in-game only.

#include "fh6/log.hpp"
#include "fh6/config.hpp"
#include "fh6/config_store.hpp"
#include "fh6/deps.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/texture_injector.hpp"
#include "fh6/worker/worker_client.hpp"
#include "fh6/sources/ooc_radio_source.hpp"

#include <windows.h>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fh6 {

namespace {

std::filesystem::path module_directory(HMODULE self) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}

bool host_is_game(const std::filesystem::path& dll_dir) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const std::filesystem::path exe{std::wstring_view{buf, n}};

    std::error_code ec;
    if (!std::filesystem::equivalent(exe.parent_path(), dll_dir, ec)) return false;

    std::wstring name = exe.filename().wstring();
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name != L"gamelaunchhelper.exe";
}

// Swap a blank ffmpeg/texconv path for the auto-downloaded copy.
Config with_resolved_bins(Config c, const DependencyManager& deps) {
    c.general.ffmpeg_path    = deps.resolve(Tool::ffmpeg, c.general.ffmpeg_path);
    std::string texconv_path = deps.resolve(Tool::texconv, "").string();

    log::info("[bridge] Resolved ffmpeg path to: {}", c.general.ffmpeg_path.string());
    log::info("[bridge] Resolved texconv path to: {}", texconv_path);
    return c;
}

} // namespace

void run_bridge(HMODULE self) noexcept {
    const auto dir = module_directory(self);
    if (!host_is_game(dir)) return; // never spawn a bridge outside the game itself

    if (HANDLE guard = CreateMutexW(nullptr, TRUE, L"Local\\ooc-radio-fh6-bridge");
        guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(guard);
        return;
    }

    const auto data_dir = dir / "fh6-radio";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    log::init(data_dir / "bridge.log");
    log::info("[bridge] OOC Radio for FH6 starting; data_dir={}", data_dir.string());
    log::info("[bridge] Forked from fh6-universal-radio (github.com/g0ldyy/fh6-universal-radio, "
              "GPLv3) -- see NOTICE for the full attribution chain.");

    ConfigStore store{data_dir / "config.toml", load_config(data_dir / "config.toml")};
    auto cfg = store.snapshot();

    auto img = fmod_bridge::parse(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)));
    if (!img.valid()) {
        log::error("[bridge] failed to parse host PE image; aborting");
        return;
    }
    fmod_bridge::FMODFns fns;
    if (!fmod_bridge::resolve_fmod_signatures(img, fns)) {
        log::warn("[bridge] some FMOD signatures unresolved -- DSP injection disabled");
    }

    const std::size_t ring_bytes = static_cast<std::size_t>(cfg.general.ring_buffer_mb) << 20;
    AudioSourceManager mgr{ring_bytes};

    DependencyManager deps{data_dir / "bin"};

    TextureInjector::instance().set_deps(&deps);
    TextureInjector::instance().set_config_store(&store);

    // Worker process: delegates CreateProcess calls to a small external exe
    // so the fork() Wine performs is cheap (~5 MB) instead of copying the
    // game's multi-GB page table.  Falls back to direct spawn if absent.
    auto worker = std::make_shared<worker::WorkerClient>();
    {
        auto worker_exe = data_dir / "fh6-radio-worker.exe";
        if (!std::filesystem::exists(worker_exe))
            worker_exe = dir / "fh6-radio" / "fh6-radio-worker.exe";

        if (worker->start(
                worker_exe,
                {{L"RUST_LOG", L"librespot_playback::player=debug,librespot_metadata=trace"}})) {
            log::info("[bridge] worker process started");

            TextureInjector::instance().set_worker_client(worker);
        } else {
            log::warn("[bridge] worker process unavailable -- falling back to direct spawn");
        }
    }

    // Register/unregister the sole OOC Radio source to match the enabled
    // flag. Called at startup and on every config change (hand edits to
    // config.toml are picked up live, no game restart needed).
    auto sync_sources = [&mgr, &worker](const Config& c) {
        if (c.ooc_radio.enabled && !mgr.find("ooc_radio")) {
            auto src = std::make_unique<sources::OocRadioSource>(c.ooc_radio, c.general.ffmpeg_path,
                                                                 worker.get());
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.ooc_radio.enabled && mgr.find("ooc_radio")) {
            mgr.unregister_source("ooc_radio");
        }
    };

    sync_sources(with_resolved_bins(cfg, deps));

    if (!mgr.switch_to("ooc_radio")) {
        log::warn("[bridge] ooc_radio source not registered (disabled in config.toml?)");
    }

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.playback.force_stereo_audio);

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready()) {
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.playback,
                                                          cfg.audio.output_gain);
    }

    for (auto* s : mgr.sources_snapshot()) s->set_playback_options(cfg.playback);

    auto apply_config = [&bridge, &mgr, &deps, sync_sources,
                         ctrl_ptr = ctrl.get()](const Config& raw) {
        const Config c = with_resolved_bins(raw, deps);
        sync_sources(c);
        if (!mgr.active()) {
            if (!mgr.switch_to("ooc_radio")) {
                log::error("[bridge] failed to switch to ooc_radio source");
            }
        }

        // Push the gain to both: the control loop's ramper otherwise snaps
        // the bridge value back to its own cached target on the next tick.
        bridge.set_gain(c.audio.output_gain);
        bridge.set_force_stereo_audio(c.playback.force_stereo_audio);
        if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
        if (auto* r = dynamic_cast<sources::OocRadioSource*>(mgr.find("ooc_radio"))) {
            r->set_ffmpeg_path(c.general.ffmpeg_path);
            r->set_config(c.ooc_radio);
            if (mgr.active() == r && r->playback_state() != PlaybackState::playing) {
                r->play();
            }
        }

        for (auto* s : mgr.sources_snapshot()) s->set_playback_options(c.playback);
        if (ctrl_ptr) ctrl_ptr->push_playback_options(c.playback);
    };
    store.on_change(apply_config);

    // Re-resolve binary paths into the live sources once a download lands.
    deps.start([&store, &apply_config] { apply_config(store.snapshot()); });

    log::info("[bridge] running");

    for (;;) Sleep(60'000);
}

} // namespace fh6