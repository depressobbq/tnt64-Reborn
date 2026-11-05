#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "custom_audio.hpp"
#include "recomp.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "SDL.h"

namespace fs = std::filesystem;

namespace {
    static const char* k_track_names[16] = {
        "morocco", "dvie", "polyasia", "floppy",
        "pyramid", "giali", "thread6", "haluci",
        "mayan", "greek", "egypt", "celtic",
        "africa", "japan", "kalinka", "title"
    };

    struct TrackEntry {
        std::string path;
        int track_id = -1; // 1..16 for soundtrack, 17 for custom
        std::string name;
        bool is_custom = false;
    };

    struct AudioBuffer {
        std::string path;
        int track_id = -1;
        std::vector<float> samples; // 48000 Hz, stereo interleaved (L, R)
        size_t cursor = 0;
        bool is_loaded = false;
    };

    std::mutex g_audio_mutex;
    AudioBuffer g_current_track;
    int g_current_track_id = -1;
    bool g_is_playing = false;
    std::atomic<float> g_custom_volume{ 1.0f };

    // Continuous rotation is ONLY enabled in Random music mode
    bool g_is_random_mode = false;      // "RANDOM" in Audio Menu: rotates all soundtrack + custom tracks
    std::string g_staged_track_path;    // Staged path picked by tnt_on_pick_random_bgm

    // Background preloader for seamless rotation with zero hitch
    AudioBuffer g_next_track;
    std::atomic<bool> g_next_track_ready{ false };
    std::thread g_preload_thread;
    std::mutex g_preload_mutex;
    std::condition_variable g_preload_cv;
    std::atomic<bool> g_shutdown{ false };
    std::atomic<bool> g_preload_requested{ false };

    struct PreloadThreadGuard {
        ~PreloadThreadGuard() {
            g_shutdown.store(true);
            g_preload_cv.notify_all();
            if (g_preload_thread.joinable()) {
                g_preload_thread.join();
            }
        }
    };
    static PreloadThreadGuard s_preload_guard;

    std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    fs::path get_soundtrack_dir() {
        return fs::current_path() / "soundtrack";
    }

    fs::path get_custom_tracks_dir() {
        return fs::current_path() / "custom_tracks";
    }

    std::vector<std::string> get_custom_track_files() {
        std::vector<std::string> files;
        fs::path p = get_custom_tracks_dir();
        if (!fs::exists(p) || !fs::is_directory(p)) {
            return files;
        }
        for (const auto& entry : fs::directory_iterator(p)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = to_lower(entry.path().extension().string());
            if (ext == ".mp3" || ext == ".wav") {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::string find_soundtrack_file(int track_id) {
        if (track_id < 0 || track_id >= 16) return "";
        std::string name = k_track_names[track_id];
        fs::path p = get_soundtrack_dir();
        if (!fs::exists(p) || !fs::is_directory(p)) return "";

        for (const auto& entry : fs::directory_iterator(p)) {
            if (!entry.is_regular_file()) continue;
            std::string stem = to_lower(entry.path().stem().string());
            std::string ext = to_lower(entry.path().extension().string());
            if (ext != ".mp3" && ext != ".wav") continue;

            if (stem == name || stem.find(name) != std::string::npos || name.find(stem) != std::string::npos) {
                return entry.path().string();
            }
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), "%02d", track_id + 1);
            if (stem.find(num_buf) != std::string::npos) {
                return entry.path().string();
            }
        }
        return "";
    }

    std::vector<TrackEntry> get_all_available_tracks() {
        std::vector<TrackEntry> pool;
        // 1..16 soundtrack tracks
        for (int i = 0; i < 16; ++i) {
            std::string p = find_soundtrack_file(i);
            if (!p.empty()) {
                pool.push_back({ p, i + 1, k_track_names[i], false });
            }
        }
        // Custom tracks
        std::vector<std::string> ct_files = get_custom_track_files();
        for (const auto& f : ct_files) {
            pool.push_back({ f, 17, fs::path(f).stem().string(), true });
        }
        return pool;
    }

    TrackEntry pick_random_track(const std::string& current_path) {
        auto pool = get_all_available_tracks();
        if (pool.empty()) return {};
        if (pool.size() == 1) return pool[0];

        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
        size_t idx = dist(rng);
        if (pool[idx].path == current_path) {
            idx = (idx + 1 + (dist(rng) % (pool.size() - 1))) % pool.size();
        }
        return pool[idx];
    }

    TrackEntry pick_random_custom_track(const std::string& current_path) {
        auto ct_files = get_custom_track_files();
        if (ct_files.empty()) return {};
        if (ct_files.size() == 1) return { ct_files[0], 17, fs::path(ct_files[0]).stem().string(), true };

        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, ct_files.size() - 1);
        size_t idx = dist(rng);
        if (ct_files[idx] == current_path) {
            idx = (idx + 1 + (dist(rng) % (ct_files.size() - 1))) % ct_files.size();
        }
        return { ct_files[idx], 17, fs::path(ct_files[idx]).stem().string(), true };
    }

    bool resample_linear(const float* in, size_t in_frames, unsigned int in_rate,
                         std::vector<float>& out, unsigned int out_rate) {
        if (in_frames == 0) return false;
        if (in_rate == out_rate) {
            out.assign(in, in + (in_frames * 2));
            return true;
        }
        double ratio = static_cast<double>(in_rate) / static_cast<double>(out_rate);
        size_t out_frames = static_cast<size_t>(in_frames / ratio);
        out.resize(out_frames * 2);

        for (size_t i = 0; i < out_frames; ++i) {
            double src_idx = i * ratio;
            size_t idx0 = static_cast<size_t>(src_idx);
            size_t idx1 = std::min(idx0 + 1, in_frames - 1);
            float frac = static_cast<float>(src_idx - idx0);

            out[i * 2 + 0] = in[idx0 * 2 + 0] * (1.0f - frac) + in[idx1 * 2 + 0] * frac;
            out[i * 2 + 1] = in[idx0 * 2 + 1] * (1.0f - frac) + in[idx1 * 2 + 1] * frac;
        }
        return true;
    }

    bool load_audio_file(const std::string& path, std::vector<float>& out_samples_48k) {
        std::string ext = to_lower(fs::path(path).extension().string());
        if (ext == ".mp3") {
            drmp3_config cfg;
            drmp3_uint64 total_frames = 0;
            float* pSampleData = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &cfg, &total_frames, nullptr);
            if (!pSampleData) {
                printf("[custom audio] Failed to decode MP3: %s\n", path.c_str());
                return false;
            }

            std::vector<float> stereo_input;
            if (cfg.channels == 1) {
                stereo_input.resize(total_frames * 2);
                for (size_t i = 0; i < total_frames; ++i) {
                    stereo_input[i * 2 + 0] = pSampleData[i];
                    stereo_input[i * 2 + 1] = pSampleData[i];
                }
            } else if (cfg.channels == 2) {
                stereo_input.assign(pSampleData, pSampleData + (total_frames * 2));
            } else {
                stereo_input.resize(total_frames * 2);
                for (size_t i = 0; i < total_frames; ++i) {
                    stereo_input[i * 2 + 0] = pSampleData[i * cfg.channels + 0];
                    stereo_input[i * 2 + 1] = pSampleData[i * cfg.channels + 1];
                }
            }
            drmp3_free(pSampleData, nullptr);

            return resample_linear(stereo_input.data(), total_frames, cfg.sampleRate, out_samples_48k, 48000);
        } else if (ext == ".wav") {
            drwav wav;
            if (!drwav_init_file(&wav, path.c_str(), nullptr)) {
                printf("[custom audio] Failed to open WAV: %s\n", path.c_str());
                return false;
            }
            std::vector<float> raw_frames(wav.totalPCMFrameCount * wav.channels);
            drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, raw_frames.data());
            drwav_uninit(&wav);

            std::vector<float> stereo_input;
            if (wav.channels == 1) {
                stereo_input.resize(wav.totalPCMFrameCount * 2);
                for (size_t i = 0; i < wav.totalPCMFrameCount; ++i) {
                    stereo_input[i * 2 + 0] = raw_frames[i];
                    stereo_input[i * 2 + 1] = raw_frames[i];
                }
            } else if (wav.channels == 2) {
                stereo_input = std::move(raw_frames);
            } else {
                stereo_input.resize(wav.totalPCMFrameCount * 2);
                for (size_t i = 0; i < wav.totalPCMFrameCount; ++i) {
                    stereo_input[i * 2 + 0] = raw_frames[i * wav.channels + 0];
                    stereo_input[i * 2 + 1] = raw_frames[i * wav.channels + 1];
                }
            }
            return resample_linear(stereo_input.data(), wav.totalPCMFrameCount, wav.sampleRate, out_samples_48k, 48000);
        }
        return false;
    }

    void request_preload() {
        {
            std::lock_guard<std::mutex> lock(g_preload_mutex);
            g_preload_requested.store(true);
        }
        g_preload_cv.notify_one();
    }

    void preload_worker() {
        while (!g_shutdown.load()) {
            std::unique_lock<std::mutex> lk(g_preload_mutex);
            g_preload_cv.wait(lk, [] {
                return g_shutdown.load() || g_preload_requested.load();
            });
            if (g_shutdown.load()) break;
            g_preload_requested.store(false);
            lk.unlock();

            std::string current_path;

            {
                std::lock_guard<std::mutex> lock(g_audio_mutex);
                if (!g_is_playing || !g_is_random_mode) {
                    continue;
                }
                current_path = g_current_track.path;
            }

            TrackEntry next_entry = pick_random_track(current_path);
            if (next_entry.path.empty()) continue;

            AudioBuffer temp_buf;
            if (load_audio_file(next_entry.path, temp_buf.samples)) {
                temp_buf.path = next_entry.path;
                temp_buf.track_id = next_entry.track_id;
                temp_buf.is_loaded = true;
                temp_buf.cursor = 0;

                std::lock_guard<std::mutex> lock(g_audio_mutex);
                if (g_is_playing && g_is_random_mode) {
                    g_next_track = std::move(temp_buf);
                    g_next_track_ready.store(true);
                    printf("[custom audio] Preloaded next random track (%s): %s\n",
                           next_entry.is_custom ? "Custom" : "Soundtrack",
                           next_entry.path.c_str());
                    fflush(stdout);
                }
            }
        }
    }
}

namespace tnt::audio {
    void init_custom_audio() {
        fs::path st_dir = get_soundtrack_dir();
        fs::path ct_dir = get_custom_tracks_dir();
        std::error_code ec;
        if (!fs::exists(st_dir)) fs::create_directories(st_dir, ec);
        if (!fs::exists(ct_dir)) fs::create_directories(ct_dir, ec);

        // Start background preloader thread
        if (!g_preload_thread.joinable()) {
            std::atexit(tnt::audio::shutdown_custom_audio);
            g_preload_thread = std::thread(preload_worker);
        }

        auto all = get_all_available_tracks();
        printf("[custom audio] Initialized! Total rotating pool: %zu tracks (soundtrack + custom)\n", all.size());
        fflush(stdout);
    }

    void shutdown_custom_audio() {
        static std::atomic<bool> s_has_shutdown{ false };
        if (s_has_shutdown.exchange(true)) return;
        g_shutdown.store(true);
        g_preload_cv.notify_all();
        if (g_preload_thread.joinable()) {
            g_preload_thread.join();
        }
    }

    bool is_custom_playing() {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        return g_is_playing;
    }

    bool play_track(uint8_t* /*rdram*/, int track_id) {
        bool is_custom = (track_id == 17);

        std::string target_file;
        bool keep_random_mode = false;

        {
            std::lock_guard<std::mutex> lock(g_audio_mutex);
            if (!g_staged_track_path.empty()) {
                target_file = g_staged_track_path;
                g_staged_track_path.clear();
                keep_random_mode = true;
            }
        }

        if (keep_random_mode) {
            // target_file was explicitly chosen by random mode roll
            g_is_random_mode = true;
        } else if (is_custom) {
            // In CHOOSE mode with Custom: pick one random track from custom_tracks for this match, and loop it
            TrackEntry entry = pick_random_custom_track(g_current_track.path);
            if (entry.path.empty()) {
                printf("[custom audio] Custom track selected, but '%s' has no .mp3/.wav files.\n",
                       get_custom_tracks_dir().string().c_str());
                fflush(stdout);
                stop_custom_audio();
                return true; // Return true to prevent N64 sequence bank crash on invalid index 17
            }
            target_file = entry.path;
            g_is_random_mode = false;
            printf("[custom audio] Selected Custom Track for match (looping): %s\n", target_file.c_str());
            fflush(stdout);
        } else if (track_id >= 0 && track_id <= 16) {
            // TNT track IDs are 1-based: 1=Morocco, 2=Dvie, ..., 10=Greek, 11=Egypt, 12=Celtic, ..., 16=Title
            int name_idx = (track_id <= 0) ? 0 : (track_id - 1);
            target_file = find_soundtrack_file(name_idx);
            if (target_file.empty()) {
                // No live MP3 replacement found - let original N64 music play!
                printf("[custom audio] No soundtrack replacement for track #%d (%s) - playing native N64 synth.\n",
                       track_id, k_track_names[name_idx]);
                fflush(stdout);
                stop_custom_audio();
                return false;
            }
            g_is_random_mode = false;
            printf("[custom audio] Found Live Soundtrack replacement for track #%d (%s): %s\n",
                   track_id, k_track_names[name_idx], target_file.c_str());
            fflush(stdout);
        } else {
            stop_custom_audio();
            return false;
        }

        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (!is_custom && !g_is_random_mode) {
            if (g_is_playing && g_current_track_id == track_id && g_current_track.path == target_file && g_current_track.is_loaded) {
                return true; // Keep playing current track smoothly
            }
        }

        // Reset preloader for new track
        g_next_track_ready.store(false);
        g_next_track.samples.clear();
        g_next_track.is_loaded = false;

        if (g_current_track.path != target_file || !g_current_track.is_loaded) {
            g_current_track.samples.clear();
            g_current_track.cursor = 0;
            g_current_track.is_loaded = false;
            if (load_audio_file(target_file, g_current_track.samples)) {
                g_current_track.path = target_file;
                g_current_track.track_id = track_id;
                g_current_track.is_loaded = true;
                g_current_track.cursor = 0;
                printf("[custom audio] Loaded %s (%.1f sec): %s\n",
                       (g_is_random_mode ? "Random Mode (rotating)" : (is_custom ? "Custom Track (looping)" : "Single Track (looping)")),
                       (float)g_current_track.samples.size() / 96000.0f,
                       target_file.c_str());
                fflush(stdout);
            } else {
                g_is_playing = false;
                g_current_track_id = -1;
                return is_custom;
            }
        } else {
            g_current_track.cursor = 0;
        }

        g_current_track_id = track_id;
        g_is_playing = true;

        if (g_is_random_mode) {
            request_preload();
        }

        return true;
    }

    void stop_custom_audio() {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_is_playing = false;
        g_current_track_id = -1;
        g_is_random_mode = false;
        g_next_track_ready.store(false);
        g_next_track.samples.clear();
        g_staged_track_path.clear();
    }

    void reset_match_track() {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_current_track_id = -1;
        g_staged_track_path.clear();
    }

    void mix_custom_samples(float* buffer, size_t frame_count) {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (!g_is_playing || !g_current_track.is_loaded || g_current_track.samples.empty()) {
            return;
        }

        size_t total_samples = g_current_track.samples.size();
        float vol = g_custom_volume.load();

        for (size_t i = 0; i < frame_count * 2; ++i) {
            float custom_sample = g_current_track.samples[g_current_track.cursor] * vol;
            g_current_track.cursor++;
            if (g_current_track.cursor >= total_samples) {
                // Track finished!
                if (g_is_random_mode && g_next_track_ready.load() && g_next_track.is_loaded) {
                    // Seamlessly rotate to next preloaded track ONLY in Random mode
                    g_current_track = std::move(g_next_track);
                    g_current_track_id = g_current_track.track_id;
                    g_current_track.cursor = 0;
                    total_samples = g_current_track.samples.size();
                    g_next_track_ready.store(false);

                    request_preload();

                    printf("[custom audio] Seamlessly rotated to next track: %s\n", g_current_track.path.c_str());
                    fflush(stdout);
                } else {
                    // In CHOOSE mode (including Custom and soundtrack tracks), loop the chosen track seamlessly
                    g_current_track.cursor = 0;
                }
            }

            float mixed = buffer[i] + custom_sample;
            if (mixed > 1.0f) mixed = 1.0f;
            else if (mixed < -1.0f) mixed = -1.0f;
            buffer[i] = mixed;
        }
    }

    void set_custom_volume(float vol) {
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        g_custom_volume.store(vol);
    }
}

// Hook called when the game triggers a music track
extern "C" int tnt_on_play_music(uint8_t* rdram, void* ctx_ptr) {
    recomp_context* ctx = static_cast<recomp_context*>(ctx_ptr);
    if (!ctx) return 0;
    int track_id = static_cast<int>(ctx->r4);
    return tnt::audio::play_track(rdram, track_id) ? 1 : 0;
}

// Hook called when the game selects a random BGM (in RANDOM mode or AUTO mode)
extern "C" int tnt_on_pick_random_bgm(uint8_t* /*rdram*/) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_is_random_mode = true;

    TrackEntry entry = pick_random_track(g_current_track.path);
    if (entry.path.empty()) {
        return 1;
    }

    g_staged_track_path = entry.path;
    printf("[custom audio] Random BGM rolled track #%d (%s): %s\n",
           entry.track_id, entry.is_custom ? "Custom" : "Soundtrack", entry.path.c_str());
    fflush(stdout);
    return entry.track_id;
}

// Hook to check if custom music is actively playing
extern "C" int tnt_is_custom_music_playing(void) {
    return tnt::audio::is_custom_playing() ? 1 : 0;
}

// Hook called when the game sets music volume (via func_800875EC)
extern "C" void tnt_on_set_music_volume(uint32_t vol) {
    float f_vol = static_cast<float>(vol & 0xFFFF) / 32767.0f;
    tnt::audio::set_custom_volume(f_vol);
}
