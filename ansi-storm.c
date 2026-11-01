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

#define TERM_W 132
#define TERM_H 48
#define PIX_W  TERM_W
#define PIX_H  (TERM_H * 2) // 2 vertical pixels per terminal row using '▀'

#define MAX_DROPS    140
#define MAX_SPLASHES 40
#define MAX_BOLT_PTS 64

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
    Color out;
    out.r = clamp_u8((int)(bg.r * (1.0f - alpha) + fg.r * alpha));
    out.g = clamp_u8((int)(bg.g * (1.0f - alpha) + fg.g * alpha));
    out.b = clamp_u8((int)(bg.b * (1.0f - alpha) + fg.b * alpha));
    return out;
}

// Procedural billowing cloud density for a given parallax layer
static float cloud_density(float x, float y, float offset, float scale_x, float center_y, float thickness)
{
    // Combine harmonic sine waves to form rolling cloud tops and flat bottoms
    float wave = sinf((x + offset) * scale_x) * 2.2f
               + cosf((x + offset * 0.7f) * (scale_x * 2.3f)) * 1.4f
               + sinf((x - offset * 0.3f) * (scale_x * 4.1f)) * 0.6f;

    // Clouds puff upward more than downward
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

// Spawn a jagged lightning bolt with occasional side branches
static void spawn_lightning(int *strike_x)
{
    num_bolt_segs = 0;
    int x = 12 + rand() % (PIX_W - 24);
    int y = 4;
    *strike_x = x;

    while (y < PIX_H - 4 && num_bolt_segs < MAX_BOLT_PTS - 4) {
        int nx = x + (rand() % 9) - 4;
        int ny = y + 2 + (rand() % 4);
        bolt_segs[num_bolt_segs++] = (Segment){x, y, nx, ny};

        // 30% chance to spawn a secondary fork
        if ((rand() % 100) < 30 && num_bolt_segs < MAX_BOLT_PTS - 2) {
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

// Spawn a ground splash when a foreground raindrop hits the bottom
static void spawn_splash(float x)
{
    for (int i = 0; i < MAX_SPLASHES; i++) {
        if (splashes[i].life <= 0) {
            splashes[i].x = x;
            splashes[i].y = (float)(PIX_H - 2);
            splashes[i].vx = ((float)(rand() % 100) / 50.0f - 1.0f) * 0.7f;
            splashes[i].vy = -0.5f - ((float)(rand() % 100) / 250.0f);
            splashes[i].life = 3 + (rand() % 4);
            break;
        }
    }
}

int main(void)
{
    const char *pname = "ansi-storm-demo"; // Pointer to program name
    const char *pversion = "1.0";          // Pointer to program version

    srand((unsigned int)time(NULL));
    signal(SIGINT, handle_sigint);

    // Initialize rain particles across 3 depth layers
    for (int i = 0; i < MAX_DROPS; i++) {
        drops[i].x = (float)(rand() % PIX_W);
        drops[i].y = (float)(rand() % PIX_H);
        drops[i].layer = i % 3; // 0 = background, 1 = mid, 2 = foreground
        drops[i].speed = 0.85f + (float)drops[i].layer * 0.45f + ((float)(rand() % 100) / 400.0f);
    }

    // Clear screen and hide cursor
    printf("\033[2J\033[?25l");

    float t = 0.0f;
    float flash_intensity = 0.0f;
    int strike_x = PIX_W / 2;

    // Frame output buffer to write each frame in a single fwrite() call (prevents tearing)
    static char out_buf[TERM_W * TERM_H * 48 + 512];

    while (keep_running) {
        // Trigger random lightning strikes
        if (flash_intensity <= 0.05f && (rand() % 1000) < 28) {
            flash_intensity = 1.0f;
            spawn_lightning(&strike_x);
        }

        // 1. Render Night Sky Gradient + Lightning Flash
        for (int y = 0; y < PIX_H; y++) {
            float v = (float)y / (float)PIX_H;
            for (int x = 0; x < PIX_W; x++) {
                // Base dark stormy night gradient
                int r = (int)(8.0f  + v * 14.0f);
                int g = (int)(12.0f + v * 20.0f);
                int b = (int)(26.0f + v * 36.0f);

                // Add radial illumination from lightning bolt
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
        const float speeds[3]     = { 3.5f,  8.0f,  15.5f };
        const float scales[3]     = { 0.09f, 0.12f, 0.16f };
        const float centers_y[3]  = { 8.0f,  12.5f, 17.5f };
        const float thickness[3]  = { 4.5f,  5.5f,  6.5f  };

        for (int layer = 0; layer < 3; layer++) {
            float offset = t * speeds[layer] + layer * 37.0f;

            for (int y = 0; y < PIX_H - 12; y++) {
                for (int x = 0; x < PIX_W; x++) {
                    float d = cloud_density((float)x, (float)y, offset,
                                            scales[layer], centers_y[layer], thickness[layer]);
                    if (d <= 0.01f) continue;

                    // Vertical shading inside the cloud (brighter top, dark rain-heavy base)
                    float shade = 1.0f - ((float)y / 28.0f);
                    if (shade < 0.1f) shade = 0.1f;

                    // Closer layers are darker and more dramatic
                    int base_c = 22 + (2 - layer) * 14;
                    int r = base_c + (int)(shade * 26.0f);
                    int g = base_c + 4 + (int)(shade * 30.0f);
                    int b = base_c + 14 + (int)(shade * 42.0f);

                    // Backlight cloud edges when lightning flashes
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

        // 3. Render Lightning Bolt Segments (if active)
        if (flash_intensity > 0.15f) {
            Color glow_col = {150, 185, 255};
            Color core_col = {255, 255, 255};
            for (int i = 0; i < num_bolt_segs; i++) {
                Segment s = bolt_segs[i];
                // Outer electric-blue glow
                draw_line(s.x0 - 1, s.y0, s.x1 - 1, s.y1, glow_col, flash_intensity * 0.55f);
                draw_line(s.x0 + 1, s.y0, s.x1 + 1, s.y1, glow_col, flash_intensity * 0.55f);
                // Bright white core
                draw_line(s.x0, s.y0, s.x1, s.y1, core_col, flash_intensity);
            }
        }

        // 4. Update & Render Dynamic Wind-Driven Rain
        float wind_dx = -0.38f + sinf(t * 0.7f) * 0.12f; // Gusting wind angle
        for (int i = 0; i < MAX_DROPS; i++) {
            drops[i].x += wind_dx * drops[i].speed;
            drops[i].y += drops[i].speed;

            // Wrap around screen edges
            if (drops[i].x < 0.0f) drops[i].x += (float)PIX_W;
            if (drops[i].x >= (float)PIX_W) drops[i].x -= (float)PIX_W;

            if (drops[i].y >= (float)(PIX_H - 1)) {
                if (drops[i].layer == 2) {
                    spawn_splash(drops[i].x);
                }
                drops[i].y = (float)(10 + (rand() % 10));
                drops[i].x = (float)(rand() % PIX_W);
            }

            // Draw raindrop streak
            int x0 = (int)drops[i].x;
            int y0 = (int)drops[i].y;
            int x1 = (int)(drops[i].x - wind_dx * (1.2f + drops[i].layer * 0.6f));
            int y1 = (int)(drops[i].y - (1.0f + drops[i].layer * 0.8f));

            Color rain_col = {
                (uint8_t)(110 + drops[i].layer * 40),
                (uint8_t)(145 + drops[i].layer * 40),
                (uint8_t)(195 + drops[i].layer * 28)
            };
            float alpha = 0.28f + drops[i].layer * 0.24f;
            draw_line(x0, y0, x1, y1, rain_col, alpha);
        }

        // 5. Update & Render Ground Puddle & Splashes
        for (int x = 0; x < PIX_W; x++) {
            Color ground = {14, 20, 34};
            fb[PIX_H - 1][x] = blend(fb[PIX_H - 1][x], ground, 0.8f);
        }

        for (int i = 0; i < MAX_SPLASHES; i++) {
            if (splashes[i].life > 0) {
                int sx = (int)splashes[i].x;
                int sy = (int)splashes[i].y;
                if (sx >= 0 && sx < PIX_W && sy >= 0 && sy < PIX_H) {
                    Color sp_col = {180, 210, 245};
                    fb[sy][sx] = blend(fb[sy][sx], sp_col, 0.75f);
                }
                splashes[i].x += splashes[i].vx;
                splashes[i].y += splashes[i].vy;
                splashes[i].vy += 0.25f; // Gravity
                splashes[i].life--;
            }
        }

        // 6. Convert Pixel Framebuffer into UTF-8 Half-Block ('▀') ANSI Stream
        char *p = out_buf;
        p += sprintf(p, "\033[H\033[1;36m%s v%s\033[0m - 24-bit Parallax Storm (Press Ctrl+C to quit)\n",
                     pname, pversion);

        for (int row = 0; row < TERM_H; row++) {
            int y_top = row * 2;
            int y_bot = row * 2 + 1;
            for (int col = 0; col < TERM_W; col++) {
                Color top = fb[y_top][col];
                Color bot = fb[y_bot][col];
                // Foreground = top pixel, Background = bottom pixel, character = '▀'
                p += sprintf(p, "\033[38;2;%d;%d;%d;48;2;%d;%d;%dm▀",
                             top.r, top.g, top.b, bot.r, bot.g, bot.b);
            }
            p += sprintf(p, "\033[0m\n");
        }

        fwrite(out_buf, 1, (size_t)(p - out_buf), stdout);
        fflush(stdout);

        // Decay lightning flash and advance clock
        flash_intensity *= 0.76f;
        t += 0.045f;
        usleep(33000); // ~30 FPS
    }

    // Restore cursor and terminal colors on exit
    printf("\033[0m\033[?25h\nExited %s cleanly.\n", pname);
    return 0;
}