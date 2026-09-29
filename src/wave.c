#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <SDL.h>
#include "launcher.h"
#include <launcher_config.h>
#include "wave.h"
#include "debug.h"

extern Config config;
extern SDL_Renderer *renderer;
extern Geometry geo;

// An XMB-style animated background: a vertical color gradient with soft,
// glowing ribbons and thin strands of light drifting across the screen.
// The shapes are drawn with SDL_RenderGeometry, which needs SDL 2.0.18.

#define WAVE_SEGMENTS 96
#define PI_F 3.14159265f

// Base colors for each month when WaveColor=Auto (January first)
static const SDL_Color month_colors[12] = {
    {0x9D, 0xA3, 0xAB, 0xFF}, // Silver
    {0xC9, 0xA2, 0x27, 0xFF}, // Gold
    {0x8B, 0xC3, 0x4A, 0xFF}, // Light green
    {0xE0, 0x8B, 0xB1, 0xFF}, // Pink
    {0x3F, 0xA3, 0x4D, 0xFF}, // Green
    {0x8E, 0x5B, 0xB5, 0xFF}, // Purple
    {0x2B, 0xB3, 0xB8, 0xFF}, // Teal
    {0x2D, 0x6F, 0xD6, 0xFF}, // Blue
    {0x6A, 0x4B, 0xB0, 0xFF}, // Violet
    {0xC9, 0x8A, 0x2B, 0xFF}, // Amber
    {0x8B, 0x5A, 0x3C, 0xFF}, // Brown
    {0xC0, 0x39, 0x2B, 0xFF}  // Red
};

// Sky colors through the day for WaveColor=Sky: the gradient's top and bottom (horizon)
// colors at times of day, blended smoothly in between. Daytime colors are kept deep
// enough for white text to stay readable.
typedef struct {
    float hour;
    SDL_Color top;
    SDL_Color bottom;
} SkyKeyframe;

static const SkyKeyframe sky_keyframes[] = {
    { 0.0f, {0x0B, 0x10, 0x26, 0xFF}, {0x05, 0x07, 0x0F, 0xFF}}, // Night
    { 4.5f, {0x0E, 0x14, 0x33, 0xFF}, {0x0A, 0x0C, 0x1C, 0xFF}}, // Late night
    { 5.5f, {0x1B, 0x1F, 0x4B, 0xFF}, {0x3A, 0x2A, 0x5A, 0xFF}}, // Pre-dawn
    { 6.5f, {0x3A, 0x55, 0x9A, 0xFF}, {0xE0, 0x86, 0x62, 0xFF}}, // Dawn
    { 8.0f, {0x3D, 0x7C, 0xC9, 0xFF}, {0x7F, 0xB5, 0xE0, 0xFF}}, // Morning
    {12.0f, {0x1F, 0x6F, 0xC5, 0xFF}, {0x5F, 0xA8, 0xE0, 0xFF}}, // Midday
    {16.0f, {0x2F, 0x77, 0xC0, 0xFF}, {0xC8, 0xA6, 0x6A, 0xFF}}, // Afternoon
    {18.5f, {0x4E, 0x40, 0x8C, 0xFF}, {0xD8, 0x72, 0x3A, 0xFF}}, // Sunset
    {20.0f, {0x2A, 0x24, 0x62, 0xFF}, {0x5E, 0x33, 0x70, 0xFF}}, // Dusk
    {21.5f, {0x0E, 0x14, 0x33, 0xFF}, {0x08, 0x0A, 0x18, 0xFF}}, // Night
    {24.0f, {0x0B, 0x10, 0x26, 0xFF}, {0x05, 0x07, 0x0F, 0xFF}}  // Night (wraps to midnight)
};

static SDL_Color mix_colors(SDL_Color a, SDL_Color b, float t)
{
    return (SDL_Color) {
        (Uint8) ((float) a.r + ((float) b.r - (float) a.r) * t + 0.5f),
        (Uint8) ((float) a.g + ((float) b.g - (float) a.g) * t + 0.5f),
        (Uint8) ((float) a.b + ((float) b.b - (float) a.b) * t + 0.5f),
        0xFF
    };
}

// A function to get the sky gradient colors for an hour of the day (0-24)
static void sky_colors(float hour, SDL_Color *top, SDL_Color *bottom)
{
    size_t count = sizeof(sky_keyframes) / sizeof(sky_keyframes[0]);
    for (size_t i = 0; i + 1 < count; i++) {
        const SkyKeyframe *a = &sky_keyframes[i];
        const SkyKeyframe *b = &sky_keyframes[i + 1];
        if (hour >= a->hour && hour <= b->hour) {
            float t = (hour - a->hour) / (b->hour - a->hour);
            t = t * t * (3.0f - 2.0f * t); // Smoothstep for gentler transitions
            *top = mix_colors(a->top, b->top, t);
            *bottom = mix_colors(a->bottom, b->bottom, t);
            return;
        }
    }
    *top = sky_keyframes[0].top;
    *bottom = sky_keyframes[0].bottom;
}

// A ribbon: a band following a moving sine wave, which is brightest along its center
typedef struct {
    float base_y;      // Vertical center, fraction of screen height
    float amplitude;   // Fraction of screen height
    float frequency;   // Wave periods across the screen
    float speed;       // Radians per second
    float phase;
    float thickness;   // Half height of the band, fraction of screen height
    Uint8 alpha;       // Brightness at the center of the band
} Ribbon;

static const Ribbon ribbons[] = {
    {0.62f, 0.07f, 0.9f, 0.25f, 0.0f, 0.090f, 60},
    {0.60f, 0.06f, 1.2f, -0.18f, 1.7f, 0.060f, 45},
    {0.64f, 0.05f, 0.7f, 0.12f, 3.1f, 0.030f, 70},
    // Thin strands of light
    {0.61f, 0.075f, 0.9f, 0.25f, 0.35f, 0.0025f, 140},
    {0.63f, 0.065f, 1.1f, 0.21f, 0.9f, 0.0020f, 120},
    {0.59f, 0.070f, 0.8f, -0.16f, 2.2f, 0.0020f, 110},
    {0.65f, 0.060f, 1.3f, 0.19f, 4.0f, 0.0015f, 100}
};

// A function to calculate the brightness for the current time of day (dimmer at night)
static float time_of_day_brightness(const struct tm *now)
{
    if (!config.wave_time_of_day)
        return 1.0f;
    float hour = (float) now->tm_hour + (float) now->tm_min / 60.0f;
    float daylight = 0.5f + 0.5f * cosf(2.0f * PI_F * (hour - 13.0f) / 24.0f); // 1 at 1 pm, 0 at 1 am
    return 0.45f + 0.55f * daylight;
}

static SDL_Color scale_color(SDL_Color color, float factor, Uint8 alpha)
{
    return (SDL_Color) {
        (Uint8) fminf((float) color.r * factor, 255.0f),
        (Uint8) fminf((float) color.g * factor, 255.0f),
        (Uint8) fminf((float) color.b * factor, 255.0f),
        alpha
    };
}

#if SDL_VERSION_ATLEAST(2, 0, 18)
// A function to draw the vertical gradient
static void draw_gradient(SDL_Color top, SDL_Color bottom)
{
    float w = (float) geo.screen_width;
    float h = (float) geo.screen_height;
    SDL_Vertex vertices[4] = {
        {{0.0f, 0.0f}, top, {0.0f, 0.0f}},
        {{w, 0.0f}, top, {0.0f, 0.0f}},
        {{0.0f, h}, bottom, {0.0f, 0.0f}},
        {{w, h}, bottom, {0.0f, 0.0f}}
    };
    int indices[6] = {0, 1, 2, 1, 3, 2};
    SDL_RenderGeometry(renderer, NULL, vertices, 4, indices, 6);
}

// A function to draw a ribbon as three rows of vertices (transparent edges, bright center)
static void draw_ribbon(const Ribbon *ribbon, float seconds, SDL_Color color)
{
    SDL_Vertex vertices[(WAVE_SEGMENTS + 1) * 3];
    int indices[WAVE_SEGMENTS * 12];
    float w = (float) geo.screen_width;
    float h = (float) geo.screen_height;
    SDL_Color edge = color;
    edge.a = 0;
    SDL_Color center = color;
    center.a = ribbon->alpha;

    for (int i = 0; i <= WAVE_SEGMENTS; i++) {
        float u = (float) i / (float) WAVE_SEGMENTS;
        float angle = 2.0f * PI_F * ribbon->frequency * u + ribbon->speed * seconds + ribbon->phase;
        float y = h * (ribbon->base_y + ribbon->amplitude * sinf(angle) +
                  0.3f * ribbon->amplitude * sinf(2.3f * angle + 0.7f * seconds));

        // The band twists: its thickness varies along the wave
        float half = h * ribbon->thickness * (0.55f + 0.45f * sinf(1.7f * angle - 0.4f * seconds));
        float x = w * u;
        vertices[i * 3] = (SDL_Vertex) {{x, y - half}, edge, {0.0f, 0.0f}};
        vertices[i * 3 + 1] = (SDL_Vertex) {{x, y}, center, {0.0f, 0.0f}};
        vertices[i * 3 + 2] = (SDL_Vertex) {{x, y + half}, edge, {0.0f, 0.0f}};
    }
    int n = 0;
    for (int i = 0; i < WAVE_SEGMENTS; i++) {
        for (int row = 0; row < 2; row++) {
            int a = i * 3 + row, b = a + 1, c = a + 3, d = a + 4;
            indices[n++] = a; indices[n++] = c; indices[n++] = b;
            indices[n++] = b; indices[n++] = c; indices[n++] = d;
        }
    }
    SDL_RenderGeometry(renderer, NULL, vertices, (WAVE_SEGMENTS + 1) * 3, indices, n);
}
#endif

// A function to draw the wave background for the current frame
void draw_wave_background(Uint32 ticks)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    time_t t = time(NULL);
    struct tm *now = localtime(&t);
    float brightness = time_of_day_brightness(now);
    float seconds = (float) ticks / 1000.0f;

    // Save the renderer state, since the gradient and ribbons use their own blending
    SDL_BlendMode mode;
    SDL_GetRenderDrawBlendMode(renderer, &mode);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    if (config.wave_color_mode == WAVE_COLOR_SKY) {
        // The sky colors already get darker at night
        SDL_Color top, bottom;
        float hour = (float) now->tm_hour + (float) now->tm_min / 60.0f + (float) now->tm_sec / 3600.0f;
        sky_colors(hour, &top, &bottom);
        draw_gradient(top, bottom);
    }
    else {
        SDL_Color base = config.wave_color_mode == WAVE_COLOR_MONTH ? month_colors[now->tm_mon] : config.wave_color;
        draw_gradient(scale_color(base, 0.85f * brightness, 0xFF), scale_color(base, 0.25f * brightness, 0xFF));
    }

    // Additive blending makes overlapping ribbons glow
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    SDL_Color light = scale_color((SDL_Color) {0xFF, 0xFF, 0xFF, 0xFF}, 0.6f + 0.4f * brightness, 0xFF);
    for (size_t i = 0; i < sizeof(ribbons) / sizeof(ribbons[0]); i++)
        draw_ribbon(&ribbons[i], seconds, light);

    SDL_SetRenderDrawBlendMode(renderer, mode);
#else
    (void) ticks;
#endif
}

// A function to check whether the wave background can be drawn with this SDL version
bool wave_background_supported()
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_version linked;
    SDL_GetVersion(&linked);
    return SDL_VERSIONNUM(linked.major, linked.minor, linked.patch) >= SDL_VERSIONNUM(2, 0, 18);
#else
    return false;
#endif
}
