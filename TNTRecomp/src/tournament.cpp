#include "tnt_game.hpp"
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
#include <mutex>
#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#endif

extern std::atomic<bool> tnt_overlay_enabled;
extern std::atomic<bool> tnt_timer_enabled;

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
        int active_tab = 0; // 0: RULES, 1: ENHANCEMENTS, 2: GRAPHICS, 3: DISCORD
        int selected_row = 0;

        // Tab 0: Rules
        int piece_ruleset = 0; // 0: TNT ORIG, 1: 7-BAG, 2: NES 1989, 3: TGM, 4: RANDOM, 5: TRAINING
        bool block_training = false;
        bool seeded_run = false;
        uint32_t current_seed = 0x12345678;
        int tournament_mode = 0; // 0: Marathon, 1: Sprint, 2: Ultra
        int players = 1;         // 1 or 2

        // Tab 1: Enhancements
        bool hud_enabled = true;
        bool timer_enabled = true;
        bool panel_enabled = true;

        // Tab 2: Graphics
        int antialiasing = 3;    // 0: OFF, 1: 2X, 2: 4X, 3: 8X
        int filtering = 0;       // 0: SCALING, 1: LINEAR, 2: NEAREST
        int upscale_2d = 0;      // 0: SCALED ONLY, 1: ALL, 2: ORIGINAL
        bool show_fps = true;
        bool fullscreen = false;

        // Tab 3: Discord
        bool discord_enabled = false;
        std::string discord_player_name = "Player 1";
        bool webhook_set = false;
        bool test_post_sent = false;

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
        s.active_tab = g_menu_state.active_tab;
        s.selected_row = g_menu_state.selected_row;
        s.piece_ruleset = g_menu_state.piece_ruleset;
        s.block_training = g_menu_state.block_training;
        s.tournament_mode = g_menu_state.tournament_mode;
        s.players = g_menu_state.players;
        s.seeded_run = g_menu_state.seeded_run;
        s.current_seed = g_menu_state.current_seed;
        s.hud_enabled = g_menu_state.hud_enabled;
        s.timer_enabled = g_menu_state.timer_enabled;
        s.panel_enabled = g_menu_state.panel_enabled;
        s.antialiasing = g_menu_state.antialiasing;
        s.filtering = g_menu_state.filtering;
        s.upscale_2d = g_menu_state.upscale_2d;
        s.show_fps = g_menu_state.show_fps;
        s.fullscreen = g_menu_state.fullscreen;
        s.discord_enabled = g_menu_state.discord_enabled;
        snprintf(s.player_name, sizeof(s.player_name), "%s", g_menu_state.discord_player_name.c_str());
        s.webhook_set = g_menu_state.webhook_set;
        s.test_post_sent = g_menu_state.test_post_sent;
        return s;
    }
}

extern "C" void tnt_apply_menu_patches(uint8_t* rdram) {
    if (!rdram) return;

    // Options menu item #4 pointer in RDRAM (VRAM 0x800D5518)
    constexpr uint32_t credits_ptr_phys = 0x000D5518;
    constexpr uint32_t tourney_label_vram = 0x807E0040;

    if (rd32(rdram, credits_ptr_phys) != tourney_label_vram) {
        wr_string(rdram, tourney_label_vram, "REBORN SETTINGS");
        wr32(rdram, credits_ptr_phys, tourney_label_vram);

        // Menu label at VRAM 0x800E0724
        wr_string(rdram, 0x800E0724, "SETTINGS");

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

// Hook called when the 400-piece game queue is generated for a player
extern "C" void tnt_on_pieces_generated(uint8_t* rdram, uint32_t player) {
    if (!rdram || player > 3) return;
    const auto& s = tnt::settings();

    bool block_train = s.block_training || (s.piece_ruleset == "block_training");
    if (!block_train && (s.piece_ruleset == "tnt_original" || s.piece_ruleset.empty())) {
        return; // Keep game's authentic TNT PRNG sequence
    }

    uint32_t table_ptr = rd32(rdram, 0x800E1F20);
    if (!table_ptr) return;

    // Deterministic PRNG seeding
    uint32_t rng_state;
    if (s.seeded_run) {
        rng_state = parse_seed_string(s.custom_seed) + (player * 0x1F123BB5u);
        if (rng_state == 0) rng_state = 0x12345678u + player;
    } else {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        rng_state = static_cast<uint32_t>(now ^ (now >> 32)) + player * 7777u;
    }

    auto next_rand = [&rng_state]() -> uint32_t {
        rng_state = rng_state * 1664525u + 1013904223u;
        return (rng_state >> 16);
    };

    uint8_t pieces[400];

    // TNT piece constants:
    // 0: J, 1: L, 2: Z, 3: S, 4: T, 5: I, 6: Square (O)
    if (block_train) {
        // BlockTraining Mode: ONLY pieces 0 (J), 1 (L), 4 (T), 5 (I), 6 (Square/O)
        // Strictly excludes 2 (Z) and 3 (S) for pure mono-block (4x4 gold) and silver cube building
        int idx = 0;
        while (idx < 400) {
            uint8_t bag[5] = { 0, 1, 4, 5, 6 };
            for (int i = 4; i > 0; --i) {
                int j = next_rand() % (i + 1);
                std::swap(bag[i], bag[j]);
            }
            for (int i = 0; i < 5 && idx < 400; ++i) {
                pieces[idx++] = bag[i];
            }
        }
    } else if (s.piece_ruleset == "7_bag") {
        // Modern Tetris Guideline 7-Bag
        int idx = 0;
        while (idx < 400) {
            uint8_t bag[7] = { 0, 1, 2, 3, 4, 5, 6 };
            for (int i = 6; i > 0; --i) {
                int j = next_rand() % (i + 1);
                std::swap(bag[i], bag[j]);
            }
            for (int i = 0; i < 7 && idx < 400; ++i) {
                pieces[idx++] = bag[i];
            }
        }
    } else if (s.piece_ruleset == "nes_1989") {
        uint8_t prev = 7;
        for (int idx = 0; idx < 400; ++idx) {
            uint32_t roll = next_rand() % 8;
            if (roll == 7 || roll == prev) {
                roll = next_rand() % 7;
            }
            prev = static_cast<uint8_t>(roll);
            pieces[idx] = prev;
        }
    } else if (s.piece_ruleset == "tgm") {
        uint8_t history[4] = { 2, 2, 3, 3 }; // Z, Z, S, S
        const uint8_t first_pool[4] = { 0, 1, 4, 5 }; // J, L, T, I (no S, Z, or Square on opener)
        pieces[0] = first_pool[next_rand() % 4];
        history[0] = history[1]; history[1] = history[2]; history[2] = history[3]; history[3] = pieces[0];

        for (int idx = 1; idx < 400; ++idx) {
            uint8_t piece = next_rand() % 7;
            for (int retry = 0; retry < 4; ++retry) {
                bool in_hist = (piece == history[0] || piece == history[1] || piece == history[2] || piece == history[3]);
                if (!in_hist) break;
                piece = next_rand() % 7;
            }
            pieces[idx] = piece;
            history[0] = history[1]; history[1] = history[2]; history[2] = history[3]; history[3] = piece;
        }
    } else if (s.piece_ruleset == "pure_random") {
        for (int idx = 0; idx < 400; ++idx) {
            pieces[idx] = static_cast<uint8_t>(next_rand() % 7);
        }
    }

    // Write to player's piece table in RDRAM: (base + 1200 * player + 3 * i + 3)
    uint32_t base = table_ptr & 0x7FFFFF;
    for (uint32_t i = 0; i < 400; ++i) {
        uint32_t piece_addr = base + 1200 * player + 3 * i + 3;
        wr8(rdram, piece_addr, pieces[i]);
    }

    printf("[rules] Applied %s ruleset to player %u (first 10: %d %d %d %d %d %d %d %d %d %d)\n",
           block_train ? "BlockTraining" : s.piece_ruleset.c_str(),
           player,
           pieces[0], pieces[1], pieces[2], pieces[3], pieces[4],
           pieces[5], pieces[6], pieces[7], pieces[8], pieces[9]);
}

// -------------------------------------------------------------
// Live Piece Generator & Filter (Marathon / Single-Player & All Modes)
// -------------------------------------------------------------
namespace {
    std::mutex g_piece_mutex;
    uint32_t g_piece_rng_state = 0x12345678;

    uint32_t next_piece_rand() {
        g_piece_rng_state = g_piece_rng_state * 1664525u + 1013904223u;
        return (g_piece_rng_state >> 16);
    }

    uint8_t g_block_bag[5] = { 0, 1, 4, 5, 6 };
    int g_block_bag_idx = 5;

    uint8_t g_seven_bag[7] = { 0, 1, 2, 3, 4, 5, 6 };
    int g_seven_bag_idx = 7;

    uint8_t g_nes_prev_piece = 7;

    uint8_t g_tgm_history[4] = { 2, 2, 3, 3 }; // Z, Z, S, S
    bool g_tgm_first_piece = true;
}

extern "C" void tnt_reset_piece_generator(void) {
    std::lock_guard<std::mutex> lock(g_piece_mutex);
    const auto& s = tnt::settings();
    if (s.seeded_run) {
        g_piece_rng_state = parse_seed_string(s.custom_seed);
        if (g_piece_rng_state == 0) g_piece_rng_state = 0x12345678;
    } else {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        g_piece_rng_state = static_cast<uint32_t>(now ^ (now >> 32));
    }
    g_block_bag_idx = 5;
    g_seven_bag_idx = 7;
    g_nes_prev_piece = 7;
    g_tgm_history[0] = 2; g_tgm_history[1] = 2;
    g_tgm_history[2] = 3; g_tgm_history[3] = 3;
    g_tgm_first_piece = true;
}

namespace tnt {
    void reset_piece_generator() {
        tnt_reset_piece_generator();
    }
}

// Intercepts the next dealt piece from the game randomizer (func_80071394)
extern "C" uint32_t tnt_on_deal_next_piece(uint8_t* rdram, uint32_t orig_piece) {
    (void)rdram;
    std::lock_guard<std::mutex> lock(g_piece_mutex);
    const auto& s = tnt::settings();

    bool block_train = s.block_training || (s.piece_ruleset == "block_training");
    if (!block_train && (s.piece_ruleset == "tnt_original" || s.piece_ruleset.empty())) {
        return orig_piece;
    }

    if (block_train) {
        // BlockTraining Mode: ONLY pieces 0 (J), 1 (L), 4 (T), 5 (I), 6 (Square/O)
        // Strictly excludes 2 (Z) and 3 (S)
        if (g_block_bag_idx >= 5) {
            uint8_t pool[5] = { 0, 1, 4, 5, 6 };
            for (int i = 0; i < 5; ++i) g_block_bag[i] = pool[i];
            for (int i = 4; i > 0; --i) {
                int j = next_piece_rand() % (i + 1);
                std::swap(g_block_bag[i], g_block_bag[j]);
            }
            g_block_bag_idx = 0;
        }
        uint32_t piece = g_block_bag[g_block_bag_idx++];
        return piece;
    } else if (s.piece_ruleset == "7_bag") {
        if (g_seven_bag_idx >= 7) {
            for (int i = 0; i < 7; ++i) g_seven_bag[i] = (uint8_t)i;
            for (int i = 6; i > 0; --i) {
                int j = next_piece_rand() % (i + 1);
                std::swap(g_seven_bag[i], g_seven_bag[j]);
            }
            g_seven_bag_idx = 0;
        }
        return g_seven_bag[g_seven_bag_idx++];
    } else if (s.piece_ruleset == "nes_1989") {
        uint32_t roll = next_piece_rand() % 8;
        if (roll == 7 || roll == g_nes_prev_piece) {
            roll = next_piece_rand() % 7;
        }
        g_nes_prev_piece = static_cast<uint8_t>(roll);
        return roll;
    } else if (s.piece_ruleset == "tgm") {
        uint8_t piece = 0;
        if (g_tgm_first_piece) {
            const uint8_t first_pool[4] = { 0, 1, 4, 5 }; // J, L, T, I (no S, Z, or Square on opener)
            piece = first_pool[next_piece_rand() % 4];
            g_tgm_first_piece = false;
        } else {
            piece = static_cast<uint8_t>(next_piece_rand() % 7);
            for (int retry = 0; retry < 4; ++retry) {
                bool in_hist = (piece == g_tgm_history[0] || piece == g_tgm_history[1] ||
                                piece == g_tgm_history[2] || piece == g_tgm_history[3]);
                if (!in_hist) break;
                piece = static_cast<uint8_t>(next_piece_rand() % 7);
            }
        }
        g_tgm_history[0] = g_tgm_history[1];
        g_tgm_history[1] = g_tgm_history[2];
        g_tgm_history[2] = g_tgm_history[3];
        g_tgm_history[3] = piece;
        return piece;
    } else if (s.piece_ruleset == "pure_random") {
        return next_piece_rand() % 7;
    }

    return orig_piece;
}

// Failsafe filter on piece spawn / queue write: strictly guarantees no S or Z in BlockTraining
extern "C" uint32_t tnt_filter_spawn_piece(uint32_t piece) {
    const auto& s = tnt::settings();
    bool block_train = s.block_training || (s.piece_ruleset == "block_training");
    if (block_train) {
        if (piece == 2) return 1; // Z -> L
        if (piece == 3) return 0; // S -> J
        if (piece > 6) return 5;  // clamp -> I
    }
    return piece;
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

extern "C" void tnt_sync_player_profile(uint8_t* rdram, recomp_context* ctx) {
    if (!rdram) return;
    (void)ctx;

    uint32_t save_block = rd32(rdram, 0x8011FCF0);
    if (save_block < 0x80000000u || save_block + 0x1900 > 0x80800000u) {
        return;
    }

    const auto& s = tnt::settings();
    std::string raw_name = s.discord_player_name.empty() ? "PLAYER 1" : s.discord_player_name;

    // Strip leading '@' symbols for N64 save block because N64 font textures
    // do not contain '@' and in-game name slots are 8 uppercase alphanumeric characters
    std::string clean_raw = raw_name;
    while (!clean_raw.empty() && (clean_raw.front() == '@' || clean_raw.front() == ' ')) {
        clean_raw.erase(clean_raw.begin());
    }

    std::string target_name;
    for (char ch : clean_raw) {
        if (target_name.size() == 8) break;
        char c = (char)::toupper((unsigned char)ch);
        // Only accept A-Z, 0-9, and standard punctuation/spaces supported by TNT ROM font
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' || c == '!' || c == '?') {
            target_name += c;
        }
    }
    while (!target_name.empty() && target_name.back() == ' ') target_name.pop_back();
    if (target_name.empty()) target_name = "PLAYER 1";

    auto unpack_slot_name = [&](uint32_t slot_addr) -> std::string {
        uint8_t b[6];
        for (int k = 0; k < 6; k++) b[k] = rd8(rdram, slot_addr + k);
        uint8_t chars[8] = {
            (uint8_t)((b[0] >> 2) & 0x3F),
            (uint8_t)(((b[0] & 0x3) << 4) | ((b[1] >> 4) & 0xF)),
            (uint8_t)(((b[1] & 0xF) << 2) | ((b[2] >> 6) & 0x3)),
            (uint8_t)(b[2] & 0x3F),
            (uint8_t)((b[3] >> 2) & 0x3F),
            (uint8_t)(((b[3] & 0x3) << 4) | ((b[4] >> 4) & 0xF)),
            (uint8_t)(((b[4] & 0xF) << 2) | ((b[5] >> 6) & 0x3)),
            (uint8_t)(b[5] & 0x3F)
        };
        std::string res;
        for (int k = 0; k < 8; k++) res += (char)(chars[k] + 0x20);
        while (!res.empty() && res.back() == ' ') res.pop_back();
        return res;
    };

    auto pack_slot_name = [&](const std::string& name, uint8_t b[6]) {
        std::string padded = name;
        while (padded.size() < 8) padded += ' ';
        uint8_t chars[8];
        for (int k = 0; k < 8; k++) {
            char c = padded[k];
            if (c < 0x20 || c > 0x5F) c = ' ';
            chars[k] = (uint8_t)(c - 0x20);
        }
        b[0] = (chars[0] << 2) | (chars[1] >> 4);
        b[1] = ((chars[1] & 0xF) << 4) | (chars[2] >> 2);
        b[2] = ((chars[2] & 0x3) << 6) | chars[3];
        b[3] = (chars[4] << 2) | (chars[5] >> 4);
        b[4] = ((chars[5] & 0xF) << 4) | (chars[6] >> 2);
        b[5] = ((chars[6] & 0x3) << 6) | chars[7];
    };

    auto compute_checksum = [&](uint32_t block) -> uint32_t {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < 0x18FC; k += 4) {
            uint8_t b0 = rd8(rdram, block + k);
            uint8_t b1 = rd8(rdram, block + k + 1);
            uint8_t b2 = rd8(rdram, block + k + 2);
            uint8_t b3 = rd8(rdram, block + k + 3);
            sum = (sum + (b0 ^ 0x10)) - b2;
            sum = (sum + (b1 ^ 0x20)) - ((uint32_t)b3 << 1);
        }
        return sum;
    };

    uint32_t active_mask = rd32(rdram, save_block);
    int matched_slot = -1;
    int first_free_slot = -1;

    for (int i = 0; i < 32; i++) {
        if (active_mask & (1u << i)) {
            if (unpack_slot_name(save_block + 4 + i * 0x78) == target_name) {
                matched_slot = i;
                break;
            }
        } else if (first_free_slot == -1) {
            first_free_slot = i;
        }
    }

    if (matched_slot == -1) {
        matched_slot = (first_free_slot != -1) ? first_free_slot : 0;
        uint32_t slot_addr = save_block + 4 + matched_slot * 0x78;

        uint8_t b[6];
        pack_slot_name(target_name, b);
        for (int k = 0; k < 6; k++) wr8(rdram, slot_addr + k, b[k]);

        uint8_t s1 = static_cast<uint8_t>(((matched_slot + 1) * 37) & 0xFF);
        uint8_t s2 = static_cast<uint8_t>(((matched_slot + 1) * 73) & 0xFF);
        wr8(rdram, slot_addr + 6, s1);
        wr8(rdram, slot_addr + 7, s2);
        for (int k = 8; k < 0x78; k++) wr8(rdram, slot_addr + k, 0);

        active_mask |= (1u << matched_slot);
        wr32(rdram, save_block, active_mask);

        uint32_t sum = compute_checksum(save_block);
        wr32(rdram, save_block + 0x18FC, sum);

        printf("[profile] Populated new Name File in slot %d ('%s') in save block\n",
               matched_slot, target_name.c_str());
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
    tnt_reset_piece_generator();

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
    tnt_sync_player_profile(rdram, ctx);
}

extern "C" void tnt_on_match_start(uint8_t* rdram, recomp_context* ctx) {
    tnt_apply_seed_override(rdram, ctx);
    tnt_sync_player_profile(rdram, ctx);
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
        // In The New Tetris line clear logic (func_800698E0 / func_800696F4):
        //   clear_type == 1: Normal line clear
        //   clear_type == 2: Silver line clear (line contains multi-block pieces)
        //   clear_type == 3: Gold line clear (line contains mono-block pieces)
        //   clear_type == 4: Tetris clear (4 lines cleared with I-piece)
        if (clear_type == 3) {
            g_live_gold_lines.fetch_add(1);
        } else if (clear_type == 2) {
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
        // Exit / teardown in progress: return to Scene 4 cleanly
        g_options_menu_open.store(false);
        g_menu_state.initialized = false;
        return 0;
    }

    // State 1: In Master Settings Menu
    g_options_menu_open.store(true);

    if (!g_menu_state.initialized) {
        const auto& s = tnt::settings();
        g_menu_state.active_tab = 0;
        g_menu_state.selected_row = 0;

        // Rules
        if (s.piece_ruleset == "7_bag") g_menu_state.piece_ruleset = 1;
        else if (s.piece_ruleset == "nes_1989") g_menu_state.piece_ruleset = 2;
        else if (s.piece_ruleset == "tgm") g_menu_state.piece_ruleset = 3;
        else if (s.piece_ruleset == "pure_random") g_menu_state.piece_ruleset = 4;
        else if (s.piece_ruleset == "block_training" || s.block_training) g_menu_state.piece_ruleset = 5;
        else g_menu_state.piece_ruleset = 0; // tnt_original

        g_menu_state.block_training = (g_menu_state.piece_ruleset == 5);
        g_menu_state.tournament_mode = 0; // Marathon
        g_menu_state.players = 1;
        g_menu_state.seeded_run = s.seeded_run;
        g_menu_state.current_seed = parse_seed_string(s.custom_seed);
        if (g_menu_state.current_seed == 0) g_menu_state.current_seed = 0x12345678;

        // Enhancements
        g_menu_state.hud_enabled = s.tournament_hud;
        g_menu_state.timer_enabled = s.show_timer;
        g_menu_state.panel_enabled = s.after_game_panel;

        // Graphics
        if (s.antialiasing == "none") g_menu_state.antialiasing = 0;
        else if (s.antialiasing == "2x") g_menu_state.antialiasing = 1;
        else if (s.antialiasing == "4x") g_menu_state.antialiasing = 2;
        else g_menu_state.antialiasing = 3; // 8x

        if (s.filtering == "linear") g_menu_state.filtering = 1;
        else if (s.filtering == "nearest") g_menu_state.filtering = 2;
        else g_menu_state.filtering = 0; // pixel_scaling

        if (s.upscale_2d == "all") g_menu_state.upscale_2d = 1;
        else if (s.upscale_2d == "original") g_menu_state.upscale_2d = 2;
        else g_menu_state.upscale_2d = 0; // scaled_only

        g_menu_state.show_fps = s.show_fps;
        g_menu_state.fullscreen = s.fullscreen;

        // Discord
        g_menu_state.discord_enabled = s.enable_discord;
        g_menu_state.discord_player_name = s.discord_player_name.empty() ? "Player 1" : s.discord_player_name;
        g_menu_state.webhook_set = !s.discord_webhook.empty();
        g_menu_state.test_post_sent = false;

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

    // L / R Bumpers switch active tabs
    if (pressed & 0x0020) { // L Bumper
        g_menu_state.active_tab = (g_menu_state.active_tab + 3) % 4;
        g_menu_state.selected_row = 0;
    }
    if (pressed & 0x0010) { // R Bumper
        g_menu_state.active_tab = (g_menu_state.active_tab + 1) % 4;
        g_menu_state.selected_row = 0;
    }

    // Number of rows per tab:
    // Tab 0 (RULES): 8 rows
    // Tab 1 (ENHANCEMENTS): 5 rows
    // Tab 2 (GRAPHICS): 6 rows
    // Tab 3 (DISCORD): 5 rows
    int row_counts[4] = { 7, 5, 6, 5 };
    int max_rows = row_counts[g_menu_state.active_tab];

    if (pressed & 0x0800) { // Up
        g_menu_state.selected_row = (g_menu_state.selected_row + max_rows - 1) % max_rows;
    }
    if (pressed & 0x0400) { // Down
        g_menu_state.selected_row = (g_menu_state.selected_row + 1) % max_rows;
    }

    bool exit_menu = false;

    if (g_menu_state.active_tab == 0) { // RULES
        switch (g_menu_state.selected_row) {
            case 0: // Piece Ruleset
                if (pressed & 0x0100) g_menu_state.piece_ruleset = (g_menu_state.piece_ruleset + 1) % 6;
                if (pressed & 0x0200) g_menu_state.piece_ruleset = (g_menu_state.piece_ruleset + 5) % 6;
                if (pressed & 0x8000) g_menu_state.piece_ruleset = (g_menu_state.piece_ruleset + 1) % 6;
                g_menu_state.block_training = (g_menu_state.piece_ruleset == 5);
                break;
            case 1: // Seeded Run toggle
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.seeded_run = !g_menu_state.seeded_run;
                }
                break;
            case 2: // Match Seed Hex Adjust
                if (pressed & 0x0100) g_menu_state.current_seed += 0x00010000;
                if (pressed & 0x0200) g_menu_state.current_seed -= 0x00010000;
                break;
            case 3: // Randomize Seed
                if (pressed & (0x8000 | 0x0100 | 0x0200)) {
                    g_menu_state.current_seed = static_cast<uint32_t>(rand()) ^ (static_cast<uint32_t>(rand()) << 16);
                    if (g_menu_state.current_seed == 0) g_menu_state.current_seed = 0x12345678;
                }
                break;
            case 4: // Game Type: 0: Marathon, 1: Sprint, 2: Ultra
                if (pressed & 0x0100) g_menu_state.tournament_mode = (g_menu_state.tournament_mode + 1) % 3;
                if (pressed & 0x0200) g_menu_state.tournament_mode = (g_menu_state.tournament_mode + 2) % 3;
                break;
            case 5: // Match Mode: 1 Player / 2 Players
                if (pressed & (0x0100 | 0x0200 | 0x8000)) {
                    g_menu_state.players = (g_menu_state.players == 1) ? 2 : 1;
                }
                break;
            case 6: // Save & Return
                if (pressed & (0x8000 | 0x1000)) {
                    exit_menu = true;
                }
                break;
        }
    } else if (g_menu_state.active_tab == 1) { // ENHANCEMENTS
        switch (g_menu_state.selected_row) {
            case 0: // Sidebar HUD
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.hud_enabled = !g_menu_state.hud_enabled;
                }
                break;
            case 1: // Match Timer
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.timer_enabled = !g_menu_state.timer_enabled;
                    tnt_timer_enabled.store(g_menu_state.timer_enabled);
                }
                break;
            case 2: // After-Game Panel
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.panel_enabled = !g_menu_state.panel_enabled;
                }
                break;
            case 3: // Save & Return
                if (pressed & (0x8000 | 0x1000)) {
                    exit_menu = true;
                }
                break;
            case 4: // Exit Game
                if (pressed & (0x8000 | 0x1000)) {
                    printf("[game] Exiting via settings menu...\n");
                    tnt::audio::shutdown_custom_audio();
                    std::exit(0);
                }
                break;
        }
    } else if (g_menu_state.active_tab == 2) { // GRAPHICS
        switch (g_menu_state.selected_row) {
            case 0: // Antialiasing (MSAA)
                if (pressed & 0x0100) g_menu_state.antialiasing = (g_menu_state.antialiasing + 1) % 4;
                if (pressed & 0x0200) g_menu_state.antialiasing = (g_menu_state.antialiasing + 3) % 4;
                if (pressed & 0x8000) g_menu_state.antialiasing = (g_menu_state.antialiasing + 1) % 4;
                if (pressed & (0x0100 | 0x0200 | 0x8000)) {
                    const char* msaa_keys[4] = { "none", "2x", "4x", "8x" };
                    tnt::settings().antialiasing = msaa_keys[std::clamp(g_menu_state.antialiasing, 0, 3)];
                    tnt::apply_runtime_gfx_settings();
                }
                break;
            case 1: // Texture Filtering
                if (pressed & 0x0100) g_menu_state.filtering = (g_menu_state.filtering + 1) % 3;
                if (pressed & 0x0200) g_menu_state.filtering = (g_menu_state.filtering + 2) % 3;
                if (pressed & 0x8000) g_menu_state.filtering = (g_menu_state.filtering + 1) % 3;
                break;
            case 2: // 2D UI Upscale
                if (pressed & 0x0100) g_menu_state.upscale_2d = (g_menu_state.upscale_2d + 1) % 3;
                if (pressed & 0x0200) g_menu_state.upscale_2d = (g_menu_state.upscale_2d + 2) % 3;
                if (pressed & 0x8000) g_menu_state.upscale_2d = (g_menu_state.upscale_2d + 1) % 3;
                break;
            case 3: // FPS Overlay
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.show_fps = !g_menu_state.show_fps;
                    tnt_overlay_enabled.store(g_menu_state.show_fps);
                }
                break;
            case 4: // Display Mode (Windowed / Fullscreen)
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.fullscreen = !g_menu_state.fullscreen;
                    tnt::set_fullscreen(g_menu_state.fullscreen);
                }
                break;
            case 5: // Save & Return
                if (pressed & (0x8000 | 0x1000)) {
                    exit_menu = true;
                }
                break;
        }
    } else if (g_menu_state.active_tab == 3) { // DISCORD
        switch (g_menu_state.selected_row) {
            case 0: // Discord Webhook Toggle
                if (pressed & (0x0200 | 0x0100 | 0x8000)) {
                    g_menu_state.discord_enabled = !g_menu_state.discord_enabled;
                }
                break;
            case 1: { // Player Name (Cycle through active save block names or Discord handle)
                if (pressed & (0x0200 | 0x0100)) { // Left or Right
                    std::vector<std::string> names;
                    std::string cfg_name = tnt::settings().discord_player_name;
                    if (!cfg_name.empty()) names.push_back(cfg_name);

                    std::string clean_cfg = cfg_name;
                    while (!clean_cfg.empty() && (clean_cfg.front() == '@' || clean_cfg.front() == ' ')) clean_cfg.erase(clean_cfg.begin());
                    for (auto& c : clean_cfg) c = (char)::toupper((unsigned char)c);
                    if (clean_cfg.size() > 8) clean_cfg.resize(8);

                    uint32_t block = rd32(rdram, 0x8011FCF0);
                    if (block >= 0x80000000u && block + 0x1900 <= 0x80800000u) {
                        uint32_t mask = rd32(rdram, block);
                        for (int i = 0; i < 32; i++) {
                            if (mask & (1u << i)) {
                                uint32_t slot_addr = block + 4 + i * 0x78;
                                uint8_t b[6];
                                for (int k = 0; k < 6; k++) b[k] = rd8(rdram, slot_addr + k);
                                uint8_t chs[8] = {
                                    (uint8_t)((b[0] >> 2) & 0x3F),
                                    (uint8_t)(((b[0] & 0x3) << 4) | ((b[1] >> 4) & 0xF)),
                                    (uint8_t)(((b[1] & 0xF) << 2) | ((b[2] >> 6) & 0x3)),
                                    (uint8_t)(b[2] & 0x3F),
                                    (uint8_t)((b[3] >> 2) & 0x3F),
                                    (uint8_t)(((b[3] & 0x3) << 4) | ((b[4] >> 4) & 0xF)),
                                    (uint8_t)(((b[4] & 0xF) << 2) | ((b[5] >> 6) & 0x3)),
                                    (uint8_t)(b[5] & 0x3F)
                                };
                                std::string n;
                                for (int k = 0; k < 8; k++) n += (char)(chs[k] + 0x20);
                                while (!n.empty() && n.back() == ' ') n.pop_back();
                                if (!n.empty() && n != clean_cfg && std::find(names.begin(), names.end(), n) == names.end()) {
                                    names.push_back(n);
                                }
                            }
                        }
                    }
                    if (names.empty()) names.push_back("PLAYER 1");

                    int cur_idx = 0;
                    for (size_t i = 0; i < names.size(); i++) {
                        if (names[i] == g_menu_state.discord_player_name) {
                            cur_idx = (int)i;
                            break;
                        }
                    }
                    if (pressed & 0x0100) { // Right
                        cur_idx = (cur_idx + 1) % (int)names.size();
                    } else { // Left
                        cur_idx = (cur_idx + (int)names.size() - 1) % (int)names.size();
                    }
                    g_menu_state.discord_player_name = names[cur_idx];
                }
                break;
            }
            case 2: // Webhook status
                break;
            case 3: // Test Webhook
                if (pressed & (0x8000 | 0x1000)) {
                    const auto& s = tnt::settings();
                    if (!s.discord_webhook.empty()) {
                        std::string url = s.discord_webhook;
                        std::string json = "{\"username\":\"The New Tetris: Reborn\",\"embeds\":[{\"title\":\"🔧 Webhook Test\",\"description\":\"Connection verified successfully from The New Tetris: Reborn!\",\"color\":65535}]}";
                        std::thread([url, json]() {
                            send_discord_webhook_post(url, json);
                        }).detach();
                        g_menu_state.test_post_sent = true;
                    }
                }
                break;
            case 4: // Save & Return
                if (pressed & (0x8000 | 0x1000)) {
                    exit_menu = true;
                }
                break;
        }
    }

    if (pressed & 0x4000) { // B Button: Return / Back
        exit_menu = true;
    }

    if (exit_menu) {
        auto s = tnt::settings();

        // Rules
        const char* ruleset_keys[6] = { "tnt_original", "7_bag", "nes_1989", "tgm", "pure_random", "block_training" };
        s.piece_ruleset = ruleset_keys[std::clamp(g_menu_state.piece_ruleset, 0, 5)];
        s.block_training = (g_menu_state.piece_ruleset == 5);
        s.seeded_run = g_menu_state.seeded_run;
        char hex_buf[16];
        snprintf(hex_buf, sizeof(hex_buf), "0x%08X", g_menu_state.current_seed);
        s.custom_seed = hex_buf;

        // Enhancements
        s.tournament_hud = g_menu_state.hud_enabled;
        s.show_timer = g_menu_state.timer_enabled;
        s.after_game_panel = g_menu_state.panel_enabled;
        tnt_timer_enabled.store(s.show_timer);

        // Graphics
        const char* msaa_keys[4] = { "none", "2x", "4x", "8x" };
        s.antialiasing = msaa_keys[std::clamp(g_menu_state.antialiasing, 0, 3)];
        const char* filter_keys[3] = { "pixel_scaling", "linear", "nearest" };
        s.filtering = filter_keys[std::clamp(g_menu_state.filtering, 0, 2)];
        const char* upscale_keys[3] = { "scaled_only", "all", "original" };
        s.upscale_2d = upscale_keys[std::clamp(g_menu_state.upscale_2d, 0, 2)];
        s.show_fps = g_menu_state.show_fps;
        s.fullscreen = g_menu_state.fullscreen;
        tnt_overlay_enabled.store(s.show_fps);

        // Discord
        s.enable_discord = g_menu_state.discord_enabled;
        s.discord_player_name = g_menu_state.discord_player_name;

        tnt::settings() = s;
        tnt::save_settings(tnt::settings_path(), s);
        tnt::set_fullscreen(s.fullscreen);
        tnt::apply_runtime_gfx_settings();
        tnt::reset_piece_generator();
        tnt_sync_player_profile(rdram, ctx);
        printf("[settings] Saved Master Menu configuration: Ruleset=%s Training=%d Seed=%s MSAA=%s HUD=%d Discord=%d Fullscreen=%d\n",
               s.piece_ruleset.c_str(), s.block_training, s.custom_seed.c_str(), s.antialiasing.c_str(), s.tournament_hud, s.enable_discord, s.fullscreen);

        // Apply mode and player count to RDRAM
        wr8(rdram, 0x800D5688, static_cast<uint8_t>(g_menu_state.tournament_mode));
        wr8(rdram, 0x8012EEEC, static_cast<uint8_t>(g_menu_state.tournament_mode));
        wr8(rdram, 0x8011EEEC, static_cast<uint8_t>(g_menu_state.tournament_mode));
        wr8(rdram, 0x800D57A0, static_cast<uint8_t>(g_menu_state.players));

        g_options_menu_open.store(false);
        g_menu_state.initialized = false;

        // Transition back to Options Menu by triggering the game's native Scene 16 teardown (sub-state 2)
        wr32(rdram, 0x800D3330, 2);
        return 0;
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

        // Active ruleset name for HUD
        if (s.block_training || s.piece_ruleset == "block_training") {
            snprintf(stats.ruleset_name, sizeof(stats.ruleset_name), "TRAINING");
        } else if (s.piece_ruleset == "7_bag") {
            snprintf(stats.ruleset_name, sizeof(stats.ruleset_name), "7-BAG");
        } else if (s.piece_ruleset == "nes_1989") {
            snprintf(stats.ruleset_name, sizeof(stats.ruleset_name), "NES 1989");
        } else if (s.piece_ruleset == "tgm") {
            snprintf(stats.ruleset_name, sizeof(stats.ruleset_name), "TGM");
        } else if (s.piece_ruleset == "pure_random") {
            snprintf(stats.ruleset_name, sizeof(stats.ruleset_name), "RANDOM");
        } else {
            snprintf(stats.ruleset_name, sizeof(stats.ruleset_name), "TNT ORIG");
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
            // Only drop out if the frame counter is zeroed before any game started
            static uint32_t s_last_known_frames = 0;
            if (s_last_known_frames == 0) {
                stats.in_game = false;
                return stats;
            }
            frames = s_last_known_frames;
        }

        // HUD Latch: During Game Over, the match scene is still active and the user is viewing
        // results awaiting input. We intentionally maintain stats.in_game = true until the scene
        // changes back to menu (scene != 9, 12, 14).
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

        std::string player_disp = s.discord_player_name.empty() ? name : s.discord_player_name;

        std::ostringstream json;
        json << "{"
             << "\"username\":\"The New Tetris: Reborn\","
             << "\"embeds\":[{"
             << "\"title\":\"🏆 Match Result - " << mode_name << "\","
             << "\"color\":16763904," // Gold hex color
             << "\"fields\":["
             << "{\"name\":\"Player\",\"value\":\"" << player_disp << "\",\"inline\":true},"
             << "{\"name\":\"Time\",\"value\":\"" << (seconds / 60) << ":" << (seconds % 60 < 10 ? "0" : "") << (seconds % 60) << "\",\"inline\":true},"
             << "{\"name\":\"Score Lines\",\"value\":\"" << score << "\",\"inline\":true},"
             << "{\"name\":\"True Lines\",\"value\":\"" << true_lines << "\",\"inline\":true},"
             << "{\"name\":\"LPM\",\"value\":\"" << std::fixed << lpm << "\",\"inline\":true},"
             << "{\"name\":\"Total Blocks\",\"value\":\"" << cubes << "\",\"inline\":true},"
             << "{\"name\":\"Gold Blocks\",\"value\":\"" << g_live_gold_cubes.load() << "\",\"inline\":true},"
             << "{\"name\":\"Silver Blocks\",\"value\":\"" << g_live_silver_cubes.load() << "\",\"inline\":true},"
             << "{\"name\":\"Gold Lines\",\"value\":\"" << g_live_gold_lines.load() << "\",\"inline\":true},"
             << "{\"name\":\"Silver Lines\",\"value\":\"" << g_live_silver_lines.load() << "\",\"inline\":true},"
             << "{\"name\":\"Ruleset\",\"value\":\"`" << (s.block_training ? "BlockTraining" : s.piece_ruleset) << "`\",\"inline\":true},"
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
