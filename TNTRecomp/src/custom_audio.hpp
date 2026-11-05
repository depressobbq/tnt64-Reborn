#pragma once
#include <cstddef>
#include <cstdint>

namespace tnt::audio {
    void init_custom_audio();
    void shutdown_custom_audio();
    bool play_track(uint8_t* rdram, int track_id);
    void stop_custom_audio();
    bool is_custom_playing();
    void reset_match_track();
    void mix_custom_samples(float* buffer, size_t frame_count);
    void set_custom_volume(float vol);
}

extern "C" {
    int tnt_on_play_music(uint8_t* rdram, void* ctx);
    int tnt_is_custom_music_playing(void);
    void tnt_on_set_music_volume(uint32_t vol);
    int tnt_on_pick_random_bgm(uint8_t* rdram);
}
