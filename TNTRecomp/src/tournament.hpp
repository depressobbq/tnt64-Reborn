#pragma once
#include <cstdint>
#include <string>
#include "recomp.h"

namespace tnt::tournament {
    // Current live game stats (updated in real-time)
    struct LiveStats {
        uint32_t true_lines = 0;
        uint32_t total_score = 0;
        uint32_t game_seconds = 0;
        uint32_t blocks_made = 0; // 4x4 gold/silver cubes
        float lpm = 0.0f;
        uint32_t active_seed = 0;
        uint32_t live_seed = 0;
        bool in_game = false;
    };

    LiveStats get_live_stats(uint8_t* rdram, uint64_t frame);

    // Subroutine to read live PRNG seed state directly from N64 RDRAM
    uint32_t read_live_seed(uint8_t* rdram);

    // Called on game completion to post scores to Discord webhook
    void on_match_finish(int mode, const std::string& name, uint32_t score, uint32_t true_lines, uint32_t seconds);

    // Tournament In-Game Options Menu
    bool is_options_menu_open();
    struct MenuRenderState {
        int selected_row = 0;
        bool seeded_run = false;
        uint32_t current_seed = 0x12345678;
        bool hud_enabled = true;
        bool discord_enabled = false;
    };
    MenuRenderState get_menu_render_state();
}

// Hooks called from recompiled C code
extern "C" {
    void tnt_apply_seed_override(uint8_t* rdram, recomp_context* ctx);
    void tnt_on_cube_formed(uint8_t* rdram, recomp_context* ctx);
    int tnt_handle_tournament_options(uint8_t* rdram, recomp_context* ctx);
    void tnt_apply_menu_patches(uint8_t* rdram);
}
