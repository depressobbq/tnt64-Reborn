#include "tournament.hpp"
#include "settings.hpp"
#include "custom_audio.hpp"
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
    std::atomic<uint32_t> g_match_initial_seed{ 0x12345678 };
    std::atomic<uint32_t> g_bag_counter{ 0 };
    std::atomic<uint32_t> g_last_true_lines{ 0 };
    std::atomic<uint32_t> g_last_blocks_made{ 0 };
    std::atomic<uint32_t> g_live_cubes_made{ 0 };
    std::atomic<uint32_t> g_live_gold_cubes{ 0 };
    std::atomic<uint32_t> g_live_silver_cubes{ 0 };
    std::atomic<uint32_t> g_live_gold_lines{ 0 };
    std::atomic<uint32_t> g_live_silver_lines{ 0 };
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
        int tournament_mode = 0; // 0: Marathon, 1: Sprint, 2: Ultra
        int players = 1;         // 1: 1 Player, 2: 2 Players
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
        if (!rdram) return g_match_initial_seed.load();
        return rd32(rdram, SEED_ADDR);
    }

    bool is_options_menu_open() {
        return g_options_menu_open.load();
    }

    MenuRenderState get_menu_render_state() {
        MenuRenderState s;
        s.selected_row = g_menu_state.selected_row;
        s.tournament_mode = g_menu_state.tournament_mode;
        s.players = g_menu_state.players;
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

        // Menu label at VRAM 0x800E0724
        wr_string(rdram, 0x800E0724, "TOURNEY");

        // CUSTOM track label for Audio Menu at 0x807E0080
        wr_string(rdram, 0x807E0080, "CUSTOM.");
    }
}

extern "C" void tnt_on_match_piece_init(uint8_t* rdram, recomp_context* ctx) {
    if (!rdram) return;
    uint32_t player = (ctx ? ctx->r4 : 0);
    if (player == 0) {
        g_live_cubes_made.store(0);
        g_live_gold_cubes.store(0);
        g_live_silver_cubes.store(0);
        g_live_gold_lines.store(0);
        g_live_silver_lines.store(0);
        tnt::audio::reset_match_track();
    }
    const auto& s = tnt::settings();
    if (!s.seeded_run) {
        return;
    }

    uint32_t seed = parse_seed_string(s.custom_seed);
    if (seed == 0) seed = 0x12345678;

    if (player == 0) {
        g_bag_counter.store(0);
        g_live_cubes_made.store(0);
        g_live_gold_cubes.store(0);
        g_live_silver_cubes.store(0);
        g_live_gold_lines.store(0);
        g_live_silver_lines.store(0);
        g_is_in_match.store(true);
        g_match_initial_seed.store(seed);
    }

    for (int p = 0; p < 4; ++p) {
        wr32(rdram, SEED_ADDR + (p * 4), seed);
    }

    float f_val = static_cast<float>(seed % 100001u);
    uint32_t f_u32 = 0;
    memcpy(&f_u32, &f_val, sizeof(float));
    for (int p = 0; p < 4; ++p) {
        wr32(rdram, FLOAT_SEED_ADDR + (p * 4), f_u32);
    }

    printf("[seed] Applied seed: 0x%08X (player %u)\n", seed, player);
}

extern "C" void tnt_on_bag_generate(uint8_t* rdram, recomp_context* ctx) {
    if (!rdram) return;
    (void)ctx;
    const auto& s = tnt::settings();
    if (!s.seeded_run) {
        return;
    }

    uint32_t base_seed = g_match_initial_seed.load();
    if (base_seed == 0) {
        base_seed = parse_seed_string(s.custom_seed);
        if (base_seed == 0) base_seed = 0x12345678;
        g_match_initial_seed.store(base_seed);
    }

    uint32_t bag_idx = g_bag_counter.fetch_add(1);

    // Predictable 64-bit LCG step for deterministic multi-bag piece sequences
    uint64_t state = (static_cast<uint64_t>(base_seed) ^ (static_cast<uint64_t>(bag_idx) * 0x5DEECE66DULL)) + 0xBULL;
    state = (state * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
    uint32_t next_seed = static_cast<uint32_t>(state >> 16);

    for (int p = 0; p < 4; ++p) {
        wr32(rdram, SEED_ADDR + (p * 4), next_seed);
    }
}

extern "C" void tnt_apply_seed_override(uint8_t* rdram, recomp_context* ctx) {
    if (!rdram) return;
    (void)ctx;
    const auto& s = tnt::settings();
    uint32_t seed = 0;

    // Reset match live counters on match start
    g_live_cubes_made.store(0);
    g_live_gold_cubes.store(0);
    g_live_silver_cubes.store(0);
    g_live_gold_lines.store(0);
    g_live_silver_lines.store(0);
    g_bag_counter.store(0);
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

        g_match_initial_seed.store(seed);
        printf("[seed] Match seed: 0x%08X\n", seed);
    } else {
        seed = tnt::tournament::read_live_seed(rdram);
        g_match_initial_seed.store(seed);
        printf("[seed] Random seed: 0x%08X\n", seed);
    }
}

extern "C" void tnt_on_match_start(uint8_t* rdram, recomp_context* ctx) {
    tnt_apply_seed_override(rdram, ctx);
}

extern "C" void tnt_on_cube_formed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) {
        uint32_t cube_type = ctx->r6 & 0xFF;
        if (cube_type == 2) {
            g_live_gold_cubes.fetch_add(1);
        } else if (cube_type == 1) {
            g_live_silver_cubes.fetch_add(1);
        }
    }
    g_live_cubes_made.fetch_add(1);
}

extern "C" void tnt_on_line_cleared(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (ctx) {
        uint32_t clear_type = ctx->r6 & 0xFF;
        if (clear_type == 4) {
            g_live_gold_lines.fetch_add(1);
        } else if (clear_type == 2) {
            g_live_silver_lines.fetch_add(1);
        } else if (clear_type == 3) {
            g_live_gold_lines.fetch_add(1);
            g_live_silver_lines.fetch_add(1);
        }
    }
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
        // Exit / teardown in progress: let original teardown run cleanly to return to Scene 4
        g_options_menu_open.store(false);
        g_menu_state.initialized = false;
        return 0;
    }

    // State 1: In Tournament Options menu
    g_options_menu_open.store(true);

    if (!g_menu_state.initialized) {
        const auto& s = tnt::settings();
        g_menu_state.tournament_mode = 0; // Marathon
        g_menu_state.players = 1;         // 1 Player
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

    // Edge detect button presses
    static uint16_t s_last_buttons = 0;
    uint16_t pressed = buttons & ~s_last_buttons;
    s_last_buttons = buttons;

    if (pressed & 0x0800) { // Up
        g_menu_state.selected_row = (g_menu_state.selected_row + 8) % 9;
    }
    if (pressed & 0x0400) { // Down
        g_menu_state.selected_row = (g_menu_state.selected_row + 1) % 9;
    }

    bool exit_menu = false;

    switch (g_menu_state.selected_row) {
        case 0: // Seeded Run toggle
            if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                g_menu_state.seeded_run = !g_menu_state.seeded_run;
            }
            break;
        case 1: // Mode: 0: Marathon, 1: Sprint, 2: Ultra
            if (pressed & 0x0100) g_menu_state.tournament_mode = (g_menu_state.tournament_mode + 1) % 3;
            if (pressed & 0x0200) g_menu_state.tournament_mode = (g_menu_state.tournament_mode + 2) % 3;
            break;
        case 2: // Players: 1 or 2
            if (pressed & (0x0100 | 0x0200)) g_menu_state.players = (g_menu_state.players == 1) ? 2 : 1;
            break;
        case 3: // Seed Hex Digit Adjust
            if (pressed & 0x0100) g_menu_state.current_seed += 0x00010000;
            if (pressed & 0x0200) g_menu_state.current_seed -= 0x00010000;
            break;
        case 4: // Randomize Seed
            if (pressed & (0x8000 | 0x0100 | 0x0200)) {
                g_menu_state.current_seed = static_cast<uint32_t>(rand()) ^ (static_cast<uint32_t>(rand()) << 16);
                if (g_menu_state.current_seed == 0) g_menu_state.current_seed = 0x12345678;
            }
            break;
        case 5: // HUD Toggle
            if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                g_menu_state.hud_enabled = !g_menu_state.hud_enabled;
            }
            break;
        case 6: // Discord Webhook Toggle
            if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                g_menu_state.discord_enabled = !g_menu_state.discord_enabled;
            }
            break;
        case 7: // Start Match
            if (pressed & (0x8000 | 0x1000)) {
                exit_menu = true;
            }
            break;
        case 8: // Exit Game
            if (pressed & (0x8000 | 0x1000)) {
                printf("[game] Exiting...\n");
                tnt::audio::shutdown_custom_audio();
                std::exit(0);
            }
            break;
    }

    if (pressed & 0x4000) { // B Button: Return / Back
        exit_menu = true;
    }

    if (exit_menu) {
        auto s = tnt::settings();
        s.seeded_run = g_menu_state.seeded_run;
        char hex_buf[16];
        snprintf(hex_buf, sizeof(hex_buf), "0x%08X", g_menu_state.current_seed);
        s.custom_seed = hex_buf;
        s.tournament_hud = g_menu_state.hud_enabled;
        s.enable_discord = g_menu_state.discord_enabled;

        tnt::save_settings(tnt::settings_path(), s);
        printf("[settings] Saved configuration: Seeded=%d Seed=%s Mode=%d Players=%d HUD=%d Discord=%d\n",
               s.seeded_run, s.custom_seed.c_str(), g_menu_state.tournament_mode, g_menu_state.players, s.tournament_hud, s.enable_discord);

        // Apply mode and player count to RDRAM
        wr8(rdram, 0x800D5688, static_cast<uint8_t>(g_menu_state.tournament_mode));
        wr8(rdram, 0x8012EEEC, static_cast<uint8_t>(g_menu_state.tournament_mode));
        wr8(rdram, 0x8011EEEC, static_cast<uint8_t>(g_menu_state.tournament_mode));
        wr8(rdram, 0x800D57A0, static_cast<uint8_t>(g_menu_state.players));

        g_options_menu_open.store(false);
        g_menu_state.initialized = false;

        // Transition back to Options Menu by triggering the game's native Scene 16 teardown (sub-state 2)
        wr32(rdram, 0x800D3330, 2);
        return 0; // Return 0 so func_80083470 executes L_80083504 cleanly
    }

    return 1;
}

namespace tnt::tournament {
    LiveStats get_live_stats(uint8_t* rdram, uint64_t frame) {
        LiveStats stats;
        const auto& s = tnt::settings();
        stats.seeded = s.seeded_run;
        stats.active_seed = g_match_initial_seed.load();
        if (stats.active_seed == 0 && stats.seeded) {
            stats.active_seed = parse_seed_string(s.custom_seed);
        }

        if (!rdram) return stats;

        stats.live_seed = read_live_seed(rdram);

        // Active scene byte: 9 = Single-player match, 12 (0xC) = Marathon match, 14 (0xE) = Multiplayer
        uint8_t scene = rd8(rdram, SCENE_ADDR);
        bool is_game_scene = (scene == 9 || scene == 12 || scene == 14);

        if (!is_game_scene) {
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
        g_is_in_match.store(true);
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

        // Mode name detection: 0 = Marathon, 1 = Sprint, 2 = Ultra
        uint8_t mode = rd8(rdram, 0x8011EEEC);
        if (mode == 1) {
            snprintf(stats.mode_name, sizeof(stats.mode_name), "SPRINT");
        } else if (mode == 2) {
            snprintf(stats.mode_name, sizeof(stats.mode_name), "ULTRA");
        } else {
            snprintf(stats.mode_name, sizeof(stats.mode_name), "MARATHON");
        }

        // Formatted timer string
        if (stats.game_seconds >= 3600) {
            snprintf(stats.timer_str, sizeof(stats.timer_str), "%u:%02u:%02u",
                     stats.game_seconds / 3600, (stats.game_seconds % 3600) / 60, stats.game_seconds % 60);
        } else {
            snprintf(stats.timer_str, sizeof(stats.timer_str), "%u:%02u",
                     stats.game_seconds / 60, stats.game_seconds % 60);
        }

        // Blocks / Cubes made: accurately tracked via live cube creation events
        stats.blocks_made = g_live_cubes_made.load();
        stats.gold_blocks = g_live_gold_cubes.load();
        stats.silver_blocks = g_live_silver_cubes.load();
        stats.gold_lines = g_live_gold_lines.load();
        stats.silver_lines = g_live_silver_lines.load();

        if (stats.game_seconds > 0) {
            stats.lpm = (static_cast<float>(stats.true_lines) / (static_cast<float>(stats.game_seconds) / 60.0f));
        }

        return stats;
    }

    void on_match_finish(int mode, const std::string& name, uint32_t score, uint32_t true_lines, uint32_t seconds) {
        g_is_in_match.store(false);
        g_last_true_lines.store(true_lines);
        uint32_t cubes = g_live_cubes_made.load();
        g_last_blocks_made.store(cubes);

        const auto& s = tnt::settings();
        if (!s.enable_discord || s.discord_webhook.empty()) return;

        float lpm = (seconds > 0) ? (static_cast<float>(true_lines) / (static_cast<float>(seconds) / 60.0f)) : 0.0f;
        uint32_t seed = g_match_initial_seed.load();

        const char* mode_name = (mode == 0) ? "Marathon" : (mode == 1 ? "Sprint" : "Ultra");

        std::ostringstream json;
        json << "{"
             << "\"username\":\"The New Tetris: Reborn\","
             << "\"embeds\":[{"
             << "\"title\":\"🏆 Match Result - " << mode_name << "\","
             << "\"color\":16763904," // Gold hex color
             << "\"fields\":["
             << "{\"name\":\"Player\",\"value\":\"" << name << "\",\"inline\":true},"
             << "{\"name\":\"Time\",\"value\":\"" << (seconds / 60) << ":" << (seconds % 60 < 10 ? "0" : "") << (seconds % 60) << "\",\"inline\":true},"
             << "{\"name\":\"Score Lines\",\"value\":\"" << score << "\",\"inline\":true},"
             << "{\"name\":\"True Lines\",\"value\":\"" << true_lines << "\",\"inline\":true},"
             << "{\"name\":\"LPM\",\"value\":\"" << std::fixed << lpm << "\",\"inline\":true},"
             << "{\"name\":\"Total Blocks\",\"value\":\"" << cubes << "\",\"inline\":true}," << std::endl
             << "             {\"name\":\"Gold Blocks\",\"value\":\"" << g_live_gold_cubes.load() << "\",\"inline\":true}," << std::endl
             << "             {\"name\":\"Silver Blocks\",\"value\":\"" << g_live_silver_cubes.load() << "\",\"inline\":true}," << std::endl
             << "             {\"name\":\"Gold Lines\",\"value\":\"" << g_live_gold_lines.load() << "\",\"inline\":true}," << std::endl
             << "             {\"name\":\"Silver Lines\",\"value\":\"" << g_live_silver_lines.load() << "\",\"inline\":true},"
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
