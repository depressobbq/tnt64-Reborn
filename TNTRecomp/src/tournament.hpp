#pragma once
#include <cstdint>
#include <string>

struct recomp_context;

namespace tnt::tournament {
    // Current live game stats (updated in real-time)
    struct LiveStats {
        uint32_t true_lines = 0;
        uint32_t total_score = 0;
        uint32_t game_seconds = 0;
        uint32_t blocks_made = 0; // 4x4 gold/silver cubes
        float lpm = 0.0f;
        uint32_t active_seed = 0;
        bool in_game = false;
    };

    LiveStats get_live_stats(uint8_t* rdram, uint64_t frame);

    // Called on game completion to post scores to Discord webhook
    void on_match_finish(int mode, const std::string& name, uint32_t score, uint32_t true_lines, uint32_t seconds);
}

// Hook called when the game initializes the seed (func_800A3550)
extern "C" void tnt_apply_seed_override(uint8_t* rdram, struct recomp_context* ctx);
