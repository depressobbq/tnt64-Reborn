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
    std::atomic<uint32_t> g_live_cubes_made{ 0 };

    constexpr uint32_t SEED_ADDR = 0x80129340;
    constexpr uint32_t GAME_FRAMES_ADDR = 0x8011EEE8;
    constexpr uint32_t PLAYER_PTR_TABLE = 0x8011EB2C;
    constexpr uint32_t PLAYER0_FALLBACK = 0x8011EB3C;

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

    // Reset match live counters on match start
    g_live_cubes_made.store(0);

    if (s.seeded_run) {
        seed = parse_seed_string(s.custom_seed);
        g_active_seed.store(seed);

        uint32_t phys_offset = (SEED_ADDR & 0x7FFFFC);
        *reinterpret_cast<uint32_t*>(rdram + phys_offset) = seed;

        for (int p = 1; p < 4; ++p) {
            *reinterpret_cast<uint32_t*>(rdram + phys_offset + (p * 4)) = seed;
        }
        if (ctx) {
            ctx->r3 = seed;
        }
        printf("[Tournament Edition] Match Seed Override Applied: 0x%08X (%u)\n", seed, seed);
    } else {
        uint32_t phys_offset = (SEED_ADDR & 0x7FFFFC);
        seed = *reinterpret_cast<uint32_t*>(rdram + phys_offset);
        g_active_seed.store(seed);
    }
}

extern "C" void tnt_on_cube_formed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; (void)ctx;
    g_live_cubes_made++;
    printf("[Tournament Edition] Cube formed! Total cubes: %u\n", g_live_cubes_made.load());
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

        // Locate Player 0 object in RDRAM
        uint32_t p0_ptr = *reinterpret_cast<uint32_t*>(rdram + (PLAYER_PTR_TABLE & 0x7FFFFC));
        uint32_t p0_phys = 0;
        if ((p0_ptr >> 24) == 0x80) {
            p0_phys = p0_ptr & 0x7FFFFC;
        } else {
            p0_phys = (PLAYER0_FALLBACK & 0x7FFFFC);
        }

        // Live physical lines actually cleared ("True Lines")
        stats.true_lines = *reinterpret_cast<uint32_t*>(rdram + p0_phys + 0x440);

        // Live score lines (including tetris & cube bonus lines)
        uint32_t s1 = *reinterpret_cast<uint32_t*>(rdram + p0_phys + 0x6820);
        uint32_t s2 = *reinterpret_cast<uint32_t*>(rdram + p0_phys + 0x6824);
        stats.total_score = s1 | s2;

        // Blocks / Cubes made: combine formed cubes and score difference
        uint32_t cubes_from_score = (stats.total_score > stats.true_lines) ? ((stats.total_score - stats.true_lines) / 10) : 0;
        stats.blocks_made = std::max<uint32_t>(g_live_cubes_made.load(), cubes_from_score);

        if (stats.game_seconds > 0) {
            stats.lpm = (static_cast<float>(stats.true_lines) / (static_cast<float>(stats.game_seconds) / 60.0f));
        }

        return stats;
    }

    void on_match_finish(int mode, const std::string& name, uint32_t score, uint32_t true_lines, uint32_t seconds) {
        g_last_true_lines.store(true_lines);
        uint32_t cubes = (score > true_lines) ? ((score - true_lines) / 10) : 0;
        cubes = std::max<uint32_t>(cubes, g_live_cubes_made.load());
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

        std::thread([webhook, payload]() {
            send_discord_webhook_post(webhook, payload);
        }).detach();
    }
}
