#pragma once
#include <cstdint>
#include <string>
#include "recomp.h"

namespace tnt {
    void reset_piece_generator();
}

namespace tnt::tournament {
    // Current live game stats (updated in real-time)
    struct LiveStats {
        uint32_t true_lines = 0;
        uint32_t total_score = 0;
        uint32_t game_seconds = 0;
        uint32_t blocks_made = 0;   // total 4x4 gold/silver cubes
        uint32_t gold_blocks = 0;   // 4x4 solid gold cubes
        uint32_t silver_blocks = 0; // 4x4 silver multisquares
        uint32_t gold_lines = 0;    // lines cleared containing gold blocks
        uint32_t silver_lines = 0;  // lines cleared containing silver blocks
        float lpm = 0.0f;
        uint32_t active_seed = 0;
        uint32_t live_seed = 0;
        bool in_game = false;
        bool seeded = false;
        char mode_name[16] = "MARATHON";
        char ruleset_name[24] = "TNT ORIG";
        char timer_str[16] = "0:00";
    };

    LiveStats get_live_stats(uint8_t* rdram, uint64_t frame);

    // Read live PRNG seed state directly from N64 RDRAM
    uint32_t read_live_seed(uint8_t* rdram);

    // Post scores to Discord webhook on match completion
    void on_match_finish(int mode, const std::string& name, uint32_t score, uint32_t true_lines, uint32_t seconds);

    // Master Settings In-Game Menu (Scene 16)
    bool is_options_menu_open();
    struct MenuRenderState {
        int active_tab = 0; // 0: RULES, 1: ENHANCEMENTS, 2: GRAPHICS, 3: DISCORD
        int selected_row = 0;

        // Tab 0: Rules
        int piece_ruleset = 0; // 0: TNT ORIG, 1: MODERN 7-BAG, 2: NES 1989, 3: TGM, 4: PURE RANDOM, 5: BLOCK TRAINING
        bool block_training = false;
        bool seeded_run = false;
        uint32_t current_seed = 0x12345678;
        int tournament_mode = 0; // 0: Marathon, 1: Sprint, 2: Ultra
        int players = 1;         // 1: 1 Player, 2: 2 Players

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
        char player_name[32] = "Player 1";
        bool webhook_set = false;
        bool test_post_sent = false;
    };
    MenuRenderState get_menu_render_state();
}

// Interface functions invoked from recompiled code
extern "C" {
    void tnt_apply_seed_override(uint8_t* rdram, recomp_context* ctx);
    void tnt_on_match_piece_init(uint8_t* rdram, recomp_context* ctx);
    void tnt_on_pieces_generated(uint8_t* rdram, uint32_t player);
    void tnt_on_bag_generate(uint8_t* rdram, recomp_context* ctx);
    void tnt_on_cube_formed(uint8_t* rdram, recomp_context* ctx);
    void tnt_on_line_cleared(uint8_t* rdram, recomp_context* ctx);
    int tnt_handle_tournament_options(uint8_t* rdram, recomp_context* ctx);
    void tnt_apply_menu_patches(uint8_t* rdram);

    // Dynamic piece generation & filtering
    uint32_t tnt_on_deal_next_piece(uint8_t* rdram, uint32_t orig_piece);
    uint32_t tnt_filter_spawn_piece(uint32_t piece);
    void tnt_reset_piece_generator();
}
