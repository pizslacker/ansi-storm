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
#include <time.h>
#include <unistd.h>

#define TERM_W 80
#define TERM_H 24
#define PIX_W  TERM_W
#define PIX_H  (TERM_H * 2) // 2 vertical pixels per terminal row using '▀'

#define MAX_DROPS    150
#define MAX_SPLASHES 48
#define MAX_BOLT_PTS 64

// Lighthouse coordinates in pixel space
#define LH_X      62
#define LH_TOP_Y  24
#define LH_BOT_Y  38
#define SEA_Y     42

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

static Color fb[PIX_H][PIX_W];
static Raindrop drops[MAX_DROPS];
static Splash splashes[MAX_SPLASHES];
static Segment bolt_segs[MAX_BOLT_PTS];
static int num_bolt_segs = 0;

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
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

// Procedural billowing cloud density for a given parallax layer
static float cloud_density(float x, float y, float offset, float scale_x, float center_y, float thickness)
{
    float wave = sinf((x + offset) * scale_x) * 2.2f
               + cosf((x + offset * 0.7f) * (scale_x * 2.3f)) * 1.4f
               + sinf((x - offset * 0.3f) * (scale_x * 4.1f)) * 0.6f;

    float top_edge = center_y - thickness + wave;
    float bot_edge = center_y + thickness * 0.55f + wave * 0.25f;

    if (y < top_edge || y > bot_edge) {
        return 0.0f;
    }

    float mid = (top_edge + bot_edge) * 0.5f;
    float half_span = (bot_edge - top_edge) * 0.5f;
    if (half_span <= 0.01f) return 0.0f;

    float dist = fabsf(y - mid) / half_span;
    float d = 1.0f - dist * dist;
    return (d > 0.0f) ? d : 0.0f;
}

// Procedural ridge height for parallax mountain layers
static float mountain_ridge_y(float x, float offset, int layer)
{
    if (layer == 0) {
        // Distant high peaks
        return 30.5f
             - sinf((x + offset) * 0.065f) * 5.0f
             - cosf((x + offset) * 0.17f)  * 2.6f
             - sinf((x + offset) * 0.39f)  * 1.1f;
    } else {
        // Closer rugged coastal hills
        return 35.0f
             - sinf((x + offset + 25.0f) * 0.09f) * 4.2f
             - cosf((x + offset) * 0.23f)         * 2.0f
             - sinf((x + offset) * 0.51f)         * 0.8f;
    }
}

// Foreground rocky headland height on the right side (supports the lighthouse)
static int headland_top_y(int x)
{
    if (x < 46) return PIX_H;
    float norm = (float)(x - 46) / 14.0f;
    if (norm > 1.0f) norm = 1.0f;
    float roughness = sinf((float)x * 0.6f) * 0.7f + cosf((float)x * 1.3f) * 0.4f;
    return (int)(44.0f - norm * 6.5f + roughness);
}

// Draw a line segment into the pixel framebuffer (Bresenham)
static void draw_line(int x0, int y0, int x1, int y1, Color c, float alpha)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        if (x0 >= 0 && x0 < PIX_W && y0 >= 0 && y0 < PIX_H) {
            fb[y0][x0] = blend(fb[y0][x0], c, alpha);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Spawn a jagged lightning bolt in the sky
static void spawn_lightning(int *strike_x)
{
    num_bolt_segs = 0;
    // Prefer striking over the sea/mountains to the left of the lighthouse
    int x = 8 + rand() % (PIX_W - 28);
    int y = 3;
    *strike_x = x;

    while (y < SEA_Y - 4 && num_bolt_segs < MAX_BOLT_PTS - 4) {
        int nx = x + (rand() % 9) - 4;
        int ny = y + 2 + (rand() % 4);
        bolt_segs[num_bolt_segs++] = (Segment){x, y, nx, ny};

        // 32% chance to spawn a secondary fork
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

// Spawn a splash at a specific (x, y) collision point
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
    const char *pname = "ansi-storm-demo"; // Pointer to program name
    const char *pversion = "1.1";          // Pointer to program version

    srand((unsigned int)time(NULL));
    signal(SIGINT, handle_sigint);

    // Initialize rain particles across 3 depth layers
    for (int i = 0; i < MAX_DROPS; i++) {
        drops[i].x = (float)(rand() % PIX_W);
        drops[i].y = (float)(rand() % PIX_H);
        drops[i].layer = i % 3;
        drops[i].speed = 0.85f + (float)drops[i].layer * 0.45f + ((float)(rand() % 100) / 400.0f);
    }

    // Clear screen and hide cursor
    printf("\033[2J\033[?25l");

    float t = 0.0f;
    float flash_intensity = 0.0f;
    int strike_x = PIX_W / 3;

    // Frame output buffer for single fwrite() call
    static char out_buf[TERM_W * TERM_H * 48 + 512];

    while (keep_running) {
        // Trigger random lightning strikes
        if (flash_intensity <= 0.05f && (rand() % 1000) < 28) {
            flash_intensity = 1.0f;
            spawn_lightning(&strike_x);
        }

        // Lighthouse rotating beacon phase:
        // sweep_dir in [-1, 1] is the horizontal direction of the beam
        // viewer_flare peaks when the beam points directly toward the camera
        float beacon_phase = t * 1.7f;
        float sweep_dir = cosf(beacon_phase);
        float toward_viewer = sinf(beacon_phase);
        float viewer_flare = (toward_viewer > 0.0f) ? powf(toward_viewer, 6.0f) : 0.0f;

        // 1. Render Night Sky Gradient + Lightning Flash
        for (int y = 0; y < PIX_H; y++) {
            float v = (float)y / (float)SEA_Y;
            if (v > 1.0f) v = 1.0f;
            for (int x = 0; x < PIX_W; x++) {
                int r = (int)(7.0f  + v * 15.0f);
                int g = (int)(11.0f + v * 22.0f);
                int b = (int)(24.0f + v * 38.0f);

                if (flash_intensity > 0.01f) {
                    float dist_x = fabsf((float)x - (float)strike_x) / (float)PIX_W;
                    float glow = flash_intensity * (1.0f - dist_x * 0.85f);
                    if (glow < 0.0f) glow = 0.0f;
                    r += (int)(glow * 140.0f);
                    g += (int)(glow * 155.0f);
                    b += (int)(glow * 195.0f);
                }

                fb[y][x] = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
            }
        }

        // 2. Render 3 Parallax Cloud Layers (Far -> Mid -> Near)
        const float speeds[3]    = { 3.2f,  7.5f,  15.0f };
        const float scales[3]    = { 0.09f, 0.12f, 0.16f };
        const float centers_y[3] = { 7.5f,  12.0f, 16.5f };
        const float thickness[3] = { 4.2f,  5.2f,  6.2f  };

        for (int layer = 0; layer < 3; layer++) {
            float offset = t * speeds[layer] + layer * 37.0f;

            for (int y = 0; y < SEA_Y - 6; y++) {
                for (int x = 0; x < PIX_W; x++) {
                    float d = cloud_density((float)x, (float)y, offset,
                                            scales[layer], centers_y[layer], thickness[layer]);
                    if (d <= 0.01f) continue;

                    float shade = 1.0f - ((float)y / 26.0f);
                    if (shade < 0.1f) shade = 0.1f;

                    int base_c = 20 + (2 - layer) * 13;
                    int r = base_c + (int)(shade * 26.0f);
                    int g = base_c + 4 + (int)(shade * 30.0f);
                    int b = base_c + 14 + (int)(shade * 42.0f);

                    if (flash_intensity > 0.01f) {
                        float dist_x = fabsf((float)x - (float)strike_x) / (float)PIX_W;
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
                    fb[y][x] = blend(fb[y][x], cloud_col, alpha);
                }
            }
        }

        // 3. Render Lightning Bolt Segments (behind mountains)
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

        // 4. Render 2 Parallax Mountain Ranges with Lightning Rim-Lighting
        const float mtn_speeds[2] = { 0.8f, 2.1f };
        for (int mlayer = 0; mlayer < 2; mlayer++) {
            float m_offset = t * mtn_speeds[mlayer];
            for (int x = 0; x < PIX_W; x++) {
                float ry = mountain_ridge_y((float)x, m_offset, mlayer);
                int top_y = (int)ry;
                if (top_y < 0) top_y = 0;

                for (int y = top_y; y < SEA_Y; y++) {
                    float depth_fade = (float)(y - top_y) / 14.0f;
                    if (depth_fade > 1.0f) depth_fade = 1.0f;

                    // Far mountains are hazier blue-slate; near mountains are darker charcoal-blue
                    int r = (mlayer == 0) ? (16 - (int)(depth_fade * 5.0f)) : (10 - (int)(depth_fade * 4.0f));
                    int g = (mlayer == 0) ? (22 - (int)(depth_fade * 6.0f)) : (14 - (int)(depth_fade * 5.0f));
                    int b = (mlayer == 0) ? (38 - (int)(depth_fade * 8.0f)) : (25 - (int)(depth_fade * 6.0f));

                    // Electric rim-highlight along the mountain ridge line during lightning
                    if (flash_intensity > 0.02f) {
                        float dist_x = fabsf((float)x - (float)strike_x) / (float)PIX_W;
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

                    fb[y][x] = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
                }
            }
        }

        // 5. Render Volumetric Rotating Lighthouse Beam
        float beam_cx = (float)LH_X;
        float beam_cy = (float)(LH_TOP_Y + 2);
        Color beam_col = {255, 232, 155}; // Warm sodium-vapor gold

        for (int y = 4; y < SEA_Y + 2; y++) {
            for (int x = 0; x < PIX_W; x++) {
                float dx = (float)x - beam_cx;
                float dy = (float)y - beam_cy;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist < 1.0f || dist > 68.0f) continue;

                // Primary directional cone (sweeping left and right across the horizon)
                float side_match = (dx * sweep_dir > 0.0f) ? fabsf(sweep_dir) : 0.0f;
                if (side_match > 0.02f && fabsf(dx) > 0.5f) {
                    float slope = fabsf(dy + 0.04f * dist) / fabsf(dx);
                    float cone_half_width = 0.16f + (1.0f - side_match) * 0.12f;
                    if (slope < cone_half_width) {
                        float angular = 1.0f - (slope / cone_half_width);
                        float radial  = 1.0f - (dist / 68.0f);
                        float b_alpha = angular * angular * radial * side_match * 0.58f;
                        fb[y][x] = add_light(fb[y][x], beam_col, b_alpha);
                    }
                }

                // Radial halo flare when the lamp rotates directly toward the viewer
                if (viewer_flare > 0.01f && dist < 18.0f) {
                    float halo = (1.0f - dist / 18.0f);
                    float h_alpha = halo * halo * viewer_flare * 0.75f;
                    fb[y][x] = add_light(fb[y][x], (Color){255, 248, 210}, h_alpha);
                }
            }
        }

        // 6. Render Stormy Sea & Reflections (y = SEA_Y .. PIX_H - 1)
        for (int y = SEA_Y; y < PIX_H; y++) {
            int depth = y - SEA_Y;
            for (int x = 0; x < PIX_W; x++) {
                float wave = sinf((float)x * 0.35f + t * 4.5f + (float)depth * 0.9f);
                int r = 6  + depth * 2 + (int)(wave * 3.0f);
                int g = 13 + depth * 3 + (int)(wave * 5.0f);
                int b = 26 + depth * 5 + (int)(wave * 9.0f);

                // Lightning shimmer on wave crests
                if (flash_intensity > 0.02f && wave > 0.1f) {
                    float dist_x = fabsf((float)x - (float)strike_x) / (float)PIX_W;
                    float refl = flash_intensity * (1.0f - dist_x * 0.8f) * wave * 0.65f;
                    if (refl > 0.0f) {
                        r += (int)(refl * 130.0f);
                        g += (int)(refl * 150.0f);
                        b += (int)(refl * 195.0f);
                    }
                }

                // Golden reflection of the sweeping lighthouse beam on the water
                float dx = (float)x - beam_cx;
                if (dx * sweep_dir > 0.0f && wave > -0.2f) {
                    float dist_x = fabsf(dx);
                    if (dist_x < 55.0f) {
                        float w_refl = (1.0f - dist_x / 55.0f) * fabsf(sweep_dir) * (wave + 0.5f) * 0.28f;
                        r += (int)(w_refl * 220.0f);
                        g += (int)(w_refl * 185.0f);
                        b += (int)(w_refl * 90.0f);
                    }
                }

                fb[y][x] = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
            }
        }

        // 7. Render Foreground Rocky Headland, Keeper's Cottage & Lighthouse Tower
        for (int x = 46; x < PIX_W; x++) {
            int cliff_y = headland_top_y(x);
            for (int y = cliff_y; y < PIX_H; y++) {
                int r = 10, g = 13, b = 20;
                // Subtle rock strata texture + lightning rim on left cliff slope
                if (y == cliff_y && flash_intensity > 0.02f) {
                    r += (int)(flash_intensity * 65.0f);
                    g += (int)(flash_intensity * 78.0f);
                    b += (int)(flash_intensity * 105.0f);
                }
                fb[y][x] = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
            }
        }

        // Keeper's cottage silhouette just left of the tower (x = 55..59, y = 34..38)
        for (int y = 34; y <= 38; y++) {
            int half_roof = (y == 34) ? 1 : 2;
            for (int x = 57 - half_roof; x <= 57 + half_roof; x++) {
                fb[y][x] = (Color){14, 16, 24};
            }
        }
        // Warm glowing cottage windows
        fb[36][56] = (Color){235, 165, 65};
        fb[36][58] = (Color){235, 165, 65};

        // Lighthouse tower (y = LH_TOP_Y .. LH_BOT_Y, centered at LH_X)
        for (int y = LH_TOP_Y; y <= LH_BOT_Y; y++) {
            int ry = y - LH_TOP_Y; // 0 at roof peak .. 14 at cliff base
            int half_w = 2;
            if (ry == 0)      half_w = 0; // Cupola tip
            else if (ry == 1) half_w = 1; // Dome roof
            else if (ry <= 3) half_w = 1; // Glass lantern room
            else if (ry == 4) half_w = 2; // Gallery deck railing
            else if (ry <= 8) half_w = 1; // Upper tapered tower
            else              half_w = 2; // Lower tower base

            for (int x = LH_X - half_w; x <= LH_X + half_w; x++) {
                if (ry == 2 || ry == 3) {
                    // Lantern room blazing core
                    int pulse = (int)(viewer_flare * 55.0f);
                    fb[y][x] = (Color){
                        255,
                        clamp_u8(215 + pulse),
                        clamp_u8(125 + pulse * 2)
                    };
                } else {
                    // Classic red-and-white lighthouse bands (darkened in night silhouette)
                    int is_red_band = ((ry / 3) % 2 == 1);
                    int r = is_red_band ? 38 : 30;
                    int g = is_red_band ? 14 : 33;
                    int b = is_red_band ? 18 : 42;

                    // Rim-light the left edge of the tower during lightning flashes
                    if (x == LH_X - half_w && flash_intensity > 0.02f) {
                        r += (int)(flash_intensity * 115.0f);
                        g += (int)(flash_intensity * 130.0f);
                        b += (int)(flash_intensity * 165.0f);
                    }
                    // Warm spill from the lantern onto the gallery deck
                    if (ry == 4) {
                        r += 28; g += 22; b += 10;
                    }
                    fb[y][x] = (Color){clamp_u8(r), clamp_u8(g), clamp_u8(b)};
                }
            }
        }

        // 8. Update & Render Dynamic Wind-Driven Rain
        float wind_dx = -0.38f + sinf(t * 0.7f) * 0.12f;
        for (int i = 0; i < MAX_DROPS; i++) {
            drops[i].x += wind_dx * drops[i].speed;
            drops[i].y += drops[i].speed;

            if (drops[i].x < 0.0f) drops[i].x += (float)PIX_W;
            if (drops[i].x >= (float)PIX_W) drops[i].x -= (float)PIX_W;

            // Collide foreground drops with the headland cliff or the sea surface
            int ix = (int)drops[i].x;
            int ground_y = headland_top_y(ix);
            if (ground_y > SEA_Y + 3) ground_y = SEA_Y + 3;

            if (drops[i].y >= (float)ground_y) {
                if (drops[i].layer == 2) {
                    spawn_splash(drops[i].x, (float)(ground_y - 1));
                }
                drops[i].y = (float)(8 + (rand() % 10));
                drops[i].x = (float)(rand() % PIX_W);
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

        // 9. Update & Render Splashes on Sea and Cliff Rocks
        for (int i = 0; i < MAX_SPLASHES; i++) {
            if (splashes[i].life > 0) {
                int sx = (int)splashes[i].x;
                int sy = (int)splashes[i].y;
                if (sx >= 0 && sx < PIX_W && sy >= 0 && sy < PIX_H) {
                    Color sp_col = {185, 215, 250};
                    fb[sy][sx] = blend(fb[sy][sx], sp_col, 0.78f);
                }
                splashes[i].x += splashes[i].vx;
                splashes[i].y += splashes[i].vy;
                splashes[i].vy += 0.25f;
                splashes[i].life--;
            }
        }

        // 10. Convert Pixel Framebuffer into UTF-8 Half-Block ('▀') ANSI Stream
        char *p = out_buf;
        p += sprintf(p, "\033[H\033[1;36m%s v%s\033[0m - Coastal Lighthouse & Parallax Storm (Ctrl+C to quit)\n",
                     pname, pversion);

        for (int row = 0; row < TERM_H; row++) {
            int y_top = row * 2;
            int y_bot = row * 2 + 1;
            for (int col = 0; col < TERM_W; col++) {
                Color top = fb[y_top][col];
                Color bot = fb[y_bot][col];
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

    // Restore cursor and terminal colors on exit
    printf("\033[0m\033[?25h\nExited %s cleanly.\n", pname);
    return 0;
}