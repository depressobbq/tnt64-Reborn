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
        const int cw = 6 * s, lh = 12 * s;
        const int cols = 38;

        struct Line { std::string text; int color; }; // 0 white, 1 gold, 2 gray, 3 cyan, 4 green, 5 red
        std::vector<Line> lines;

        lines.push_back({ "* REBORN MASTER SETTINGS *", 1 });

        // Tab Bar header with active tab bracketed
        char tab_buf[64];
        const char* t0 = (state.active_tab == 0) ? "<RULES>" : " RULES ";
        const char* t1 = (state.active_tab == 1) ? "<ENHANCE>" : " ENHANCE ";
        const char* t2 = (state.active_tab == 2) ? "<GFX>" : " GFX ";
        const char* t3 = (state.active_tab == 3) ? "<DISCORD>" : " DISCORD ";
        snprintf(tab_buf, sizeof(tab_buf), "[L] %s %s %s %s [R]", t0, t1, t2, t3);
        lines.push_back({ tab_buf, 3 });
        lines.push_back({ "", 0 });

        char buf[64];

        if (state.active_tab == 0) { // RULES
            const char* ruleset_names[6] = { "AUTHENTIC TNT", "MODERN 7-BAG", "NES 1989", "TGM HISTORY", "PURE RANDOM", "BLOCK TRAINING" };
            snprintf(buf, sizeof(buf), "%s PIECE RULESET: <%s>", state.selected_row == 0 ? ">" : " ", ruleset_names[std::clamp(state.piece_ruleset, 0, 5)]);
            lines.push_back({ buf, state.selected_row == 0 ? 1 : 3 });

            snprintf(buf, sizeof(buf), "%s SEED RUN:      <%s>", state.selected_row == 1 ? ">" : " ", state.seeded_run ? "FIXED SEED" : "RANDOM");
            lines.push_back({ buf, state.selected_row == 1 ? 1 : (state.seeded_run ? 4 : 2) });

            snprintf(buf, sizeof(buf), "%s MATCH SEED:    <0x%08X>", state.selected_row == 2 ? ">" : " ", state.current_seed);
            lines.push_back({ buf, state.selected_row == 2 ? 1 : 3 });

            snprintf(buf, sizeof(buf), "%s RANDOMIZE SEED", state.selected_row == 3 ? ">" : " ");
            lines.push_back({ buf, state.selected_row == 3 ? 1 : 0 });

            const char* mode_str = (state.tournament_mode == 0) ? "MARATHON" :
                                  (state.tournament_mode == 1 ? "SPRINT (3 MIN)" : "ULTRA (150L)");
            snprintf(buf, sizeof(buf), "%s GAME TYPE:     <%s>", state.selected_row == 4 ? ">" : " ", mode_str);
            lines.push_back({ buf, state.selected_row == 4 ? 1 : 0 });

            const char* plyr_str = (state.players == 1) ? "1 PLAYER" : "2 PLAYERS";
            snprintf(buf, sizeof(buf), "%s MATCH MODE:    <%s>", state.selected_row == 5 ? ">" : " ", plyr_str);
            lines.push_back({ buf, state.selected_row == 5 ? 1 : 0 });

            snprintf(buf, sizeof(buf), "%s SAVE & RETURN", state.selected_row == 6 ? ">" : " ");
            lines.push_back({ buf, state.selected_row == 6 ? 1 : 4 });
        } else if (state.active_tab == 1) { // ENHANCEMENTS
            snprintf(buf, sizeof(buf), "%s SIDEBAR HUD:   <%s>", state.selected_row == 0 ? ">" : " ", state.hud_enabled ? "ON" : "OFF");
            lines.push_back({ buf, state.selected_row == 0 ? 1 : (state.hud_enabled ? 4 : 2) });

            snprintf(buf, sizeof(buf), "%s MATCH TIMER:   <%s>", state.selected_row == 1 ? ">" : " ", state.timer_enabled ? "ON" : "OFF");
            lines.push_back({ buf, state.selected_row == 1 ? 1 : (state.timer_enabled ? 4 : 2) });

            snprintf(buf, sizeof(buf), "%s POST-GAME PANEL:<%s>", state.selected_row == 2 ? ">" : " ", state.panel_enabled ? "ON" : "OFF");
            lines.push_back({ buf, state.selected_row == 2 ? 1 : (state.panel_enabled ? 4 : 2) });

            snprintf(buf, sizeof(buf), "%s SAVE & RETURN", state.selected_row == 3 ? ">" : " ");
            lines.push_back({ buf, state.selected_row == 3 ? 1 : 4 });

            snprintf(buf, sizeof(buf), "%s EXIT GAME", state.selected_row == 4 ? ">" : " ");
            lines.push_back({ buf, state.selected_row == 4 ? 1 : 5 });
        } else if (state.active_tab == 2) { // GRAPHICS
            const char* msaa_str[4] = { "OFF", "2X", "4X", "8X" };
            snprintf(buf, sizeof(buf), "%s ANTIALIASING:  <%s>", state.selected_row == 0 ? ">" : " ", msaa_str[std::clamp(state.antialiasing, 0, 3)]);
            lines.push_back({ buf, state.selected_row == 0 ? 1 : 3 });

            const char* filter_str[3] = { "PIXEL SCALING", "LINEAR", "NEAREST" };
            snprintf(buf, sizeof(buf), "%s TEXTURE FILTER:<%s>", state.selected_row == 1 ? ">" : " ", filter_str[std::clamp(state.filtering, 0, 2)]);
            lines.push_back({ buf, state.selected_row == 1 ? 1 : 0 });

            const char* upscale_str[3] = { "SCALED ONLY", "ALL", "ORIGINAL" };
            snprintf(buf, sizeof(buf), "%s 2D UI UPSCALE: <%s>", state.selected_row == 2 ? ">" : " ", upscale_str[std::clamp(state.upscale_2d, 0, 2)]);
            lines.push_back({ buf, state.selected_row == 2 ? 1 : 0 });

            snprintf(buf, sizeof(buf), "%s FPS OVERLAY:   <%s>", state.selected_row == 3 ? ">" : " ", state.show_fps ? "ON" : "OFF");
            lines.push_back({ buf, state.selected_row == 3 ? 1 : (state.show_fps ? 4 : 2) });

            snprintf(buf, sizeof(buf), "%s DISPLAY MODE:  <%s>", state.selected_row == 4 ? ">" : " ", state.fullscreen ? "FULLSCREEN" : "WINDOWED");
            lines.push_back({ buf, state.selected_row == 4 ? 1 : 0 });

            snprintf(buf, sizeof(buf), "%s SAVE & RETURN", state.selected_row == 5 ? ">" : " ");
            lines.push_back({ buf, state.selected_row == 5 ? 1 : 4 });
        } else if (state.active_tab == 3) { // DISCORD
            snprintf(buf, sizeof(buf), "%s DISCORD WEBHOOK:<%s>", state.selected_row == 0 ? ">" : " ", state.discord_enabled ? "ON" : "OFF");
            lines.push_back({ buf, state.selected_row == 0 ? 1 : (state.discord_enabled ? 4 : 2) });

            snprintf(buf, sizeof(buf), "%s PLAYER NAME:    <%s>", state.selected_row == 1 ? ">" : " ", state.player_name);
            lines.push_back({ buf, state.selected_row == 1 ? 1 : 3 });

            snprintf(buf, sizeof(buf), "%s WEBHOOK URL:    <%s>", state.selected_row == 2 ? ">" : " ", state.webhook_set ? "CONFIGURED" : "NOT SET IN INI");
            lines.push_back({ buf, state.selected_row == 2 ? 1 : (state.webhook_set ? 4 : 2) });

            const char* test_label = state.test_post_sent ? "< POST SENT! >" : (state.webhook_set ? "[A] SEND TEST POST" : "[SET IN INI]");
            snprintf(buf, sizeof(buf), "%s TEST WEBHOOK:   %s", state.selected_row == 3 ? ">" : " ", test_label);
            lines.push_back({ buf, state.selected_row == 3 ? 1 : (state.test_post_sent ? 4 : 0) });

            snprintf(buf, sizeof(buf), "%s SAVE & RETURN", state.selected_row == 4 ? ">" : " ");
            lines.push_back({ buf, state.selected_row == 4 ? 1 : 4 });
        }

        lines.push_back({ "", 0 });
        lines.push_back({ " [L/R] SWITCH TAB   [D-PAD] ADJUST", 2 });
        lines.push_back({ " [A] SELECT/TOGGLE  [B] SAVE & RETURN", 2 });

        int menu_cols = cols;
        for (const auto& l : lines) {
            if ((int)l.text.size() + 2 > menu_cols) menu_cols = (int)l.text.size() + 2;
        }

        int pw = menu_cols * cw + 24 * s, ph = int(lines.size()) * lh + 20 * s;
        int x0 = (width - pw) / 2, y0 = (height - ph) / 2;
        if (x0 < 0 || y0 < 0) return;

        // Draw authentic ornate beveled panel
        draw_beveled_box(w, cimg0, x0, y0, x0 + pw - 1, y0 + ph - 1, s);

        // Decorative mini-cubes on header
        draw_mini_cube(w, cimg0, x0 + 10 * s, y0 + 7 * s, s, true);
        draw_mini_cube(w, cimg0, x0 + pw - 18 * s, y0 + 7 * s, s, false);

        // Accent line under title
        set_color(w, cimg0, 190, 150, 45);
        rect(w, x0 + 8 * s, y0 + 19 * s, x0 + pw - 9 * s, y0 + 19 * s + s - 1);

        // Accent line under tab bar
        set_color(w, cimg0, 120, 140, 180);
        rect(w, x0 + 8 * s, y0 + 31 * s, x0 + pw - 9 * s, y0 + 31 * s + s - 1);

        // Glowing selection pill highlight (line 3 is row 0)
        int sel_line = 3 + state.selected_row;
        int hl_y0 = y0 + 10 * s + sel_line * lh - 1 * s;
        int hl_y1 = hl_y0 + lh - 1 * s;

        // Pill background
        set_color(w, cimg0, 30, 50, 95);
        rect(w, x0 + 6 * s, hl_y0, x0 + pw - 7 * s, hl_y1);
        // Pill top/bottom glow border
        set_color(w, cimg0, 80, 160, 240);
        rect(w, x0 + 6 * s, hl_y0, x0 + pw - 7 * s, hl_y0 + s - 1);
        rect(w, x0 + 6 * s, hl_y1 - s + 1, x0 + pw - 7 * s, hl_y1);

        const uint8_t colors[6][3] = {
            { 240, 240, 248 }, // 0: white
            { 255, 215, 60 },  // 1: gold
            { 145, 155, 185 }, // 2: gray
            { 110, 225, 255 }, // 3: cyan
            { 90, 235, 120 },  // 4: green
            { 255, 115, 95 }   // 5: coral/red for EXIT GAME
        };

        int last_color = -1;
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].text.empty()) continue;
            int c = lines[i].color;
            if (c != last_color) {
                set_color(w, cimg0, colors[c][0], colors[c][1], colors[c][2]);
                last_color = c;
            }
            text(w, x0 + 10 * s, y0 + 10 * s + int(i) * lh, s, lines[i].text);
        }
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
