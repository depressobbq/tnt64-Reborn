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
#endif

namespace {
    std::atomic<uint32_t> g_active_seed{ 0 };
    std::atomic<uint32_t> g_last_true_lines{ 0 };
    std::atomic<uint32_t> g_last_blocks_made{ 0 };
    std::atomic<uint32_t> g_live_cubes_made{ 0 };
    std::atomic<bool> g_is_in_match{ false };
    std::atomic<bool> g_options_menu_open{ false };

    constexpr uint32_t SEED_ADDR = 0x80129340;
    constexpr uint32_t FLOAT_SEED_ADDR = 0x80129358;
    constexpr uint32_t SCENE_ADDR = 0x800CFEE8;
    constexpr uint32_t GAME_FRAMES_ADDR = 0x8011EEE8;
    constexpr uint32_t PLAYER_PTR_TABLE = 0x8011EB2C;
    constexpr uint32_t PLAYER0_FALLBACK = 0x8011EB3C;
    constexpr uint32_t CONTROLLER_MGR_PTR = 0x800E1F18;

    // RDRAM endianness-aware access helpers for N64 little-endian host mapping
    inline uint8_t rd8(const uint8_t* rdram, uint32_t a) {
        return rdram[(a ^ 3) & 0x7FFFFF];
    }
    inline void wr8(uint8_t* rdram, uint32_t a, uint8_t val) {
        rdram[(a ^ 3) & 0x7FFFFF] = val;
    }
    inline uint16_t rd16(const uint8_t* rdram, uint32_t a) {
        return *reinterpret_cast<const uint16_t*>(rdram + ((a ^ 2) & 0x7FFFFE));
    }
    inline uint32_t rd32(const uint8_t* rdram, uint32_t a) {
        return *reinterpret_cast<const uint32_t*>(rdram + (a & 0x7FFFFC));
    }
    inline void wr32(uint8_t* rdram, uint32_t a, uint32_t val) {
        *reinterpret_cast<uint32_t*>(rdram + (a & 0x7FFFFC)) = val;
    }
    inline void wr_string(uint8_t* rdram, uint32_t a, const char* str) {
        size_t len = strlen(str);
        for (size_t i = 0; i <= len; i++) {
            rdram[((a + i) ^ 3) & 0x7FFFFF] = static_cast<uint8_t>(str[i]);
        }
    }

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

    struct TournamentMenuInternalState {
        int selected_row = 0;
        uint32_t current_seed = 0x12345678;
        bool seeded_run = false;
        bool hud_enabled = true;
        bool discord_enabled = false;
        bool initialized = false;
    };
    TournamentMenuInternalState g_menu_state;

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

namespace tnt::tournament {
    uint32_t read_live_seed(uint8_t* rdram) {
        if (!rdram) return g_active_seed.load();
        return rd32(rdram, SEED_ADDR);
    }

    bool is_options_menu_open() {
        return g_options_menu_open.load();
    }

    MenuRenderState get_menu_render_state() {
        MenuRenderState s;
        s.selected_row = g_menu_state.selected_row;
        s.seeded_run = g_menu_state.seeded_run;
        s.current_seed = g_menu_state.current_seed;
        s.hud_enabled = g_menu_state.hud_enabled;
        s.discord_enabled = g_menu_state.discord_enabled;
        return s;
    }
}

extern "C" void tnt_apply_menu_patches(uint8_t* rdram) {
    if (!rdram) return;

    // Options menu item #4 pointer in RDRAM (VRAM 0x800D5518)
    constexpr uint32_t credits_ptr_phys = 0x000D5518;
    constexpr uint32_t tourney_label_vram = 0x807E0040;

    if (rd32(rdram, credits_ptr_phys) != tourney_label_vram) {
        wr_string(rdram, tourney_label_vram, "TOURNAMENT");
        wr32(rdram, credits_ptr_phys, tourney_label_vram);

        // Also patch in-place at original CREDITS string (VRAM 0x800E0724)
        wr_string(rdram, 0x800E0724, "TOURNEY");

        printf("[Tournament Edition] Options menu patched: CREDITS -> TOURNAMENT\n");
    }
}

extern "C" void tnt_apply_seed_override(uint8_t* rdram, recomp_context* ctx) {
    if (!rdram) return;
    (void)ctx;
    const auto& s = tnt::settings();
    uint32_t seed = 0;

    // Reset match live counters on match start
    g_live_cubes_made.store(0);
    g_is_in_match.store(true);

    if (s.seeded_run) {
        seed = parse_seed_string(s.custom_seed);
        if (seed == 0) seed = 0x12345678;

        // Apply integer seed to all player slots (0..3)
        for (int p = 0; p < 4; ++p) {
            wr32(rdram, SEED_ADDR + (p * 4), seed);
        }

        // Apply float seed table at 0x80129358 for all player slots (0..3)
        float f_val = static_cast<float>(seed % 100001u);
        uint32_t f_u32 = 0;
        memcpy(&f_u32, &f_val, sizeof(float));
        for (int p = 0; p < 4; ++p) {
            wr32(rdram, FLOAT_SEED_ADDR + (p * 4), f_u32);
        }

        printf("[Tournament Edition] Match Seed Applied: 0x%08X (%u)\n", seed, seed);
    }

    // Read live PRNG seed back directly from RDRAM to verify
    seed = tnt::tournament::read_live_seed(rdram);
    g_active_seed.store(seed);
}

extern "C" void tnt_on_cube_formed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; (void)ctx;
    g_live_cubes_made++;
}

extern "C" int tnt_handle_tournament_options(uint8_t* rdram, recomp_context* ctx) {
    if (!rdram) return 0;
    (void)ctx;

    // Check active scene byte (0x800CFEE8)
    uint8_t scene = rd8(rdram, SCENE_ADDR);
    if (scene != 16) {
        g_options_menu_open.store(false);
        return 0; // Not in credits/tournament scene
    }

    // Check credits sub-state (0x800D3330)
    uint32_t state = rd32(rdram, 0x800D3330);
    if (state == 0) {
        // Frame 0 of scene 16: let original initialization run so structures are ready
        return 0;
    }
    if (state == 2) {
        // Exit / teardown in progress: let original teardown run to cleanly return to scene 4
        g_options_menu_open.store(false);
        g_menu_state.initialized = false;
        return 0;
    }

    // State 1: In Tournament Options menu
    g_options_menu_open.store(true);

    if (!g_menu_state.initialized) {
        const auto& s = tnt::settings();
        g_menu_state.seeded_run = s.seeded_run;
        g_menu_state.current_seed = parse_seed_string(s.custom_seed);
        if (g_menu_state.current_seed == 0) g_menu_state.current_seed = 0x12345678;
        g_menu_state.hud_enabled = s.tournament_hud;
        g_menu_state.discord_enabled = s.enable_discord;
        g_menu_state.selected_row = 0;
        g_menu_state.initialized = true;
    }

    // Read controller 0 input from N64 controller buffer
    uint32_t p = rd32(rdram, CONTROLLER_MGR_PTR);
    uint16_t buttons = 0;
    int8_t stick_x = 0, stick_y = 0;
    if ((p >> 24) == 0x80) {
        uint32_t pad = p + 0x2372;
        buttons = rd16(rdram, pad);
        stick_x = static_cast<int8_t>(rd8(rdram, pad + 2));
        stick_y = static_cast<int8_t>(rd8(rdram, pad + 3));
    }

    // Map analogue stick to D-pad
    if (stick_y > 40) buttons |= 0x0800;  // Up
    if (stick_y < -40) buttons |= 0x0400; // Down
    if (stick_x < -40) buttons |= 0x0200; // Left
    if (stick_x > 40) buttons |= 0x0100;  // Right

    static uint16_t s_last_buttons = 0;
    static int s_repeat_timer = 0;
    uint16_t pressed = buttons & ~s_last_buttons;

    if (buttons != 0 && buttons == s_last_buttons) {
        s_repeat_timer++;
        if (s_repeat_timer > 20 && (s_repeat_timer % 6 == 0)) {
            pressed = buttons;
        }
    } else {
        s_repeat_timer = 0;
    }
    s_last_buttons = buttons;

    constexpr int NUM_ROWS = 6;
    if (pressed & 0x0800) { // Up
        g_menu_state.selected_row = (g_menu_state.selected_row + NUM_ROWS - 1) % NUM_ROWS;
    }
    if (pressed & 0x0400) { // Down
        g_menu_state.selected_row = (g_menu_state.selected_row + 1) % NUM_ROWS;
    }

    bool exit_menu = false;

    switch (g_menu_state.selected_row) {
        case 0: // Seeded Run
            if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                g_menu_state.seeded_run = !g_menu_state.seeded_run;
            }
            break;
        case 1: // Custom Seed Adjust
            if (pressed & 0x0100) g_menu_state.current_seed++;
            if (pressed & 0x0200) g_menu_state.current_seed--;
            if (pressed & 0x0010) g_menu_state.current_seed += 0x1000; // R
            if (pressed & 0x0020) g_menu_state.current_seed -= 0x1000; // L
            if (pressed & 0x8000) { // A: Randomize
                g_menu_state.current_seed = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
            }
            break;
        case 2: // Sidebar HUD
            if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                g_menu_state.hud_enabled = !g_menu_state.hud_enabled;
            }
            break;
        case 3: // Discord Share
            if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                g_menu_state.discord_enabled = !g_menu_state.discord_enabled;
            }
            break;
        case 4: // Randomize Seed
            if (pressed & (0x8000 | 0x0200 | 0x0100)) {
                g_menu_state.current_seed = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
            }
            break;
        case 5: // Save & Return
            if (pressed & (0x8000 | 0x1000)) {
                exit_menu = true;
            }
            break;
    }

    // B button or Start exits and saves
    if (pressed & (0x4000 | 0x1000)) {
        exit_menu = true;
    }

    if (exit_menu) {
        auto& s = tnt::settings();
        s.seeded_run = g_menu_state.seeded_run;
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08X", g_menu_state.current_seed);
        s.custom_seed = buf;
        s.tournament_hud = g_menu_state.hud_enabled;
        s.enable_discord = g_menu_state.discord_enabled;

        tnt::save_settings(tnt::settings_path(), s);
        printf("[Tournament Options] Saved settings: Seeded=%d Seed=%s HUD=%d Discord=%d\n",
               s.seeded_run, s.custom_seed.c_str(), s.tournament_hud, s.enable_discord);

        g_options_menu_open.store(false);
        g_menu_state.initialized = false;

        // Transition back to Options Menu by triggering the game's native Scene 16 teardown (sub-state 2)
        wr32(rdram, 0x800D3330, 2);
        return 0; // Return 0 so func_80083470 executes L_80083504 and transitions back cleanly to Scene 4!
    }

    return 1; // Suppress original credits scrolling and drawing
}

namespace tnt::tournament {

    LiveStats get_live_stats(uint8_t* rdram, uint64_t frame) {
        LiveStats stats;
        stats.active_seed = g_active_seed.load();

        if (!rdram) return stats;

        stats.live_seed = read_live_seed(rdram);

        // Active scene byte: 9 = Single-player match, 12 (0xC) = Marathon match, 14 (0xE) = Multiplayer
        uint8_t scene = rd8(rdram, SCENE_ADDR);
        bool is_game_scene = (scene == 9 || scene == 12 || scene == 14);

        if (!is_game_scene || !g_is_in_match.load()) {
            stats.in_game = false;
            return stats;
        }

        uint32_t frames = rd32(rdram, GAME_FRAMES_ADDR);
        if (frames == 0 || frames > 60u * 60 * 60 * 24) {
            stats.in_game = false;
            return stats;
        }

        // Liveness check: verify game frame counter is actively advancing
        static uint32_t s_prev_frames = 0;
        static uint64_t s_last_advance_frame = 0;
        if (frames != s_prev_frames) {
            s_prev_frames = frames;
            s_last_advance_frame = frame;
        } else if (frame > s_last_advance_frame + 60) {
            // Frames stopped advancing for > 1.0s (Game Over, Quit to Menu, or Paused)
            stats.in_game = false;
            return stats;
        }

        stats.in_game = true;
        stats.game_seconds = frames / 60;

        // Locate Player 0 object in RDRAM
        uint32_t p0_ptr = rd32(rdram, PLAYER_PTR_TABLE);
        uint32_t p0_vram = ((p0_ptr >> 24) == 0x80) ? p0_ptr : PLAYER0_FALLBACK;

        // Live physical lines actually cleared ("True Lines")
        stats.true_lines = rd32(rdram, p0_vram + 0x440);

        // Live score lines (including tetris & cube bonus lines)
        uint32_t s1 = rd32(rdram, p0_vram + 0x6820);
        uint32_t s2 = rd32(rdram, p0_vram + 0x6824);
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
        g_is_in_match.store(false);
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
