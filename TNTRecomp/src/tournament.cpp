#include "tournament.hpp"
#include "settings.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace {
    std::atomic<uint32_t> g_active_seed{ 0 };
    std::atomic<uint32_t> g_last_true_lines{ 0 };
    std::atomic<uint32_t> g_last_blocks_made{ 0 };

    constexpr uint32_t SEED_ADDR = 0x80129340;
    constexpr uint32_t GAME_FRAMES_ADDR = 0x8011EEE8;

    uint32_t parse_seed_string(const std::string& s) {
        if (s.empty()) return 0;
        try {
            if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
                return (uint32_t)std::stoul(s, nullptr, 16);
            }
            return (uint32_t)std::stoul(s);
        } catch (...) {
            return 0;
        }
    }

    void send_discord_webhook_post(const std::string& webhook_url, const std::string& json_payload) {
        // Write payload to a temporary file to avoid shell escaping issues
        char tmp_path[512] = {};
#if defined(_WIN32)
        char tmp_dir[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp_dir);
        snprintf(tmp_path, sizeof(tmp_path), "%stnt_score_%u.json", tmp_dir, (unsigned)rand());
#else
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/tnt_score_%u.json", (unsigned)rand());
#endif
        FILE* fp = fopen(tmp_path, "wb");
        if (fp) {
            fwrite(json_payload.data(), 1, json_payload.size(), fp);
            fclose(fp);

            // Execute curl in silent mode
            std::string cmd = "curl -s -X POST -H \"Content-Type: application/json\" --data-binary \"@" + std::string(tmp_path) + "\" \"" + webhook_url + "\"";
            int ret = std::system(cmd.c_str());
            (void)ret;

            std::remove(tmp_path);
        }
    }
}

extern "C" void tnt_apply_seed_override(uint8_t* rdram, recomp_context* ctx) {
    const auto& s = tnt::settings();
    uint32_t seed = 0;

    if (s.seeded_run) {
        seed = parse_seed_string(s.custom_seed);
        g_active_seed.store(seed);

        // 0x80129340 is the game's PRNG seed in RDRAM
        uint32_t phys_offset = (SEED_ADDR & 0x7FFFFC);
        *reinterpret_cast<uint32_t*>(rdram + phys_offset) = seed;

        // Propagate to all 4 player slots
        for (int p = 1; p < 4; ++p) {
            *reinterpret_cast<uint32_t*>(rdram + phys_offset + (p * 4)) = seed;
        }
        printf("[Tournament Edition] Active Seed Override Applied: 0x%08X (%u)\n", seed, seed);
    } else {
        // Store the hardware-generated seed for reporting
        uint32_t phys_offset = (SEED_ADDR & 0x7FFFFC);
        seed = *reinterpret_cast<uint32_t*>(rdram + phys_offset);
        g_active_seed.store(seed);
    }
}

namespace tnt::tournament {

    LiveStats get_live_stats(uint8_t* rdram, uint64_t frame) {
        LiveStats stats;
        stats.active_seed = g_active_seed.load();

        if (!rdram) return stats;

        uint32_t frames = *reinterpret_cast<uint32_t*>(rdram + (GAME_FRAMES_ADDR & 0x7FFFFC));
        if (frames == 0 || frames > 60u * 60 * 60 * 24) {
            stats.in_game = false;
            return stats;
        }

        stats.in_game = true;
        stats.game_seconds = frames / 60;

        // Player 0 stats from RDRAM
        // In TNT, player 0 active state block is at 0x8011EB3C
        stats.true_lines = g_last_true_lines.load();
        stats.blocks_made = g_last_blocks_made.load();

        if (stats.game_seconds > 0) {
            stats.lpm = (static_cast<float>(stats.true_lines) / (static_cast<float>(stats.game_seconds) / 60.0f));
        }

        return stats;
    }

    void on_match_finish(int mode, const std::string& name, uint32_t score, uint32_t true_lines, uint32_t seconds) {
        g_last_true_lines.store(true_lines);
        uint32_t cubes = (score > true_lines) ? ((score - true_lines) / 10) : 0;
        g_last_blocks_made.store(cubes);

        const auto& s = tnt::settings();
        if (!s.enable_discord || s.discord_webhook.empty()) return;

        float lpm = (seconds > 0) ? (static_cast<float>(true_lines) / (static_cast<float>(seconds) / 60.0f)) : 0.0f;
        uint32_t seed = g_active_seed.load();

        const char* mode_name = (mode == 0) ? "Marathon" : (mode == 1 ? "Sprint" : "Ultra");

        std::ostringstream json;
        json << "{"
             << "\"username\":\"The New Tetris - Tournament Edition\","
             << "\"embeds\":[{"
             << "\"title\":\"🏆 Match Result - " << mode_name << "\","
             << "\"color\":16763904," // Gold hex color
             << "\"fields\":["
             << "{\"name\":\"Player\",\"value\":\"" << name << "\",\"inline\":true},"
             << "{\"name\":\"Time\",\"value\":\"" << (seconds / 60) << ":" << (seconds % 60 < 10 ? "0" : "") << (seconds % 60) << "\",\"inline\":true},"
             << "{\"name\":\"Score Lines\",\"value\":\"" << score << "\",\"inline\":true},"
             << "{\"name\":\"True Lines\",\"value\":\"" << true_lines << "\",\"inline\":true},"
             << "{\"name\":\"LPM\",\"value\":\"" << std::fixed << lpm << "\",\"inline\":true},"
             << "{\"name\":\"Cubes/Blocks\",\"value\":\"" << cubes << "\",\"inline\":true},"
             << "{\"name\":\"Seed\",\"value\":\"`0x" << std::uppercase << std::hex << seed << "`" << (s.seeded_run ? " (Seeded)" : " (Random)") << "\",\"inline\":false}"
             << "]"
             << "}]"
             << "}";

        std::string webhook = s.discord_webhook;
        std::string payload = json.str();

        // Launch in background detached thread so gameplay never hitches
        std::thread([webhook, payload]() {
            send_discord_webhook_post(webhook, payload);
        }).detach();
    }
}
