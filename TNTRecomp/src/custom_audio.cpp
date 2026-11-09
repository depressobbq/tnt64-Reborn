#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "custom_audio.hpp"
#include "recomp.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
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

            if (stem.find(name) != std::string::npos) {
                return entry.path().string();
            }
        }
        return "";
    }

    std::vector<TrackEntry> get_all_available_tracks() {
        std::vector<TrackEntry> list;
        for (int i = 0; i < 16; ++i) {
            std::string p = find_soundtrack_file(i);
            if (!p.empty()) {
                list.push_back({ p, i + 1, k_track_names[i], false });
            }
        }
        std::vector<std::string> custom_files = get_custom_track_files();
        for (const auto& file : custom_files) {
            std::string stem = fs::path(file).stem().string();
            list.push_back({ file, 17, stem, true });
        }
        return list;
    }

    TrackEntry pick_random_track(const std::string& exclude_path) {
        auto all = get_all_available_tracks();
        if (all.empty()) return {};

        std::vector<TrackEntry> pool;
        for (const auto& t : all) {
            if (all.size() > 1 && t.path == exclude_path) continue;
            pool.push_back(t);
        }
        if (pool.empty()) pool = all;

        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
        return pool[dist(gen)];
    }

    // High-quality cubic Hermite resampler converting any sample rate and channel layout to 48000 Hz stereo.
    // Avoids buggy SDL2 float resamplers on Windows/Wine that cause periodic 10^32 corruption spikes.
    void resample_to_48k_stereo(const float* in, size_t in_frames, unsigned int channels, unsigned int in_rate, std::vector<float>& out_samples_48k) {
        if (!in || in_frames == 0 || channels == 0) {
            out_samples_48k.clear();
            return;
        }

        constexpr uint32_t target_rate = 48000;
        if (in_rate == target_rate) {
            out_samples_48k.resize(in_frames * 2);
            if (channels == 1) {
                for (size_t i = 0; i < in_frames; ++i) {
                    float s = in[i];
                    out_samples_48k[i * 2 + 0] = s;
                    out_samples_48k[i * 2 + 1] = s;
                }
            } else {
                for (size_t i = 0; i < in_frames; ++i) {
                    out_samples_48k[i * 2 + 0] = in[i * channels + 0];
                    out_samples_48k[i * 2 + 1] = in[i * channels + 1];
                }
            }
            return;
        }

        // Resample with Catmull-Rom cubic Hermite spline interpolation
        double ratio = static_cast<double>(in_rate) / static_cast<double>(target_rate);
        size_t n_out = static_cast<size_t>(std::ceil(static_cast<double>(in_frames) / ratio));
        out_samples_48k.resize(n_out * 2);

        for (size_t i = 0; i < n_out; ++i) {
            double in_pos = i * ratio;
            int64_t idx = static_cast<int64_t>(std::floor(in_pos));
            float frac = static_cast<float>(in_pos - idx);

            int64_t i0 = (idx > 0) ? (idx - 1) : 0;
            int64_t i1 = (idx < static_cast<int64_t>(in_frames)) ? idx : (static_cast<int64_t>(in_frames) - 1);
            int64_t i2 = ((idx + 1) < static_cast<int64_t>(in_frames)) ? (idx + 1) : (static_cast<int64_t>(in_frames) - 1);
            int64_t i3 = ((idx + 2) < static_cast<int64_t>(in_frames)) ? (idx + 2) : (static_cast<int64_t>(in_frames) - 1);

            for (int ch = 0; ch < 2; ++ch) {
                int src_ch = (channels == 1) ? 0 : ch;
                float p0 = in[i0 * channels + src_ch];
                float p1 = in[i1 * channels + src_ch];
                float p2 = in[i2 * channels + src_ch];
                float p3 = in[i3 * channels + src_ch];

                float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
                float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
                float c = -0.5f * p0 + 0.5f * p2;
                float d = p1;

                float sample = ((a * frac + b) * frac + c) * frac + d;
                if (sample > 1.0f) sample = 1.0f;
                else if (sample < -1.0f) sample = -1.0f;

                out_samples_48k[i * 2 + ch] = sample;
            }
        }
    }

    bool decode_audio_file(const std::string& filepath, std::vector<float>& out_samples_48k) {
        out_samples_48k.clear();
        std::string ext = to_lower(fs::path(filepath).extension().string());

        if (ext == ".mp3") {
            drmp3_config cfg;
            drmp3_uint64 total_frames = 0;
            float* pSampleData = drmp3_open_file_and_read_pcm_frames_f32(
                filepath.c_str(), &cfg, &total_frames, nullptr);
            if (!pSampleData) {
                printf("[custom audio] Failed to decode MP3: %s\n", filepath.c_str());
                return false;
            }

            resample_to_48k_stereo(pSampleData, total_frames, cfg.channels, cfg.sampleRate, out_samples_48k);
            drmp3_free(pSampleData, nullptr);
            return true;
        } else if (ext == ".wav") {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            drwav_uint64 total_frames = 0;
            float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(
                filepath.c_str(), &channels, &sampleRate, &total_frames, nullptr);
            if (!pSampleData) {
                printf("[custom audio] Failed to decode WAV: %s\n", filepath.c_str());
                return false;
            }

            resample_to_48k_stereo(pSampleData, total_frames, channels, sampleRate, out_samples_48k);
            drwav_free(pSampleData, nullptr);
            return true;
        }

        return false;
    }

    void preload_worker() {
        while (!g_shutdown.load()) {
            std::unique_lock<std::mutex> lock(g_preload_mutex);
            g_preload_cv.wait(lock, [] {
                return g_shutdown.load() || g_preload_requested.load();
            });

            if (g_shutdown.load()) break;
            g_preload_requested.store(false);
            lock.unlock();

            std::string curr_path;
            {
                std::lock_guard<std::mutex> alock(g_audio_mutex);
                curr_path = g_current_track.path;
            }

            TrackEntry next_entry = pick_random_track(curr_path);
            if (!next_entry.path.empty()) {
                AudioBuffer buf;
                buf.path = next_entry.path;
                buf.track_id = next_entry.track_id;
                buf.cursor = 0;
                if (decode_audio_file(next_entry.path, buf.samples)) {
                    buf.is_loaded = true;
                    std::lock_guard<std::mutex> alock(g_audio_mutex);
                    g_next_track = std::move(buf);
                    g_next_track_ready.store(true);
                    printf("[custom audio] Preloaded next random track: %s\n", next_entry.path.c_str());
                    fflush(stdout);
                }
            }
        }
    }

    void request_preload() {
        if (!g_is_random_mode) return;
        g_preload_requested.store(true);
        g_preload_cv.notify_one();
    }
}

namespace tnt::audio {
    void init_custom_audio() {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (!g_preload_thread.joinable()) {
            g_preload_thread = std::thread(preload_worker);
            std::atexit(tnt::audio::shutdown_custom_audio);
        }
    }

    void shutdown_custom_audio() {
        static std::atomic<bool> s_has_shutdown{ false };
        if (s_has_shutdown.exchange(true)) return;

        g_shutdown.store(true);
        g_preload_cv.notify_all();
        if (g_preload_thread.joinable()) {
            g_preload_thread.join();
        }
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_is_playing = false;
        g_current_track.samples.clear();
        g_current_track.is_loaded = false;
        g_next_track.samples.clear();
        g_next_track.is_loaded = false;
    }

    bool is_custom_playing() {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        return g_is_playing;
    }

    bool play_track(uint8_t* rdram, int track_id) {
        init_custom_audio();

        std::string chosen_file;
        int resolved_id = track_id;

        if (track_id == 0) {
            stop_custom_audio();
            return false;
        }

        if (g_is_random_mode && !g_staged_track_path.empty()) {
            chosen_file = g_staged_track_path;
            g_staged_track_path.clear();
        } else {
            g_is_random_mode = false;
        }

        if (chosen_file.empty()) {
            if (track_id == 17) {
                auto custom_files = get_custom_track_files();
                if (!custom_files.empty()) {
                    chosen_file = custom_files[0];
                }
            } else if (track_id >= 1 && track_id <= 16) {
                chosen_file = find_soundtrack_file(track_id - 1);
            }
        }

        if (chosen_file.empty()) {
            stop_custom_audio();
            return false;
        }

        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (g_current_track_id == resolved_id && g_current_track.path == chosen_file && g_is_playing) {
            return true;
        }

        if (g_next_track_ready.load() && g_next_track.path == chosen_file && g_next_track.is_loaded) {
            g_current_track = std::move(g_next_track);
            g_current_track_id = resolved_id;
            g_current_track.cursor = 0;
            g_is_playing = true;
            g_next_track_ready.store(false);
            printf("[custom audio] Instantly swapped preloaded track: %s\n", chosen_file.c_str());
            fflush(stdout);

            request_preload();
            return true;
        }

        std::vector<float> samples;
        if (!decode_audio_file(chosen_file, samples)) {
            stop_custom_audio();
            return false;
        }

        g_current_track.path = chosen_file;
        g_current_track.track_id = resolved_id;
        g_current_track.samples = std::move(samples);
        g_current_track.cursor = 0;
        g_current_track.is_loaded = true;
        g_current_track_id = resolved_id;
        g_is_playing = true;

        printf("[custom audio] Playing track #%d: %s (%zu samples @ 48kHz)\n",
               resolved_id, chosen_file.c_str(), g_current_track.samples.size() / 2);
        fflush(stdout);

        request_preload();
        return true;
    }

    void stop_custom_audio() {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_is_playing = false;
        g_current_track_id = -1;
        g_current_track.cursor = 0;
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

// Handler called when the game triggers a music track
extern "C" int tnt_on_play_music(uint8_t* rdram, void* ctx_ptr) {
    recomp_context* ctx = static_cast<recomp_context*>(ctx_ptr);
    if (!ctx) return 0;
    int track_id = static_cast<int>(ctx->r4);
    return tnt::audio::play_track(rdram, track_id) ? 1 : 0;
}

// Handler called when the game selects a random BGM (in RANDOM mode or AUTO mode)
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

// Query whether custom music is actively playing
extern "C" int tnt_is_custom_music_playing(void) {
    return tnt::audio::is_custom_playing() ? 1 : 0;
}

// Handler called when the game sets music volume (via func_800875EC)
extern "C" void tnt_on_set_music_volume(uint32_t vol) {
    float f_vol = static_cast<float>(vol & 0xFFFF) / 32767.0f;
    tnt::audio::set_custom_volume(f_vol);
}
