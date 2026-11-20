#ifndef PS3RECOMP_RSX_HUD_H
#define PS3RECOMP_RSX_HUD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "ps3emu/posix_win32.h"   /* pthread_mutex_* on SRWLOCK (Windows port) */
#else
#include <pthread.h>
#endif

#define RSX_HUD_W 275
#define RSX_HUD_H 130
#define RSX_HUD_MARGIN_X 16
#define RSX_HUD_MARGIN_Y 16
#define RSX_HUD_SAMPLES 120
#define RSX_HUD_REDRAW_US 33333ull

/* Compact 5x7 ASCII font (characters 32..126), 7 bytes per glyph, MSB at left (bit 4). */
static const uint8_t rsx_hud_font5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* '!' */
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, /* '"' */
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* '#' */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* '$' */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* '%' */
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* '&' */
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, /* '\'' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* '(' */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ')' */
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* '*' */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* ',' */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x06,0x06}, /* '.' */
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, /* '/' */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* '0' */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* '1' */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* '2' */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* '3' */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* '4' */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* '5' */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* '6' */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* '7' */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* '8' */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* '9' */
    {0x00,0x06,0x06,0x00,0x06,0x06,0x00}, /* ':' */
    {0x00,0x06,0x06,0x00,0x06,0x04,0x08}, /* ';' */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* '<' */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* '=' */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* '>' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* '?' */
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* '@' */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* 'A' */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* 'B' */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* 'C' */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* 'D' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* 'E' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* 'F' */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* 'G' */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* 'H' */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 'I' */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* 'J' */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* 'K' */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* 'L' */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* 'M' */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* 'N' */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 'O' */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* 'P' */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* 'Q' */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* 'R' */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* 'S' */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* 'T' */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 'U' */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* 'V' */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* 'W' */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* 'X' */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* 'Y' */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* 'Z' */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* '[' */
    {0x10,0x08,0x08,0x04,0x02,0x02,0x01}, /* '\' */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ']' */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* '_' */
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* 'a' */
    {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, /* 'b' */
    {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, /* 'c' */
    {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, /* 'd' */
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* 'e' */
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, /* 'f' */
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}, /* 'g' */
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, /* 'h' */
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* 'i' */
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* 'j' */
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* 'k' */
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 'l' */
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, /* 'm' */
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, /* 'n' */
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* 'o' */
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* 'p' */
    {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}, /* 'q' */
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* 'r' */
    {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, /* 's' */
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, /* 't' */
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, /* 'u' */
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* 'v' */
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* 'w' */
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* 'x' */
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* 'y' */
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* 'z' */
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* '{' */
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* '|' */
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* '}' */
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, /* '~' */
};

typedef struct rsx_hud_state {
    int enabled;
    int initialized;
    pthread_mutex_t mu;
    char title[64];
    char gpu[64];
    float history[RSX_HUD_SAMPLES];
    int history_head;
    unsigned int fps;
    float last_ms;
    unsigned long long start_us;
    unsigned long long last_present_us;
    unsigned long long last_draw_us;
    uint32_t pixels[RSX_HUD_W * RSX_HUD_H];
    int dirty;
    FILE* csv_fp;
} rsx_hud_state;

static inline uint32_t rsx_hud_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* Little-endian ABGR -> byte order R, G, B, A matching VK_FORMAT_R8G8B8A8_UNORM. */
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static inline void rsx_hud_fill_rect(uint32_t* dst, int x0, int y0, int x1, int y1, uint32_t col)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > RSX_HUD_W) x1 = RSX_HUD_W;
    if (y1 > RSX_HUD_H) y1 = RSX_HUD_H;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = dst + y * RSX_HUD_W;
        for (int x = x0; x < x1; x++) row[x] = col;
    }
}

static inline void rsx_hud_draw_rect(uint32_t* dst, int x0, int y0, int x1, int y1, uint32_t col)
{
    for (int x = x0; x < x1; x++) {
        if ((unsigned)x < RSX_HUD_W) {
            if ((unsigned)y0 < RSX_HUD_H) dst[y0 * RSX_HUD_W + x] = col;
            if ((unsigned)(y1 - 1) < RSX_HUD_H) dst[(y1 - 1) * RSX_HUD_W + x] = col;
        }
    }
    for (int y = y0; y < y1; y++) {
        if ((unsigned)y < RSX_HUD_H) {
            if ((unsigned)x0 < RSX_HUD_W) dst[y * RSX_HUD_W + x0] = col;
            if ((unsigned)(x1 - 1) < RSX_HUD_W) dst[y * RSX_HUD_W + (x1 - 1)] = col;
        }
    }
}

static inline void rsx_hud_draw_text(uint32_t* dst, int x, int y, const char* s, uint32_t col,
                                     int bold, int max_x)
{
    int cx = x;
    const int adv = bold ? 7 : 6;
    for (; *s; s++, cx += adv) {
        if (cx + adv > max_x) break;
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const uint8_t* rows = rsx_hud_font5x7[c - 32];
        for (int r = 0; r < 7; r++) {
            int py = y + r;
            if ((unsigned)py >= RSX_HUD_H) continue;
            uint8_t bits = rows[r];
            for (int col_i = 0; col_i < 5; col_i++) {
                if (!(bits & (1u << (4 - col_i)))) continue;
                int px = cx + col_i;
                if ((unsigned)px < (unsigned)max_x && px < RSX_HUD_W) {
                    dst[py * RSX_HUD_W + px] = col;
                    if (bold && px + 1 < max_x && px + 1 < RSX_HUD_W)
                        dst[py * RSX_HUD_W + px + 1] = col;
                }
            }
        }
    }
}

static inline uint32_t rsx_hud_ms_color(float ms)
{
    if (ms <= 18.0f) return rsx_hud_rgb(60, 255, 120);   /* Solid 60 FPS green */
    if (ms <= 24.0f) return rsx_hud_rgb(180, 255, 60);   /* Minor hitch lime */
    if (ms <= 35.0f) return rsx_hud_rgb(255, 180, 40);   /* Framedrop orange */
    return rsx_hud_rgb(255, 60, 60);                     /* Major stutter red */
}

static inline int rsx_hud_env_enabled(void)
{
    const char* e = getenv("PS3RECOMP_HUD");
    if (e && *e) return *e != '0';
    const char* m = getenv("MANGOHUD");
    return (m && *m == '1');
}

static inline void rsx_hud_init(rsx_hud_state* h, const char* title, const char* gpu_name,
                                unsigned long long now_us)
{
    memset(h, 0, sizeof *h);
    pthread_mutex_init(&h->mu, NULL);
    h->enabled = rsx_hud_env_enabled();
    h->initialized = 1;
    snprintf(h->title, sizeof h->title, "%s",
             (title && *title) ? title : "MLB The Show 16");
    if (gpu_name && *gpu_name)
        snprintf(h->gpu, sizeof h->gpu, "Vulkan | %s", gpu_name);
    else
        snprintf(h->gpu, sizeof h->gpu, "Vulkan");
    for (int i = 0; i < RSX_HUD_SAMPLES; i++) h->history[i] = 16.6f;
    h->fps = 0;
    h->last_ms = 16.6f;
    h->start_us = now_us;
    h->dirty = 1;

    const char* csv_env = getenv("PS3RECOMP_HUD_CSV");
    if (h->enabled && csv_env && *csv_env && strcmp(csv_env, "0") != 0) {
        char path[1024];
        size_t len = strlen(csv_env);
        if (len >= 4 && strcmp(csv_env + len - 4, ".csv") == 0) {
            snprintf(path, sizeof path, "%s", csv_env);
        } else {
            time_t now = time(NULL);
            struct tm tm_now;
#ifdef _WIN32
            localtime_s(&tm_now, &now);   /* MSVC CRT: no localtime_r (as cellRtc.c) */
#else
            localtime_r(&now, &tm_now);
#endif
            char stamp[32];
            strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm_now);
            snprintf(path, sizeof path, "%s/MyGameRecomp_%s.csv", csv_env, stamp);
        }
        h->csv_fp = fopen(path, "w");
        if (h->csv_fp) {
            setvbuf(h->csv_fp, NULL, _IOFBF, 16384);
            fprintf(h->csv_fp, "os,cpu,gpu,ram,kernel,gpu_driver,cpusched\n");
            fprintf(h->csv_fp, "linux,,%s,,,vulkan,\n", gpu_name ? gpu_name : "");
            fprintf(h->csv_fp, "fps,frametime,elapsed\n");
        }
    }
}

static inline void rsx_hud_shutdown(rsx_hud_state* h)
{
    if (!h->initialized) return;
    pthread_mutex_lock(&h->mu);
    if (h->csv_fp) {
        fclose(h->csv_fp);
        h->csv_fp = NULL;
    }
    h->enabled = 0;
    h->initialized = 0;
    pthread_mutex_unlock(&h->mu);
    pthread_mutex_destroy(&h->mu);
}

static inline void rsx_hud_set_fps(rsx_hud_state* h, unsigned int fps)
{
    if (!h->initialized || !h->enabled) return;
    pthread_mutex_lock(&h->mu);
    h->fps = fps;
    pthread_mutex_unlock(&h->mu);
}

static inline void rsx_hud_record_present(rsx_hud_state* h, unsigned long long now_us)
{
    if (!h->initialized || !h->enabled) return;
    pthread_mutex_lock(&h->mu);
    if (h->last_present_us > 0 && now_us >= h->last_present_us) {
        float ms = (float)(now_us - h->last_present_us) / 1000.0f;
        if (ms < 0.1f) ms = 0.1f;
        if (ms > 9999.0f) ms = 9999.0f;
        h->history[h->history_head] = ms;
        h->history_head = (h->history_head + 1) % RSX_HUD_SAMPLES;
        h->last_ms = ms;
        if (h->csv_fp) {
            unsigned long long elapsed_ns = (now_us >= h->start_us)
                                            ? (now_us - h->start_us) * 1000ull
                                            : 0ull;
            fprintf(h->csv_fp, "%.2f,%.3f,%llu\n", 1000.0f / ms, ms, elapsed_ns);
        }
    }
    h->last_present_us = now_us;
    pthread_mutex_unlock(&h->mu);
}

/* Redraw HUD pixel buffer if due (>= 33.3 ms or first frame). Returns 1 if stage_dst was updated. */
static inline int rsx_hud_render_if_due(rsx_hud_state* h, unsigned long long now_us, void* stage_dst)
{
    if (!h->initialized || !h->enabled || !stage_dst) return 0;

    float history[RSX_HUD_SAMPLES];
    int head;
    unsigned int fps;
    float last_ms;
    char title[64];
    char gpu[64];

    pthread_mutex_lock(&h->mu);
    if (!h->dirty && h->last_draw_us != 0 && now_us - h->last_draw_us < RSX_HUD_REDRAW_US) {
        pthread_mutex_unlock(&h->mu);
        return 0;
    }
    h->last_draw_us = now_us;
    h->dirty = 0;
    memcpy(history, h->history, sizeof history);
    head = h->history_head;
    fps = h->fps;
    last_ms = h->last_ms;
    snprintf(title, sizeof title, "%s", h->title);
    snprintf(gpu, sizeof gpu, "%s", h->gpu);
    pthread_mutex_unlock(&h->mu);

    uint32_t* px = h->pixels;
    rsx_hud_fill_rect(px, 0, 0, RSX_HUD_W, RSX_HUD_H, rsx_hud_rgb(18, 20, 24));
    rsx_hud_draw_rect(px, 0, 0, RSX_HUD_W, RSX_HUD_H, rsx_hud_rgb(55, 62, 72));

    rsx_hud_draw_text(px, 10, 7, title, rsx_hud_rgb(160, 170, 185), 0, RSX_HUD_W - 8);

    rsx_hud_draw_text(px, 10, 25, "FPS", rsx_hud_rgb(220, 225, 235), 1, RSX_HUD_W - 8);
    char fps_str[32];
    if (!fps && last_ms > 0.1f) fps = (unsigned int)(1000.0f / last_ms + 0.5f);
    snprintf(fps_str, sizeof fps_str, "%u.0", fps);
    rsx_hud_draw_text(px, 40, 25, fps_str, rsx_hud_rgb(60, 255, 120), 1, 86);

    rsx_hud_draw_text(px, 86, 25, "TIME", rsx_hud_rgb(220, 225, 235), 1, RSX_HUD_W - 8);
    char ms_str[32];
    snprintf(ms_str, sizeof ms_str, "%.1fms", last_ms);
    rsx_hud_draw_text(px, 120, 25, ms_str, rsx_hud_rgb(0, 215, 255), 1, 176);

    float min_ms = 999.0f;
    float max_ms = 0.0f;
    int valid = 0;
    for (int i = 0; i < RSX_HUD_SAMPLES; i++) {
        float ms = history[i];
        if (ms <= 0.0f) continue;
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        valid++;
    }
    if (!valid) {
        min_ms = last_ms;
        max_ms = last_ms;
    }

    rsx_hud_draw_text(px, 178, 21, "min", rsx_hud_rgb(140, 155, 175), 0, 200);
    rsx_hud_draw_text(px, 178, 33, "max", rsx_hud_rgb(140, 155, 175), 0, 200);

    char val_str[32];
    snprintf(val_str, sizeof val_str, "%.1fms", min_ms);
    rsx_hud_draw_text(px, 202, 21, val_str, rsx_hud_rgb(60, 255, 120), 0, RSX_HUD_W - 6);
    snprintf(val_str, sizeof val_str, "%.1fms", max_ms);
    rsx_hud_draw_text(px, 202, 33, val_str, rsx_hud_ms_color(max_ms), 0, RSX_HUD_W - 6);

    rsx_hud_draw_text(px, 10, 50, gpu, rsx_hud_rgb(140, 150, 165), 0, RSX_HUD_W - 8);

    const int gx = 10;
    const int gy = 70;
    const int gw = RSX_HUD_W - 20;
    const int gh = 48;
    rsx_hud_fill_rect(px, gx, gy, gx + gw, gy + gh, rsx_hud_rgb(10, 12, 15));
    rsx_hud_draw_rect(px, gx, gy, gx + gw, gy + gh, rsx_hud_rgb(42, 48, 58));

    float scale_max = max_ms;
    if (scale_max < 18.0f) scale_max = 18.0f;

    const int bar_w = 2;
    for (int i = 0; i < RSX_HUD_SAMPLES && (i * bar_w) < (gw - 2); i++) {
        int idx = (head + i) % RSX_HUD_SAMPLES;
        float ms = history[idx];
        if (ms <= 0.0f) continue;
        int bar_h = (int)((ms / scale_max) * (float)(gh - 3));
        if (bar_h > gh - 3) bar_h = gh - 3;
        if (bar_h < 1) bar_h = 1;
        rsx_hud_fill_rect(px,
                          gx + 1 + i * bar_w, gy + gh - 1 - bar_h,
                          gx + 1 + (i + 1) * bar_w, gy + gh - 1,
                          rsx_hud_ms_color(ms));
    }

    int target_y = gy + gh - (int)((float)(gh - 2) * (16.6f / scale_max)) - 1;
    if (target_y > gy && target_y < gy + gh - 1) {
        uint32_t dot_col = rsx_hud_rgb(50, 65, 80);
        for (int x = gx + 1; x < gx + gw - 1; x += 3) {
            px[target_y * RSX_HUD_W + x] = dot_col;
        }
        int lbl_y = target_y - 9;
        if (lbl_y < gy + 2) lbl_y = target_y + 2;
        rsx_hud_draw_text(px, gx + gw - 40, lbl_y, "16.6ms", rsx_hud_rgb(75, 95, 115), 0, gx + gw - 2);
    }

    memcpy(stage_dst, px, sizeof h->pixels);
    return 1;
}

#endif /* PS3RECOMP_RSX_HUD_H */
