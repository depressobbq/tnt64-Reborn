// On-screen overlay drawn after each game frame as an extra RDP display list (fill
// rectangles only): the FPS / frame counter (F3) and the after-game high-score panel.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if !defined(__vita__)
#include "leaderboard.hpp"
#include "tournament.hpp"
#include "settings.hpp"
#endif

extern std::atomic<bool> tnt_overlay_enabled;
extern std::atomic<float> tnt_game_fps;
// Play timer in the top-right corner (settings.ini [display] show_timer; set by the back end).
#if !defined(__vita__)
std::atomic<bool> tnt_timer_enabled{ false };
#endif
constexpr uint32_t tnt_overlay_dl_addr = 0x7F0000;
constexpr uint32_t MENU_BG_RDRAM_ADDR = 0x700000;
constexpr uint32_t MENU_CAPSULE_RDRAM_ADDR = 0x730000;
#if !defined(__vita__)
extern const uint16_t g_menu_bg_rgba16[76800];
extern const uint16_t g_menu_capsule_rgba16[4096];
#endif

// ---- Frame / FPS overlay ----------------------------------------------------
// Drawn as an extra RDP display list after each game frame, into the same colour image,
// using fill rectangles shaped as 7-segment digits. The list lives in the top page of the
// 8 MB RDRAM: The New Tetris doesn't use the Expansion Pak, so 0x400000-0x7FFFFF stays empty.
namespace {

    struct DlWriter {
        uint8_t* rdram; uint32_t addr; uint32_t start;
        void cmd(uint32_t w0, uint32_t w1) {
            *reinterpret_cast<uint32_t*>(rdram + addr) = w0;
            *reinterpret_cast<uint32_t*>(rdram + addr + 4) = w1;
            addr += 8;
        }
        bool full() const { return addr - start > 0xFF00; }
    };

    // Finds the last G_SETCIMG in the game's display list (F3DEX2), following G_DL branches.
    bool find_color_image(uint8_t* rdram, uint32_t dl, uint32_t& w0_out, uint32_t& w1_out) {
        uint32_t segments[16] = {};
        uint32_t stack[18]; int sp = 0;
        auto phys = [&](uint32_t a) -> uint32_t {
            uint32_t seg = (a >> 24) & 0xF;
            return (seg ? segments[seg] + (a & 0xFFFFFF) : a) & 0x7FFFFF;
        };
        bool found = false;
        uint32_t pc = phys(dl);
        for (int guard = 0; guard < 500000; guard++) {
            if (pc > 0x7FFFF8) return found;
            uint32_t w0 = *reinterpret_cast<uint32_t*>(rdram + pc), w1 = *reinterpret_cast<uint32_t*>(rdram + pc + 4);
            pc += 8;
            switch (w0 >> 24) {
                case 0xDB: if (((w0 >> 16) & 0xFF) == 0x06) segments[((w0 & 0xFFFF) >> 2) & 0xF] = w1 & 0x7FFFFF; break;
                case 0xDE: if (((w0 >> 16) & 0xFF) == 0) { if (sp >= 18) return found; stack[sp++] = pc; } pc = phys(w1); break;
                case 0xDF: if (sp == 0) return found; pc = stack[--sp]; break;
                case 0xE4: case 0xE5: pc += 8; break;
                case 0xFF: w0_out = w0; w1_out = w1; found = true; break;
            }
        }
        return found;
    }

    // Segments: a=top b=top-right c=bottom-right d=bottom e=bottom-left f=top-left g=middle
    const uint8_t digit_segments[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

    void rect(DlWriter& w, int x0, int y0, int x1, int y1) { // inclusive, in framebuffer pixels
        w.cmd(0xF6000000u | (uint32_t(x1) << 14) | (uint32_t(y1) << 2), (uint32_t(x0) << 14) | (uint32_t(y0) << 2));
    }
    // Draws a digit with its top-left at (x, y): 5 px wide, 9 px tall, 1 px strokes.
    void digit(DlWriter& w, int x, int y, int d) {
        uint8_t m = digit_segments[d];
        if (m & 0x01) rect(w, x, y, x + 4, y);
        if (m & 0x02) rect(w, x + 4, y, x + 4, y + 4);
        if (m & 0x04) rect(w, x + 4, y + 4, x + 4, y + 8);
        if (m & 0x08) rect(w, x, y + 8, x + 4, y + 8);
        if (m & 0x10) rect(w, x, y + 4, x, y + 8);
        if (m & 0x20) rect(w, x, y, x, y + 4);
        if (m & 0x40) rect(w, x, y + 4, x + 4, y + 4);
    }
    int number(DlWriter& w, int x, int y, const char* text) {
        for (const char* c = text; *c; c++) {
            if (*c >= '0' && *c <= '9') { digit(w, x, y, *c - '0'); x += 7; }
            else if (*c == '.') { rect(w, x, y + 8, x, y + 8); x += 3; }
            else if (*c == ':') { rect(w, x, y + 2, x, y + 2); rect(w, x, y + 6, x, y + 6); x += 3; }
            else x += 4;
        }
        return x;
    }
    uint32_t fill_color(uint32_t cimg0, uint8_t r, uint8_t g, uint8_t b) {
        if (((cimg0 >> 19) & 3) == 3) return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | 0xFF; // 32-bit
        uint16_t c = uint16_t(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);              // RGBA5551
        return (uint32_t(c) << 16) | c;
    }

    // ---- 5x7 pixel font for the after-game panel ----
    struct Glyph { char c; uint8_t rows[7]; };
    const Glyph font[] = {
        { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
        { '#', { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A } },
        { '&', { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D } },
        { '\'', { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
        { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
        { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
        { '*', { 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00 } },
        { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
        { ',', { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 } },
        { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
        { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
        { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
        { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
        { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
        { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
        { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
        { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
        { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
        { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
        { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
        { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
        { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
        { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
        { '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
        { '=', { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 } },
        { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
        { '?', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
        { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
        { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
        { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
        { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
        { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
        { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
        { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
        { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
        { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
        { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
        { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
        { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
        { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
        { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
        { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
        { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
        { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
        { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
        { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
        { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
        { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
        { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
        { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
        { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
        { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
        { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
        { '[', { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E } },
        { ']', { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E } },
        { '|', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
        { '@', { 0x0E, 0x11, 0x17, 0x15, 0x16, 0x10, 0x0F } },
        { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F } },
    };
    const uint8_t* glyph(char c) {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        for (const Glyph& g : font) if (g.c == c) return g.rows;
        return nullptr;
    }
    // Draws text at (x, y) with pixel size `s`; each horizontal run of a glyph row is one fill rect.
    void text(DlWriter& w, int x, int y, int s, const std::string& str) {
        for (char c : str) {
            if (c != ' ') {
                const uint8_t* g = glyph(c);
                if (!g) g = glyph('?');
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 5; ) {
                        if (!(g[row] & (0x10 >> col))) { col++; continue; }
                        int end = col;
                        while (end + 1 < 5 && (g[row] & (0x10 >> (end + 1)))) end++;
                        rect(w, x + col * s, y + row * s, x + (end + 1) * s - 1, y + (row + 1) * s - 1);
                        col = end + 1;
                    }
                }
            }
            x += 6 * s;
        }
    }
    void set_color(DlWriter& w, uint32_t cimg0, uint8_t r, uint8_t g, uint8_t b) {
        w.cmd(0xE7000000u, 0); // pipe sync before changing the fill colour
        w.cmd(0xF7000000u, fill_color(cimg0, r, g, b));
    }

    std::string fmt_time(uint32_t s) { char b[16]; snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60); return b; }
    std::string row_text(const char* rank, const std::string& name, uint32_t score, uint32_t seconds, int name_w = 8) {
        char b[128];
        snprintf(b, sizeof(b), "%-2s %-*.*s %5u %6s", rank, name_w, name_w, name.c_str(), score, fmt_time(seconds).c_str());
        return b;
    }

#if !defined(__vita__)
    // The game's high-score table for the mode just played, with this game's players highlighted.
    void draw_panel(DlWriter& w, uint32_t cimg0, int width, int height, const tnt::leaderboard::Panel& p) {
        const int s = width >= 640 ? 2 : 1;            // pixel size
        const int cw = 6 * s, lh = 10 * s;

        // Determine name column width dynamically (support 8 up to 16 characters)
        int max_name_len = 8;
        for (const auto& e : p.table) {
            max_name_len = std::max(max_name_len, (int)e.name.size());
        }
        for (const auto& r : p.results) {
            max_name_len = std::max(max_name_len, (int)r.name.size());
        }
        int name_w = std::clamp(max_name_len, 8, 16);
        int cols = std::max(2 + 1 + name_w + 1 + 5 + 1 + 6, 24);

        struct Line { std::string text; int color; };   // 0 white, 1 gold, 2 grey, 3 highlight
        std::vector<Line> lines;
        std::string title = std::string(tnt::leaderboard::mode_name(p.mode)) + " HIGH SCORES";
        for (auto& c : title) c = char(toupper((unsigned char)c));
        cols = std::max(cols, (int)title.size() + 2);
        int pad = std::max(0, (cols - (int)title.size()) / 2);
        lines.push_back({ std::string(pad, ' ') + title, 1 });

        auto names_match = [](const std::string& table_name, const std::string& res_name) -> bool {
            if (table_name == res_name) return true;
            std::string t = table_name;
            while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) t.pop_back();
            std::string r = res_name;
            while (!r.empty() && r.front() == '@') r.erase(r.begin());
            while (!r.empty() && (r.back() == ' ' || r.back() == '\0')) r.pop_back();
            if (t.empty() || r.empty()) return false;
            size_t len = std::min({ t.size(), r.size(), (size_t)8 });
            for (size_t i = 0; i < len; i++) {
                if (::toupper((unsigned char)t[i]) != ::toupper((unsigned char)r[i])) return false;
            }
            return true;
        };

        std::vector<bool> placed(p.results.size(), false);
        for (size_t i = 0; i < std::size(p.table); i++) {
            const auto& e = p.table[i];
            int match = -1;
            for (size_t r = 0; r < p.results.size(); r++) {
                if (!placed[r] && e.score == p.results[r].score && e.seconds == p.results[r].seconds && names_match(e.name, p.results[r].name)) {
                    match = int(r);
                    break;
                }
            }
            char rank[4]; snprintf(rank, sizeof(rank), "%d.", int(i + 1));
            if (match >= 0) {
                placed[match] = true;
                lines.push_back({ row_text(rank, p.results[match].name, e.score, e.seconds, name_w), 3 });
            } else {
                lines.push_back({ row_text(rank, e.name, e.score, e.seconds, name_w), 0 });
            }
        }
        bool any_unplaced = false;
        for (bool b : placed) if (!b) any_unplaced = true;
        if (any_unplaced) {
            lines.push_back({ "", 0 });
            for (size_t r = 0; r < p.results.size(); r++) {
                if (!placed[r]) lines.push_back({ row_text("-", p.results[r].name, p.results[r].score, p.results[r].seconds, name_w), 3 });
            }
        }

        int pw = cols * cw + 16 * s, ph = int(lines.size()) * lh + 14 * s;
        int x0 = (width - pw) / 2, y0 = (height - ph) / 2;
        if (x0 < 0 || y0 < 0) return;
        set_color(w, cimg0, 222, 170, 60);                         // border
        rect(w, x0, y0, x0 + pw - 1, y0 + ph - 1);
        set_color(w, cimg0, 14, 16, 36);                           // background
        rect(w, x0 + s, y0 + s, x0 + pw - 1 - s, y0 + ph - 1 - s);
        const uint8_t colors[4][3] = { { 236, 236, 244 }, { 255, 200, 70 }, { 140, 144, 180 }, { 90, 230, 120 } };
        int last_color = -1;
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].text.empty()) continue;
            int c = lines[i].color;
            if (c != last_color) { set_color(w, cimg0, colors[c][0], colors[c][1], colors[c][2]); last_color = c; }
            text(w, x0 + 8 * s, y0 + 8 * s + int(i) * lh, s, lines[i].text);
        }
    }

    // Draw 3D beveled box in authentic The New Tetris aesthetic
    void draw_beveled_box(DlWriter& w, uint32_t cimg0, int x0, int y0, int x1, int y1, int s) {
        // Outer highlight (top and left): bright gold
        set_color(w, cimg0, 255, 235, 110);
        rect(w, x0, y0, x1, y0 + s - 1);
        rect(w, x0, y0, x0 + s - 1, y1);

        // Outer shadow (bottom and right): deep bronze
        set_color(w, cimg0, 110, 75, 15);
        rect(w, x0, y1 - s + 1, x1, y1);
        rect(w, x1 - s + 1, y0, x1, y1);

        // Inner bevel line: warm gold
        set_color(w, cimg0, 215, 165, 50);
        rect(w, x0 + s, y0 + s, x1 - s, y0 + 2 * s - 1);
        rect(w, x0 + s, y1 - 2 * s + 1, x1 - s, y1 - s);
        rect(w, x0 + s, y0 + s, x0 + 2 * s - 1, y1 - s);
        rect(w, x1 - 2 * s + 1, y0 + s, x1 - s, y1 - s);

        // Corner decorative accent blocks
        set_color(w, cimg0, 255, 255, 160);
        rect(w, x0 + s, y0 + s, x0 + 2 * s, y0 + 2 * s);
        rect(w, x1 - 2 * s, y0 + s, x1 - s, y0 + 2 * s);
        rect(w, x0 + s, y1 - 2 * s, x0 + 2 * s, y1 - s);
        rect(w, x1 - 2 * s, y1 - 2 * s, x1 - s, y1 - s);

        // Background fill: authentic TNT midnight navy
        set_color(w, cimg0, 12, 16, 36);
        rect(w, x0 + 2 * s, y0 + 2 * s, x1 - 2 * s, y1 - 2 * s);
    }

    // Mini 4x4 Cube block (Gold or Silver)
    void draw_mini_cube(DlWriter& w, uint32_t cimg0, int x, int y, int s, bool gold) {
        uint8_t r = gold ? 255 : 220;
        uint8_t g = gold ? 215 : 230;
        uint8_t b = gold ? 50 : 255;
        uint8_t sr = gold ? 140 : 130;
        uint8_t sg = gold ? 100 : 140;
        uint8_t sb = gold ? 20 : 160;

        int sz = 7 * s;
        set_color(w, cimg0, r, g, b);
        rect(w, x, y, x + sz, y + sz);
        // Bevel / cross grid lines
        set_color(w, cimg0, sr, sg, sb);
        rect(w, x + 3 * s, y, x + 4 * s - 1, y + sz);
        rect(w, x, y + 3 * s, x + sz, y + 4 * s - 1);
        // Highlight top & left
        set_color(w, cimg0, 255, 255, gold ? 180 : 255);
        rect(w, x, y, x + sz, y);
        rect(w, x, y, x, y + sz);
    }

    // In-game Master Settings Menu dialog (Scene 16)
    void draw_tournament_options(DlWriter& w, uint32_t cimg0, int width, int height) {
        auto state = tnt::tournament::get_menu_render_state();
        const int s = width >= 640 ? 2 : 1;

        // DMA menu assets (320x240 RGBA16 BG and 256x16 Selection Capsule) to high RDRAM once with big-endian layout
        static bool s_bg_dma_done = false;
        if (!s_bg_dma_done) {
            // Background (320x240 RGBA5551)
            for (size_t i = 0; i < 76800; i += 2) {
                uint32_t val32 = (uint32_t(g_menu_bg_rgba16[i]) << 16) | g_menu_bg_rgba16[i + 1];
                *reinterpret_cast<uint32_t*>(w.rdram + MENU_BG_RDRAM_ADDR + i * 2) = val32;
            }
            // Selection Capsule Ring (256x16 RGBA5551)
            for (size_t i = 0; i < 4096; i += 2) {
                uint32_t val32 = (uint32_t(g_menu_capsule_rgba16[i]) << 16) | g_menu_capsule_rgba16[i + 1];
                *reinterpret_cast<uint32_t*>(w.rdram + MENU_CAPSULE_RDRAM_ADDR + i * 2) = val32;
            }
            s_bg_dma_done = true;
        }

        const int x_off = std::max(0, (width - 320 * s) / 2);
        const int y_off = std::max(0, (height - 240 * s) / 2);
        const int total_x_shift = int(state.enter_offset_x) * s;

        // 1. Clear letterbox borders to pure black if non-4:3 aspect ratio
        if (x_off > 0) {
            set_color(w, cimg0, 0, 0, 0);
            rect(w, 0, 0, x_off - 1, height - 1);
            rect(w, width - x_off, 0, width - 1, height - 1);
        }
        if (y_off > 0) {
            set_color(w, cimg0, 0, 0, 0);
            rect(w, 0, 0, width - 1, y_off - 1);
            rect(w, 0, height - y_off, width - 1, height - 1);
        }

        // 2. Render authentic stone wall + wooden MENU frame background via textured slices
        w.cmd(0xE7000000u, 0);                 // G_RDPPIPESYNC
        w.cmd(0xD7000002u, 0x80008000u);       // G_TEXTURE: tile 0, on=1, scale 1.0
        w.cmd(0xE3000A01u, 0x00000000u);       // G_SETOTHERMODE_H: cycle type 1CYCLE
        w.cmd(0xE200001Cu, 0x0C084040u);       // G_SETOTHERMODE_L: G_RM_OPA_SURF
        w.cmd(0xFC119623u, 0xFF2FFFFFu);       // G_SETCOMBINE: Texel0 decal RGBA

        for (int i = 0; i < 40; i++) {
            uint32_t slice_addr = MENU_BG_RDRAM_ADDR + uint32_t(i * 320 * 6 * 2);
            w.cmd(0xE6000000u, 0);                                     // G_RDPLOADSYNC
            w.cmd(0xFD100000u | 319u, slice_addr);                     // G_SETTIMG: RGBA16, 320px
            w.cmd(0xF5100000u, 0x07000000u);                           // G_SETTILE: tile 7
            w.cmd(0xF3000000u, 0x07000000u | (1919u << 12) | 0x0004u); // G_LOADBLOCK: 1920 texels
            w.cmd(0xF5100000u | (80u << 9), 0x00000000u);              // G_SETTILE: tile 0, line 80
            w.cmd(0xF2000000u, (319u << 14) | (5u << 2));              // G_SETTILESIZE: 320x6

            int y0_px = y_off + i * 6 * s;
            int y1_px = y_off + (i + 1) * 6 * s;
            int x0_px = x_off + total_x_shift;
            int x1_px = x_off + 320 * s + total_x_shift;

            if (x0_px < width && x1_px > 0) {
                uint32_t ulx_fp = uint32_t(std::max(0, x0_px)) << 2;
                uint32_t uly_fp = uint32_t(y0_px) << 2;
                uint32_t lrx_fp = uint32_t(std::min(width, x1_px)) << 2;
                uint32_t lry_fp = uint32_t(y1_px) << 2;

                w.cmd(0xE4000000u | (lrx_fp << 12) | lry_fp, (0u << 24) | (ulx_fp << 12) | uly_fp);
                w.cmd(0xE1000000u, 0u);
                uint32_t step = 0x0400u / uint32_t(s);
                w.cmd(0xF1000000u, (step << 16) | step);
            }
        }

        // 3. Tab Bar below silver MENU title (y = 43)
        w.cmd(0xE7000000u, 0);                 // G_RDPPIPESYNC
        w.cmd(0xD7000000u, 0);                 // G_TEXTURE: off
        w.cmd(0xE3000A01u, 0x00300000u);       // cycle type: FILL
        w.cmd(0xE200001Cu, 0);                 // render mode: none

        char tab_buf[64];
        const char* t0 = (state.active_tab == 0) ? "<RULES>" : " RULES ";
        const char* t1 = (state.active_tab == 1) ? "<ENHANCE>" : " ENHANCE ";
        const char* t2 = (state.active_tab == 2) ? "<GFX>" : " GFX ";
        const char* t3 = (state.active_tab == 3) ? "<DISCORD>" : " DISCORD ";
        snprintf(tab_buf, sizeof(tab_buf), "[L] %s %s %s %s [R]", t0, t1, t2, t3);

        set_color(w, cimg0, 110, 225, 255); // Cyan
        text(w, x_off + total_x_shift + 34 * s, y_off + 43 * s, s, tab_buf);

        // Accent divider line under tab bar
        set_color(w, cimg0, 100, 120, 160);
        rect(w, x_off + total_x_shift + 30 * s, y_off + 55 * s, x_off + total_x_shift + 290 * s, y_off + 55 * s + s - 1);

        // Helper: Build option rows for a given tab
        struct Row { std::string text; int color; }; // 0 white, 1 gold, 2 gray, 3 cyan, 4 green, 5 red
        auto build_tab_rows = [&](int tab_idx) -> std::vector<Row> {
            std::vector<Row> r;
            char b[64];
            if (tab_idx == 0) { // RULES
                const char* ruleset_names[6] = { "AUTHENTIC TNT", "MODERN 7-BAG", "NES 1989", "TGM HISTORY", "PURE RANDOM", "BLOCK TRAINING" };
                snprintf(b, sizeof(b), "PIECE RULESET: <%s>", ruleset_names[std::clamp(state.piece_ruleset, 0, 5)]);
                r.push_back({ b, 3 });

                snprintf(b, sizeof(b), "SEED RUN:      <%s>", state.seeded_run ? "FIXED SEED" : "RANDOM");
                r.push_back({ b, state.seeded_run ? 4 : 2 });

                snprintf(b, sizeof(b), "MATCH SEED:    <0x%08X>", state.current_seed);
                r.push_back({ b, 3 });

                r.push_back({ "RANDOMIZE SEED", 0 });

                const char* mode_str = (state.tournament_mode == 0) ? "MARATHON" :
                                      (state.tournament_mode == 1 ? "SPRINT (3 MIN)" : "ULTRA (150L)");
                snprintf(b, sizeof(b), "GAME TYPE:     <%s>", mode_str);
                r.push_back({ b, 0 });

                const char* plyr_str = (state.players == 1) ? "1 PLAYER" : "2 PLAYERS";
                snprintf(b, sizeof(b), "MATCH MODE:    <%s>", plyr_str);
                r.push_back({ b, 0 });

                r.push_back({ "SAVE & RETURN", 4 });
            } else if (tab_idx == 1) { // ENHANCEMENTS
                snprintf(b, sizeof(b), "SIDEBAR HUD:   <%s>", state.hud_enabled ? "ON" : "OFF");
                r.push_back({ b, state.hud_enabled ? 4 : 2 });

                snprintf(b, sizeof(b), "MATCH TIMER:   <%s>", state.timer_enabled ? "ON" : "OFF");
                r.push_back({ b, state.timer_enabled ? 4 : 2 });

                snprintf(b, sizeof(b), "POST-GAME PANEL:<%s>", state.panel_enabled ? "ON" : "OFF");
                r.push_back({ b, state.panel_enabled ? 4 : 2 });

                r.push_back({ "SAVE & RETURN", 4 });
                r.push_back({ "EXIT GAME", 5 });
            } else if (tab_idx == 2) { // GRAPHICS
                const char* msaa_str[4] = { "OFF", "2X", "4X", "8X" };
                snprintf(b, sizeof(b), "ANTIALIASING:  <%s>", msaa_str[std::clamp(state.antialiasing, 0, 3)]);
                r.push_back({ b, 3 });

                const char* filter_str[3] = { "PIXEL SCALING", "LINEAR", "NEAREST" };
                snprintf(b, sizeof(b), "TEXTURE FILTER:<%s>", filter_str[std::clamp(state.filtering, 0, 2)]);
                r.push_back({ b, 0 });

                const char* upscale_str[3] = { "SCALED ONLY", "ALL", "ORIGINAL" };
                snprintf(b, sizeof(b), "2D UI UPSCALE: <%s>", upscale_str[std::clamp(state.upscale_2d, 0, 2)]);
                r.push_back({ b, 0 });

                snprintf(b, sizeof(b), "FPS OVERLAY:   <%s>", state.show_fps ? "ON" : "OFF");
                r.push_back({ b, state.show_fps ? 4 : 2 });

                snprintf(b, sizeof(b), "DISPLAY MODE:  <%s>", state.fullscreen ? "FULLSCREEN" : "WINDOWED");
                r.push_back({ b, 0 });

                r.push_back({ "SAVE & RETURN", 4 });
            } else if (tab_idx == 3) { // DISCORD
                snprintf(b, sizeof(b), "DISCORD WEBHOOK:<%s>", state.discord_enabled ? "ON" : "OFF");
                r.push_back({ b, state.discord_enabled ? 4 : 2 });

                snprintf(b, sizeof(b), "PLAYER NAME:    <%s>", state.player_name);
                r.push_back({ b, 3 });

                snprintf(b, sizeof(b), "WEBHOOK URL:    <%s>", state.webhook_set ? "CONFIGURED" : "NOT SET IN INI");
                r.push_back({ b, state.webhook_set ? 4 : 2 });

                const char* test_label = state.test_post_sent ? "< POST SENT! >" : (state.webhook_set ? "[A] SEND TEST POST" : "[SET IN INI]");
                snprintf(b, sizeof(b), "TEST WEBHOOK:   %s", test_label);
                r.push_back({ b, state.test_post_sent ? 4 : 0 });

                r.push_back({ "SAVE & RETURN", 4 });
            }
            return r;
        };

        auto rows = build_tab_rows(state.active_tab);
        const int row_base_y = y_off + 63 * s;
        const int row_h = 18 * s;

        // 4. Render authentic 256x16 N64 selection capsule ring over active row
        if (state.selected_row >= 0 && state.selected_row < int(rows.size())) {
            int sel_x0 = x_off + total_x_shift + (32 + int(state.slide_offset_x)) * s;
            int sel_y0 = row_base_y + state.selected_row * row_h - 2 * s;

            // Subtle dark translucent backing inside the capsule
            set_color(w, cimg0, 18, 32, 60);
            rect(w, sel_x0 + 6 * s, sel_y0 + 2 * s, sel_x0 + 250 * s, sel_y0 + 14 * s);

            // Render authentic selection capsule / ring via 2 slices of 256x8 RGBA16
            w.cmd(0xE7000000u, 0);                 // G_RDPPIPESYNC
            w.cmd(0xD7000002u, 0x80008000u);       // G_TEXTURE: tile 0, on=1, scale 1.0
            w.cmd(0xE3000A01u, 0x00000000u);       // G_SETOTHERMODE_H: cycle type 1CYCLE
            w.cmd(0xE200001Cu, 0x00504140u);       // G_SETOTHERMODE_L: G_RM_XLU_SURF (alpha blend / cutout)
            w.cmd(0xFC119623u, 0xFF2FFFFFu);       // G_SETCOMBINE: Texel0 decal RGBA

            for (int sl = 0; sl < 2; sl++) {
                uint32_t cap_slice_addr = MENU_CAPSULE_RDRAM_ADDR + uint32_t(sl * 256 * 8 * 2);
                w.cmd(0xE6000000u, 0);                                     // G_RDPLOADSYNC
                w.cmd(0xFD100000u | 255u, cap_slice_addr);                 // G_SETTIMG: RGBA16, 256px
                w.cmd(0xF5100000u, 0x07000000u);                           // G_SETTILE: tile 7
                w.cmd(0xF3000000u, 0x07000000u | (2047u << 12) | 0x0004u); // G_LOADBLOCK: 2048 texels
                w.cmd(0xF5100000u | (64u << 9), 0x00000000u);              // G_SETTILE: tile 0, line 64
                w.cmd(0xF2000000u, (255u << 14) | (7u << 2));              // G_SETTILESIZE: 256x8

                int cy0_px = sel_y0 + sl * 8 * s;
                int cy1_px = sel_y0 + (sl + 1) * 8 * s;
                int cx0_px = sel_x0;
                int cx1_px = sel_x0 + 256 * s;

                uint32_t ulx_fp = uint32_t(cx0_px) << 2;
                uint32_t uly_fp = uint32_t(cy0_px) << 2;
                uint32_t lrx_fp = uint32_t(cx1_px) << 2;
                uint32_t lry_fp = uint32_t(cy1_px) << 2;

                w.cmd(0xE4000000u | (lrx_fp << 12) | lry_fp, (0u << 24) | (ulx_fp << 12) | uly_fp);
                w.cmd(0xE1000000u, 0u);
                uint32_t step = 0x0400u / uint32_t(s);
                w.cmd(0xF1000000u, (step << 16) | step);
            }

            // Switch back to FILL mode for text rendering
            w.cmd(0xE7000000u, 0);                 // G_RDPPIPESYNC
            w.cmd(0xD7000000u, 0);                 // G_TEXTURE: off
            w.cmd(0xE3000A01u, 0x00300000u);       // cycle type: FILL
            w.cmd(0xE200001Cu, 0);                 // render mode: none
        }

        // 5. Draw options text with smooth horizontal shift transition
        const uint8_t colors[6][3] = {
            { 240, 240, 248 }, // 0: white
            { 255, 220, 65 },  // 1: gold
            { 145, 155, 185 }, // 2: gray
            { 110, 225, 255 }, // 3: cyan
            { 90, 235, 120 },  // 4: green
            { 255, 115, 95 }   // 5: coral/red for EXIT GAME
        };

        auto draw_tab_content = [&](const std::vector<Row>& tab_rows, int tab_offset_x, bool is_current) {
            for (size_t i = 0; i < tab_rows.size(); i++) {
                if (tab_rows[i].text.empty()) continue;
                bool is_selected = is_current && (int(i) == state.selected_row);
                int c = is_selected ? 1 : tab_rows[i].color;
                set_color(w, cimg0, colors[c][0], colors[c][1], colors[c][2]);

                int text_x = x_off + total_x_shift + (38 + tab_offset_x) * s;
                int text_y = row_base_y + int(i) * row_h;

                if (text_x >= x_off + 10 * s && text_x <= x_off + 280 * s) {
                    if (is_selected) {
                        text(w, text_x - 10 * s, text_y, s, ">");
                    }
                    text(w, text_x, text_y, s, tab_rows[i].text);
                }
            }
        };

        if (state.prev_tab >= 0) {
            auto prev_rows = build_tab_rows(state.prev_tab);
            draw_tab_content(prev_rows, int(state.prev_tab_offset_x), false);
        }
        draw_tab_content(rows, int(state.slide_offset_x), true);
    }
#endif

}

namespace {
    // The game's own count of frames played in the current game (60 per second, stops while
    // paused and at game over, reset when the next game starts). Found by searching RAM.
    constexpr uint32_t game_frames_addr = 0x8011EEE8;
    // Shown while it's counting, and for 5 s after it stops (so the final time stays up).
    bool game_timer(uint8_t* rdram, uint64_t frame, char* out, size_t n) {
        static uint32_t last_value = 0;
        static uint64_t last_change = 0;
        uint32_t v = *reinterpret_cast<uint32_t*>(rdram + (game_frames_addr & 0x7FFFFC));
        if (v != last_value) { last_value = v; last_change = frame; }
        if (v == 0 || v > 60u * 60 * 60 * 24 || frame - last_change > 300) return false;
        uint32_t secs = v / 60;
        if (secs >= 3600) snprintf(out, n, "%u:%02u:%02u", secs / 3600, (secs / 60) % 60, secs % 60);
        else snprintf(out, n, "%u:%02u", secs / 60, secs % 60);
        return true;
    }
}

// Builds the overlay list (FPS counter, play timer and/or after-game panel). Returns false if there's nothing to draw.

    void draw_tournament_hud(DlWriter& w, uint32_t cimg0, int width, int height, const tnt::tournament::LiveStats& st, const char* timer_str = nullptr) {
        if (!st.in_game) return;
        const int s = width >= 640 ? 2 : 1;
        int box_w = 118 * s;
        int box_h = 116 * s;
        // Position on the right side over the scorecard
        int x = width - box_w - 4 * s;
        int y = (height >= 480) ? 36 : 18;

        // Draw authentic beveled card frame
        draw_beveled_box(w, cimg0, x - 2, y - 2, x + box_w, y + box_h, s);

        // Decorative mini-cube icon
        draw_mini_cube(w, cimg0, x + 3 * s, y + 2 * s, s, true);

        // Header: Mode of play (e.g. MARATHON, SPRINT, ULTRA)
        set_color(w, cimg0, 255, 215, 60);
        text(w, x + 13 * s, y + 3 * s, s, st.mode_name);

        // Accent line under header
        set_color(w, cimg0, 180, 140, 40);
        rect(w, x + 2 * s, y + 11 * s, x + box_w - 3 * s, y + 11 * s + s - 1);

        const char* time_display = (timer_str && timer_str[0]) ? timer_str : st.timer_str;

        char buf[64];
        // Line 1: RULES
        set_color(w, cimg0, 255, 140, 220);
        snprintf(buf, sizeof(buf), "RULES:       %s", st.ruleset_name);
        text(w, x + 4 * s, y + 15 * s, s, buf);

        // Line 2: TIME (Integrated match timer)
        set_color(w, cimg0, 120, 220, 255);
        snprintf(buf, sizeof(buf), "TIME:        %s", time_display);
        text(w, x + 4 * s, y + 25 * s, s, buf);

        // Line 3: TRUE LINES (Full "TRUE LINES" text)
        set_color(w, cimg0, 245, 245, 255);
        snprintf(buf, sizeof(buf), "TRUE LINES:  %u", st.true_lines);
        text(w, x + 4 * s, y + 35 * s, s, buf);

        // Line 4: LPM
        set_color(w, cimg0, 255, 210, 70);
        snprintf(buf, sizeof(buf), "LPM:         %.1f", st.lpm);
        text(w, x + 4 * s, y + 45 * s, s, buf);

        // Line 5: TOTAL BLOCKS
        set_color(w, cimg0, 210, 225, 250);
        snprintf(buf, sizeof(buf), "TOTAL BLOCKS:%u", st.blocks_made);
        text(w, x + 4 * s, y + 55 * s, s, buf);

        // Line 6: GOLD BLOCKS
        set_color(w, cimg0, 255, 215, 0);
        snprintf(buf, sizeof(buf), "GOLD BLOCKS: %u", st.gold_blocks);
        text(w, x + 4 * s, y + 65 * s, s, buf);

        // Line 7: SILVER BLOCKS
        set_color(w, cimg0, 200, 210, 225);
        snprintf(buf, sizeof(buf), "SILVER BLKS: %u", st.silver_blocks);
        text(w, x + 4 * s, y + 75 * s, s, buf);

        // Line 8: GOLD LINES (Lines cleared with gold blocks)
        set_color(w, cimg0, 255, 195, 50);
        snprintf(buf, sizeof(buf), "GOLD LINES:  %u", st.gold_lines);
        text(w, x + 4 * s, y + 85 * s, s, buf);

        // Line 9: SILVER LINES (Lines cleared with silver blocks)
        set_color(w, cimg0, 185, 200, 220);
        snprintf(buf, sizeof(buf), "SILVER LINES:%u", st.silver_lines);
        text(w, x + 4 * s, y + 95 * s, s, buf);

        // Line 10: STATIC MATCH SEED
        if (st.seeded) {
            snprintf(buf, sizeof(buf), "SEED: %08X", st.active_seed);
            set_color(w, cimg0, 110, 225, 255);
            text(w, x + 4 * s, y + 105 * s, s, buf);
        } else {
            set_color(w, cimg0, 160, 175, 205);
            text(w, x + 4 * s, y + 105 * s, s, "SEED: RANDOM");
        }
    }
bool tnt_build_overlay(uint8_t* rdram, uint32_t game_dl, uint64_t frame) {
#if !defined(__vita__)
    tnt_apply_menu_patches(rdram);
    bool show_options_menu = tnt::tournament::is_options_menu_open();
#else
    constexpr bool show_options_menu = false;
#endif

#if defined(__vita__)
    // The Vita build has only the FPS counter: no high-score panel or play timer.
    constexpr bool show_panel = false, show_timer = false;
    char timer[16] = {};
    (void)game_timer;
#else
    tnt::leaderboard::Panel panel;
    bool show_panel = tnt::leaderboard::get_panel(panel);
    char timer[16] = {};
    bool show_timer = tnt_timer_enabled.load() && game_timer(rdram, frame, timer, sizeof(timer));
#endif
    tnt::tournament::LiveStats tourney_stats;
    bool show_tourney_hud = tnt::settings().tournament_hud;
    if (show_tourney_hud) tourney_stats = tnt::tournament::get_live_stats(rdram, frame);
    bool show_fps = tnt_overlay_enabled.load();
    if (!show_panel && !show_fps && !show_timer && (!show_tourney_hud || !tourney_stats.in_game) && !show_options_menu) return false;
    uint32_t cimg0 = 0, cimg1 = 0;
    if (!find_color_image(rdram, game_dl, cimg0, cimg1)) return false;
    int width = int(cimg0 & 0xFFF) + 1;
    int height = width >= 640 ? 480 : 240;

    DlWriter w{ rdram, tnt_overlay_dl_addr, tnt_overlay_dl_addr }; // 64 KB, up to the end of RDRAM
    w.cmd(0xE7000000u, 0);                                  // G_RDPPIPESYNC
    w.cmd(cimg0, cimg1);                                    // same colour image as the game
    w.cmd(0xE3000A01u, 0x00300000u);                        // cycle type: FILL
    w.cmd(0xE200001Cu, 0);                                  // render mode: none
    w.cmd(0xED000000u, (uint32_t(width) << 14) | (uint32_t(height) << 2)); // scissor: whole screen

    if (show_fps) {
        char fps[16], frames[24];
        snprintf(fps, sizeof(fps), "%.1f", tnt_game_fps.load());
        snprintf(frames, sizeof(frames), "%llu", (unsigned long long)frame);
        int text_w = std::max<int>(int(strlen(fps)), int(strlen(frames))) * 7 + 4;
        int x = 8, y = 8;
        if (x + text_w < width) {
            set_color(w, cimg0, 0, 0, 0);                   // backing box
            rect(w, x - 3, y - 3, x + text_w - 2, y + 23);
            set_color(w, cimg0, 255, 214, 0);               // yellow: game fps
            number(w, x, y, fps);
            set_color(w, cimg0, 255, 255, 255);             // white: frame counter
            number(w, x, y + 12, frames);
        }
    }
    if (show_timer && (!show_tourney_hud || !tourney_stats.in_game)) {
        int text_w = 0;
        for (const char* c = timer; *c; c++) text_w += (*c == ':') ? 3 : 7;
        int x = width - text_w - 8, y = 8;
        set_color(w, cimg0, 0, 0, 0);                       // backing box
        rect(w, x - 3, y - 3, x + text_w, y + 11);
        set_color(w, cimg0, 120, 220, 255);                 // light blue: play time
        number(w, x, y, timer);
    }
#if !defined(__vita__)
    if (show_options_menu) {
        draw_tournament_options(w, cimg0, width, height);
    } else {
        if (show_panel) draw_panel(w, cimg0, width, height, panel);
        if (show_tourney_hud && tourney_stats.in_game) {
            draw_tournament_hud(w, cimg0, width, height, tourney_stats, timer);
        }
    }
#endif

    w.cmd(0xE9000000u, 0);                                  // G_RDPFULLSYNC
    w.cmd(0xDF000000u, 0);                                  // G_ENDDL
    return !w.full();
}
