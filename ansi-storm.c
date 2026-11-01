/*
 * ansi-storm
 * Copyright (C) k!M/pizslacker 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_DROPS    280
#define MAX_SPLASHES 80
#define MAX_BOLT_PTS 96

typedef struct {
    uint8_t r, g, b;
} Color;

typedef struct {
    float x, y;
    float speed;
    int layer; // 0 = far, 1 = mid, 2 = near
} Raindrop;

typedef struct {
    float x, y;
    float vx, vy;
    int life;
} Splash;

typedef struct {
    int x0, y0, x1, y1;
} Segment;

// Dynamic viewport dimensions
static int term_w = 80;
static int term_h = 23; // 1 row reserved for top status bar
static int pix_w  = 80;
static int pix_h  = 46;

// Dynamically allocated buffers
static Color *fb = NULL;
static char  *out_buf = NULL;

// Macro for 2D indexing into flat framebuffer
#define FB(y, x) fb[(y) * pix_w + (x)]

static Raindrop drops[MAX_DROPS];
static Splash   splashes[MAX_SPLASHES];
static Segment  bolt_segs[MAX_BOLT_PTS];
static int      num_bolt_segs = 0;
static int      active_drops = 150;

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t needs_resize = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
}

static void handle_sigwinch(int sig)
{
    (void)sig;
    needs_resize = 1;
}

// Clamp integer to [0, 255]
static inline uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// Blend foreground color over background with alpha (0.0 to 1.0)
static inline Color blend(Color bg, Color fg, float alpha)
{
    if (alpha <= 0.0f) return bg;
    if (alpha >= 1.0f) return fg;
    Color out;
    out.r = clamp_u8((int)(bg.r * (1.0f - alpha) + fg.r * alpha));
    out.g = clamp_u8((int)(bg.g * (1.0f - alpha) + fg.g * alpha));
    out.b = clamp_u8((int)(bg.b * (1.0f - alpha) + fg.b * alpha));
    return out;
}

// Additive light helper for volumetric beams and glows
static inline Color add_light(Color base, Color light, float intensity)
{
    if (intensity <= 0.0f) return base;
    Color out;
    out.r = clamp_u8(base.r + (int)(light.r * intensity));
    out.g = clamp_u8(base.g + (int)(light.g * intensity));
    out.b = clamp_u8(base.b + (int)(light.b * intensity));
    return out;
}

// Query terminal size via ioctl(TIOCGWINSZ) and reallocate buffers
static void resize_viewport(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        term_w = (ws.ws_col >= 40) ? ws.ws_col : 40;
        // Reserve 1 line at the top for header and 1 at the bottom to prevent scrolling
        term_h = (ws.ws_row >= 12) ? (ws.ws_row - 2) : 10;
    } else {
        term_w = 80;
        term_h = 22;
    }

    pix_w = term_w;
    pix_h = term_h * 2;

    free(fb);
    free(out_buf);

    fb = calloc((size_t)(pix_w * pix_h), sizeof(Color));
    // Each cell takes up to ~42 bytes of ANSI escape sequences + UTF-8 '▀'
    out_buf = malloc((size_t)(term_w * term_h * 48 + 1024));

    if (!fb || !out_buf) {
        fprintf(stderr, "Memory allocation failed on resize.\n");
        exit(EXIT_FAILURE);
    }

    // Scale active raindrop count to match window area
    active_drops = (pix_w * pix_h) / 24;
    if (active_drops > MAX_DROPS) active_drops = MAX_DROPS;
    if (active_drops < 60) active_drops = 60;

    for (int i = 0; i < active_drops; i++) {
        drops[i].x = (float)(rand() % pix_w);
        drops[i].y = (float)(rand() % pix_h);
        drops[i].layer = i % 3;
        drops[i].speed = 0.85f + (float)drops[i].layer * 0.45f + ((float)(rand() % 100) / 400.0f);
    }

    // Clear terminal artifacts after window resize
    printf("\033[2J");
    needs_resize = 0;
}

// Procedural billowing cloud density
static float cloud_density(float x, float y, float offset, float scale_x, float center_y, float thickness)
{
    float wave = sinf((x + offset) * scale_x) * (thickness * 0.42f)
               + cosf((x + offset * 0.7f) * (scale_x * 2.3f)) * (thickness * 0.26f)
               + sinf((x - offset * 0.3f) * (scale_x * 4.1f)) * (thickness * 0.12f);

    float top_edge = center_y - thickness + wave;
    float bot_edge = center_y + thickness * 0.55f + wave * 0.25f;

    if (y < top_edge || y > bot_edge) return 0.0f;

    float mid = (top_edge + bot_edge) * 0.5f;
    float half_span = (bot_edge - top_edge) * 0.5f;
    if (half_span <= 0.01f) return 0.0f;

    float dist = fabsf(y - mid) / half_span;
    float d = 1.0f - dist * dist;
    return (d > 0.0f) ? d : 0.0f;
}

// Proportional ridge height for parallax mountain layers
static float mountain_ridge_y(float x, float offset, int layer, int sea_y)
{
    float amp_scale = (float)pix_h / 48.0f;
    if (layer == 0) {
        float base = (float)sea_y - 11.5f * amp_scale;
        return base
             - sinf((x + offset) * 0.065f) * (5.0f * amp_scale)
             - cosf((x + offset) * 0.17f)  * (2.6f * amp_scale)
             - sinf((x + offset) * 0.39f)  * (1.1f * amp_scale);
    } else {
        float base = (float)sea_y - 7.0f * amp_scale;
        return base
             - sinf((x + offset + 25.0f) * 0.09f) * (4.2f * amp_scale)
             - cosf((x + offset) * 0.23f)         * (2.0f * amp_scale)
             - sinf((x + offset) * 0.51f)         * (0.8f * amp_scale);
    }
}

// Proportional foreground rocky headland height on the right side
static int headland_top_y(int x, int cliff_start_x, int sea_y)
{
    if (x < cliff_start_x) return pix_h;
    float ramp_w = (float)(pix_w - cliff_start_x) * 0.42f;
    if (ramp_w < 1.0f) ramp_w = 1.0f;

    float norm = (float)(x - cliff_start_x) / ramp_w;
    if (norm > 1.0f) norm = 1.0f;

    float cliff_rise = (float)pix_h * 0.135f;
    float roughness = sinf((float)x * 0.6f) * 0.7f + cosf((float)x * 1.3f) * 0.4f;
    return (int)((float)(sea_y + 2) - norm * cliff_rise + roughness);
}

// Draw a line segment into the dynamic framebuffer (Bresenham)
static void draw_line(int x0, int y0, int x1, int y1, Color c, float alpha)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        if (x0 >= 0 && x0 < pix_w && y0 >= 0 && y0 < pix_h) {
            FB(y0, x0) = blend(FB(y0, x0), c, alpha);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Spawn a jagged lightning bolt scaled to current viewport height
static void spawn_lightning(int *strike_x, int sea_y)
{
    num_bolt_segs = 0;
    int max_x = (int)((float)pix_w * 0.68f);
    if (max_x < 16) max_x = 16;

    int x = 6 + rand() % (max_x - 6);
    int y = 2;
    *strike_x = x;

    while (y < sea_y - 3 && num_bolt_segs < MAX_BOLT_PTS - 4) {
        int nx = x + (rand() % 9) - 4;
        int ny = y + 2 + (rand() % 4);
        bolt_segs[num_bolt_segs++] = (Segment){x, y, nx, ny};

        if ((rand() % 100) < 32 && num_bolt_segs < MAX_BOLT_PTS - 2) {
            int bx = nx + ((rand() % 2) ? 1 : -1) * (4 + rand() % 6);
            int by = ny + 3 + (rand() % 4);
            bolt_segs[num_bolt_segs++] = (Segment){nx, ny, bx, by};

            int bx2 = bx + ((rand() % 7) - 3);
            int by2 = by + 2 + (rand() % 4);
            bolt_segs[num_bolt_segs++] = (Segment){bx, by, bx2, by2};
        }

        x = nx;
        y = ny;
    }
}

// Spawn a splash particle at (x, y)
static void spawn_splash(float x, float y)
{
    for (int i = 0; i < MAX_SPLASHES; i++) {
        if (splashes[i].life <= 0) {
            splashes[i].x = x;
            splashes[i].y = y;
            splashes[i].vx = ((float)(rand() % 100) / 50.0f - 1.0f) * 0.65f;
            splashes[i].vy = -0.45f - ((float)(rand() % 100) / 260.0f);
            splashes[i].life = 3 + (rand() % 3);
            break;
        }
    }
}

int main(void)
{
    const char *pname = "ansi-storm-demo";
    const char *pversion = "1.2";

    srand((unsigned int)time(NULL));

    // Register SIGINT (Ctrl+C) and SIGWINCH (terminal window resize)
    struct sigaction sa_int = {0}, sa_winch = {0};
    sa_int.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa_int, NULL);

    sa_winch.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &sa_winch, NULL);

    // Enter alternate screen buffer and hide cursor
    printf("\033[?1049h\033[2J\033[?25l");
    resize_viewport();

    float t = 0.0f;
    float flash_intensity = 0.0f;
    int strike_x = pix_w / 3;

    while (keep_running) {
        if (needs_resize) {
            resize_viewport();
            strike_x = pix_w / 3;
        }

        // Dynamic proportional landmarks based on current pix_w and pix_h
        int sea_y         = (int)((float)pix_h * 0.86f);
        int cliff_start_x = (int)((float)pix_w * 0.58f);
        int lh_x          = (int)((float)pix_w * 0.78f);
        int lh_bot_y      = headland_top_y(lh_x, cliff_start_x, sea_y);
        int lh_height     = (int)((float)pix_h * 0.30f);
        if (lh_height < 10) lh_height = 10;
        int lh_top_y      = lh_bot_y - lh_height;

        // Trigger random lightning strikes
        if (flash_intensity <= 0.05f && (rand() % 1000) < 28) {
            flash_intensity = 1.0f;
            spawn_lightning(&strike_x, sea_y);
        }

        // Rotating lighthouse beacon math
        float beacon_phase  = t * 1.7f;
        float sweep_dir     = cosf(beacon_phase);
        float toward_viewer = sinf(beacon_phase);
        float viewer_flare  = (toward_viewer > 0.0f) ? powf(toward_viewer, 6.0f) : 0.0f;

        // 1. Render Night Sky Gradient + Lightning Flash
        for (int y = 0; y < pix_h; y++) {
            float v = (float)y / (float)(sea_y > 0 ? sea_y : 1);
            if (v > 1.0f) v = 1.0f;
            for (int x = 0; x < pix_w; x++) {
                int r = (int)(7.0f  + v * 15.0f);
                int g = (int)(11.0f + v * 22.0f);
                int b = (int)(24.0f + v * 38.0f);

                if (flash_intensity > 0.01f) {
                    float dist_x = fabsf((float)x - (float)strike_x) / (float)pix_w;
                    float glow = flash_intensity * (1.0f - dist_x * 0.85f);
                    if (glow < 0.0f) glow = 0.0f;
                    r += (int)(glow * 140.0f);
                    g += (int)(glow * 155.0f);
                    b += (int)(glow * 195.0f);
                }

                FB(y, x) = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
            }
        }

        // 2. Render 3 Parallax Cloud Layers Scaled to Viewport Height
        float h_scale = (float)pix_h / 48.0f;
        const float speeds[3]    = { 3.2f, 7.5f, 15.0f };
        const float scales[3]    = { 0.09f / h_scale, 0.12f / h_scale, 0.16f / h_scale };
        const float centers_y[3] = { 7.5f * h_scale, 12.0f * h_scale, 16.5f * h_scale };
        const float thickness[3] = { 4.2f * h_scale, 5.2f * h_scale,  6.2f * h_scale  };

        int cloud_limit_y = (int)((float)sea_y * 0.82f);
        for (int layer = 0; layer < 3; layer++) {
            float offset = t * speeds[layer] + layer * 37.0f;

            for (int y = 0; y < cloud_limit_y; y++) {
                for (int x = 0; x < pix_w; x++) {
                    float d = cloud_density((float)x, (float)y, offset,
                                            scales[layer], centers_y[layer], thickness[layer]);
                    if (d <= 0.01f) continue;

                    float shade = 1.0f - ((float)y / (float)(cloud_limit_y > 0 ? cloud_limit_y : 1));
                    if (shade < 0.1f) shade = 0.1f;

                    int base_c = 20 + (2 - layer) * 13;
                    int r = base_c + (int)(shade * 26.0f);
                    int g = base_c + 4 + (int)(shade * 30.0f);
                    int b = base_c + 14 + (int)(shade * 42.0f);

                    if (flash_intensity > 0.01f) {
                        float dist_x = fabsf((float)x - (float)strike_x) / (float)pix_w;
                        float rim = flash_intensity * (1.1f - dist_x) * (0.4f + shade * 0.8f);
                        if (rim > 0.0f) {
                            r += (int)(rim * 165.0f);
                            g += (int)(rim * 175.0f);
                            b += (int)(rim * 210.0f);
                        }
                    }

                    Color cloud_col = {clamp_u8(r), clamp_u8(g), clamp_u8(b)};
                    float alpha = d * (0.65f + layer * 0.15f);
                    if (alpha > 0.96f) alpha = 0.96f;
                    FB(y, x) = blend(FB(y, x), cloud_col, alpha);
                }
            }
        }

        // 3. Render Lightning Bolt Segments
        if (flash_intensity > 0.15f) {
            Color glow_col = {150, 185, 255};
            Color core_col = {255, 255, 255};
            for (int i = 0; i < num_bolt_segs; i++) {
                Segment s = bolt_segs[i];
                draw_line(s.x0 - 1, s.y0, s.x1 - 1, s.y1, glow_col, flash_intensity * 0.55f);
                draw_line(s.x0 + 1, s.y0, s.x1 + 1, s.y1, glow_col, flash_intensity * 0.55f);
                draw_line(s.x0, s.y0, s.x1, s.y1, core_col, flash_intensity);
            }
        }

        // 4. Render 2 Parallax Mountain Ranges
        const float mtn_speeds[2] = { 0.8f, 2.1f };
        for (int mlayer = 0; mlayer < 2; mlayer++) {
            float m_offset = t * mtn_speeds[mlayer];
            for (int x = 0; x < pix_w; x++) {
                float ry = mountain_ridge_y((float)x, m_offset, mlayer, sea_y);
                int top_y = (int)ry;
                if (top_y < 0) top_y = 0;

                for (int y = top_y; y < sea_y; y++) {
                    float depth_fade = (float)(y - top_y) / (14.0f * h_scale);
                    if (depth_fade > 1.0f) depth_fade = 1.0f;

                    int r = (mlayer == 0) ? (16 - (int)(depth_fade * 5.0f)) : (10 - (int)(depth_fade * 4.0f));
                    int g = (mlayer == 0) ? (22 - (int)(depth_fade * 6.0f)) : (14 - (int)(depth_fade * 5.0f));
                    int b = (mlayer == 0) ? (38 - (int)(depth_fade * 8.0f)) : (25 - (int)(depth_fade * 6.0f));

                    if (flash_intensity > 0.02f) {
                        float dist_x = fabsf((float)x - (float)strike_x) / (float)pix_w;
                        float proximity = (1.1f - dist_x);
                        if (proximity < 0.0f) proximity = 0.0f;

                        if (y <= top_y + 1) {
                            float rim = flash_intensity * proximity * ((mlayer == 0) ? 0.95f : 0.75f);
                            r += (int)(rim * 130.0f);
                            g += (int)(rim * 155.0f);
                            b += (int)(rim * 205.0f);
                        } else {
                            float face = flash_intensity * proximity * (1.0f - depth_fade * 0.7f) * 0.32f;
                            r += (int)(face * 95.0f);
                            g += (int)(face * 115.0f);
                            b += (int)(face * 155.0f);
                        }
                    }

                    FB(y, x) = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
                }
            }
        }

        // 5. Render Volumetric Rotating Lighthouse Beam
        float beam_cx  = (float)lh_x;
        float beam_cy  = (float)(lh_top_y + 2);
        float max_beam = (float)pix_w * 0.85f;
        float halo_rad = (float)pix_h * 0.38f;
        Color beam_col = {255, 232, 155};

        for (int y = 2; y < sea_y + 2 && y < pix_h; y++) {
            for (int x = 0; x < pix_w; x++) {
                float dx = (float)x - beam_cx;
                float dy = (float)y - beam_cy;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist < 1.0f || dist > max_beam) continue;

                float side_match = (dx * sweep_dir > 0.0f) ? fabsf(sweep_dir) : 0.0f;
                if (side_match > 0.02f && fabsf(dx) > 0.5f) {
                    float slope = fabsf(dy + 0.04f * dist) / fabsf(dx);
                    float cone_half_width = 0.16f + (1.0f - side_match) * 0.12f;
                    if (slope < cone_half_width) {
                        float angular = 1.0f - (slope / cone_half_width);
                        float radial  = 1.0f - (dist / max_beam);
                        float b_alpha = angular * angular * radial * side_match * 0.58f;
                        FB(y, x) = add_light(FB(y, x), beam_col, b_alpha);
                    }
                }

                if (viewer_flare > 0.01f && dist < halo_rad) {
                    float halo = (1.0f - dist / halo_rad);
                    float h_alpha = halo * halo * viewer_flare * 0.75f;
                    FB(y, x) = add_light(FB(y, x), (Color){255, 248, 210}, h_alpha);
                }
            }
        }

        // 6. Render Stormy Sea & Reflections
        for (int y = sea_y; y < pix_h; y++) {
            int depth = y - sea_y;
            for (int x = 0; x < pix_w; x++) {
                float wave = sinf((float)x * 0.35f + t * 4.5f + (float)depth * 0.9f);
                int r = 6  + depth * 2 + (int)(wave * 3.0f);
                int g = 13 + depth * 3 + (int)(wave * 5.0f);
                int b = 26 + depth * 5 + (int)(wave * 9.0f);

                if (flash_intensity > 0.02f && wave > 0.1f) {
                    float dist_x = fabsf((float)x - (float)strike_x) / (float)pix_w;
                    float refl = flash_intensity * (1.0f - dist_x * 0.8f) * wave * 0.65f;
                    if (refl > 0.0f) {
                        r += (int)(refl * 130.0f);
                        g += (int)(refl * 150.0f);
                        b += (int)(refl * 195.0f);
                    }
                }

                float dx = (float)x - beam_cx;
                if (dx * sweep_dir > 0.0f && wave > -0.2f) {
                    float dist_x = fabsf(dx);
                    if (dist_x < max_beam * 0.8f) {
                        float w_refl = (1.0f - dist_x / (max_beam * 0.8f)) * fabsf(sweep_dir) * (wave + 0.5f) * 0.28f;
                        r += (int)(w_refl * 220.0f);
                        g += (int)(w_refl * 185.0f);
                        b += (int)(w_refl * 90.0f);
                    }
                }

                FB(y, x) = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
            }
        }

        // 7. Render Foreground Rocky Headland, Keeper's Cottage & Scaled Lighthouse
        for (int x = cliff_start_x; x < pix_w; x++) {
            int cliff_y = headland_top_y(x, cliff_start_x, sea_y);
            for (int y = cliff_y; y < pix_h; y++) {
                if (y < 0) continue;
                int r = 10, g = 13, b = 20;
                if (y == cliff_y && flash_intensity > 0.02f) {
                    r += (int)(flash_intensity * 65.0f);
                    g += (int)(flash_intensity * 78.0f);
                    b += (int)(flash_intensity * 105.0f);
                }
                FB(y, x) = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
            }
        }

        // Keeper's cottage scaled next to the lighthouse base
        int cot_x = lh_x - (int)(5.0f * ((float)pix_w / 80.0f));
        int cot_w = (int)(2.0f * ((float)pix_w / 80.0f));
        if (cot_w < 2) cot_w = 2;
        int cot_h = (int)(4.0f * h_scale);
        if (cot_h < 3) cot_h = 3;
        int cot_bot = headland_top_y(cot_x, cliff_start_x, sea_y);
        int cot_top = cot_bot - cot_h;

        for (int y = cot_top; y <= cot_bot; y++) {
            if (y < 0 || y >= pix_h) continue;
            int roof_shrink = (y == cot_top) ? 1 : 0;
            for (int x = cot_x - cot_w + roof_shrink; x <= cot_x + cot_w - roof_shrink; x++) {
                if (x >= 0 && x < pix_w) {
                    FB(y, x) = (Color){14, 16, 24};
                }
            }
        }
        // Glowing amber cottage windows
        int win_y = cot_top + (cot_h / 2);
        if (win_y >= 0 && win_y < pix_h) {
            if (cot_x - 1 >= 0 && cot_x - 1 < pix_w) FB(win_y, cot_x - 1) = (Color){235, 165, 65};
            if (cot_x + 1 >= 0 && cot_x + 1 < pix_w) FB(win_y, cot_x + 1) = (Color){235, 165, 65};
        }

        // Lighthouse tower scaled proportionally
        int base_half_w = (int)(2.0f * ((float)pix_w / 80.0f));
        if (base_half_w < 1) base_half_w = 1;

        for (int y = lh_top_y; y <= lh_bot_y; y++) {
            if (y < 0 || y >= pix_h) continue;
            float norm_y = (float)(y - lh_top_y) / (float)lh_height; // 0.0 at peak .. 1.0 at base

            int half_w = base_half_w;
            if (norm_y < 0.08f)      half_w = 0;                           // Cupola tip
            else if (norm_y < 0.26f) half_w = (base_half_w > 1) ? base_half_w - 1 : 1; // Lantern room
            else if (norm_y < 0.34f) half_w = base_half_w;                 // Gallery deck
            else if (norm_y < 0.65f) half_w = (base_half_w > 1) ? base_half_w - 1 : 1; // Upper shaft
            else                     half_w = base_half_w;                 // Lower tower base

            for (int x = lh_x - half_w; x <= lh_x + half_w; x++) {
                if (x < 0 || x >= pix_w) continue;

                if (norm_y >= 0.12f && norm_y < 0.26f) {
                    int pulse = (int)(viewer_flare * 55.0f);
                    FB(y, x) = (Color){
                        255,
                        clamp_u8(215 + pulse),
                        clamp_u8(125 + pulse * 2)
                    };
                } else {
                    int band = (int)(norm_y * 5.0f);
                    int is_red_band = (band % 2 == 1);
                    int r = is_red_band ? 38 : 30;
                    int g = is_red_band ? 14 : 33;
                    int b = is_red_band ? 18 : 42;

                    if (x == lh_x - half_w && flash_intensity > 0.02f) {
                        r += (int)(flash_intensity * 115.0f);
                        g += (int)(flash_intensity * 130.0f);
                        b += (int)(flash_intensity * 165.0f);
                    }
                    if (norm_y >= 0.26f && norm_y < 0.34f) {
                        r += 28; g += 22; b += 10;
                    }
                    FB(y, x) = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
                }
            }
        }

        // 8. Update & Render Dynamic Wind-Driven Rain
        float wind_dx = -0.38f + sinf(t * 0.7f) * 0.12f;
        for (int i = 0; i < active_drops; i++) {
            drops[i].x += wind_dx * drops[i].speed;
            drops[i].y += drops[i].speed;

            if (drops[i].x < 0.0f) drops[i].x += (float)pix_w;
            if (drops[i].x >= (float)pix_w) drops[i].x -= (float)pix_w;

            int ix = (int)drops[i].x;
            int ground_y = headland_top_y(ix, cliff_start_x, sea_y);
            if (ground_y > sea_y + 3) ground_y = sea_y + 3;

            if (drops[i].y >= (float)ground_y) {
                if (drops[i].layer == 2) {
                    spawn_splash(drops[i].x, (float)(ground_y - 1));
                }
                drops[i].y = (float)(4 + (rand() % 10));
                drops[i].x = (float)(rand() % pix_w);
            }

            int x0 = (int)drops[i].x;
            int y0 = (int)drops[i].y;
            int x1 = (int)(drops[i].x - wind_dx * (1.2f + drops[i].layer * 0.6f));
            int y1 = (int)(drops[i].y - (1.0f + drops[i].layer * 0.8f));

            Color rain_col = {
                (uint8_t)(110 + drops[i].layer * 40),
                (uint8_t)(145 + drops[i].layer * 40),
                (uint8_t)(195 + drops[i].layer * 28)
            };
            float alpha = 0.26f + drops[i].layer * 0.22f;
            draw_line(x0, y0, x1, y1, rain_col, alpha);
        }

        // 9. Update & Render Splashes
        for (int i = 0; i < MAX_SPLASHES; i++) {
            if (splashes[i].life > 0) {
                int sx = (int)splashes[i].x;
                int sy = (int)splashes[i].y;
                if (sx >= 0 && sx < pix_w && sy >= 0 && sy < pix_h) {
                    Color sp_col = {185, 215, 250};
                    FB(sy, sx) = blend(FB(sy, sx), sp_col, 0.78f);
                }
                splashes[i].x += splashes[i].vx;
                splashes[i].y += splashes[i].vy;
                splashes[i].vy += 0.25f;
                splashes[i].life--;
            }
        }

        // 10. Convert Pixel Framebuffer into UTF-8 Half-Block ('▀') ANSI Stream
        char *p = out_buf;
        p += sprintf(p, "\033[H\033[1;36m%s v%s\033[0m [%dx%d px | %dx%d chars] - Resize window or Ctrl+C\033[K\n",
                     pname, pversion, pix_w, pix_h, term_w, term_h);

        for (int row = 0; row < term_h; row++) {
            int y_top = row * 2;
            int y_bot = row * 2 + 1;
            for (int col = 0; col < term_w; col++) {
                Color top = FB(y_top, col);
                Color bot = FB(y_bot, col);
                p += sprintf(p, "\033[38;2;%d;%d;%d;48;2;%d;%d;%dm▀",
                             top.r, top.g, top.b, bot.r, bot.g, bot.b);
            }
            p += sprintf(p, "\033[0m\n");
        }

        fwrite(out_buf, 1, (size_t)(p - out_buf), stdout);
        fflush(stdout);

        flash_intensity *= 0.76f;
        t += 0.045f;
        usleep(33000); // ~30 FPS
    }

    // Restore cursor, leave alternate screen buffer, and free heap memory
    printf("\033[0m\033[?25h\033[?1049l");
    free(fb);
    free(out_buf);
    printf("Exited %s v%s cleanly.\n", pname, pversion);
    return 0;
}