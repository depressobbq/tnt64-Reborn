// Declarations for functions the recompiled code calls but that are not
// emitted into funcs.h (they are ignored in tnt.recomp.toml and implemented
// by hand in tnt_pak_stubs.c / tnt_cont_fix.c, or provided by N64ModernRuntime's pak.cpp).
// Force-include this when compiling RecompiledFuncs (clang/gcc: -include, MSVC/clang-cl: /FI).
#ifndef TNT_EXTRA_DECLS_H
#define TNT_EXTRA_DECLS_H
#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
void tnt_PfsUnknown_46E4(uint8_t* rdram, recomp_context* ctx);
void tnt_GbpakUnknown_5CF0(uint8_t* rdram, recomp_context* ctx);
void osGbpakInit(uint8_t* rdram, recomp_context* ctx);
void osGbpakPower(uint8_t* rdram, recomp_context* ctx);
void osGbpakReadWrite(uint8_t* rdram, recomp_context* ctx);
void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx); // N64ModernRuntime librecomp/src/pak.cpp
void tnt_osContInit(uint8_t* rdram, recomp_context* ctx);       // tnt_cont_fix.c
void tnt_osContGetQuery(uint8_t* rdram, recomp_context* ctx);   // tnt_cont_fix.c
void tnt_on_game_result(uint8_t* rdram, recomp_context* ctx);   // TNTRecomp/src/leaderboard.cpp (hook)
void tnt_apply_seed_override(uint8_t* rdram, recomp_context* ctx); // TNTRecomp/src/tournament.cpp (hook)
void tnt_on_cube_formed(uint8_t* rdram, recomp_context* ctx);     // TNTRecomp/src/tournament.cpp (hook)
void tnt_on_line_cleared(uint8_t* rdram, recomp_context* ctx);    // TNTRecomp/src/tournament.cpp (hook)
int tnt_handle_tournament_options(uint8_t* rdram, recomp_context* ctx); // TNTRecomp/src/tournament.cpp (hook)
void tnt_apply_menu_patches(uint8_t* rdram); // TNTRecomp/src/tournament.cpp

// Custom audio hooks
int tnt_on_play_music(uint8_t* rdram, recomp_context* ctx); // TNTRecomp/src/custom_audio.cpp (hook)
int tnt_is_custom_music_playing(void);                       // TNTRecomp/src/custom_audio.cpp (hook)
void tnt_on_set_music_volume(uint32_t vol);                  // TNTRecomp/src/custom_audio.cpp (hook)
int tnt_on_pick_random_bgm(uint8_t* rdram);                  // TNTRecomp/src/custom_audio.cpp (hook)

static inline gpr tnt_get_track_name_ptr(uint8_t* rdram, gpr reg, gpr track_idx) {
    if ((uint32_t)track_idx >= 16) {
        if (rdram) {
            uint32_t a = 0x807E0080;
            if (rdram[((a + 0) ^ 3) & 0x7FFFFF] != 'C') {
                const char* s = "CUSTOM.";
                for (int i = 0; i <= 7; ++i) {
                    rdram[((a + i) ^ 3) & 0x7FFFFF] = (uint8_t)s[i];
                }
            }
        }
        return S32(0x807E0080);
    }
    return MEM_W(0x3D48, reg);
}

#ifdef __cplusplus
}
#endif
#endif
