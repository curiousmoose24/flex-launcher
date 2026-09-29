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

#define MIN_WAVE_SEGMENTS 96
#define MAX_WAVE_SEGMENTS 512
#define WAVE_SEGMENT_WIDTH 10.0f  // Pixels per segment, so curves stay smooth on large screens
#define MIN_RIBBON_HALF 1.5f      // Pixels; thinner bands break up and shimmer, so they dim instead
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
    {13.0f, {0x14, 0x4D, 0x1E, 0xFF}, {0x22, 0x8B, 0x22, 0xFF}}, // Forest green (early afternoon)
    {15.0f, {0x14, 0x4D, 0x1E, 0xFF}, {0x22, 0x8B, 0x22, 0xFF}}, // Forest green
    {16.0f, {0x2F, 0x77, 0xC0, 0xFF}, {0xC8, 0xA6, 0x6A, 0xFF}}, // Afternoon
    {18.5f, {0x4E, 0x40, 0x8C, 0xFF}, {0xD8, 0x72, 0x3A, 0xFF}}, // Sunset
    {20.0f, {0x2A, 0x24, 0x62, 0xFF}, {0x5E, 0x33, 0x70, 0xFF}}, // Dusk
    {22.0f, {0x0E, 0x14, 0x33, 0xFF}, {0x08, 0x0A, 0x18, 0xFF}}, // Night
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
    {0.61f, 0.065f, 1.0f, -0.22f, 4.6f, 0.075f, 50},
    // Thin strands of light
    {0.61f, 0.075f, 0.9f, 0.25f, 0.35f, 0.0025f, 140},
    {0.63f, 0.065f, 1.1f, 0.21f, 0.9f, 0.0020f, 120},
    {0.59f, 0.070f, 0.8f, -0.16f, 2.2f, 0.0020f, 110}
};

// A function to calculate a ribbon's center line at horizontal position u (0-1), in pixels
static float ribbon_center(const Ribbon *ribbon, float u, float seconds, float *angle_out)
{
    float angle = 2.0f * PI_F * ribbon->frequency * u + ribbon->speed * seconds + ribbon->phase;
    if (angle_out != NULL)
        *angle_out = angle;
    return (float) geo.screen_height * (ribbon->base_y + ribbon->amplitude * sinf(angle) +
           0.3f * ribbon->amplitude * sinf(2.3f * angle + 0.7f * seconds));
}

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
    static SDL_Vertex vertices[(MAX_WAVE_SEGMENTS + 1) * 3];
    static int indices[MAX_WAVE_SEGMENTS * 12];
    float w = (float) geo.screen_width;
    float h = (float) geo.screen_height;
    int segments = (int) (w / WAVE_SEGMENT_WIDTH);
    if (segments < MIN_WAVE_SEGMENTS)
        segments = MIN_WAVE_SEGMENTS;
    else if (segments > MAX_WAVE_SEGMENTS)
        segments = MAX_WAVE_SEGMENTS;
    SDL_Color edge = color;
    edge.a = 0;
    SDL_Color center = color;

    for (int i = 0; i <= segments; i++) {
        float u = (float) i / (float) segments;
        float angle;
        float y = ribbon_center(ribbon, u, seconds, &angle);

        // The band twists: its thickness varies along the wave
        float half = h * ribbon->thickness * (0.55f + 0.45f * sinf(1.7f * angle - 0.4f * seconds));

        // Keep the band at least a few pixels wide, dimming it by as much as it was widened
        float alpha = (float) ribbon->alpha;
        if (half < MIN_RIBBON_HALF) {
            alpha *= half / MIN_RIBBON_HALF;
            half = MIN_RIBBON_HALF;
        }
        center.a = (Uint8) (alpha + 0.5f);
        float x = w * u;
        vertices[i * 3] = (SDL_Vertex) {{x, y - half}, edge, {0.0f, 0.0f}};
        vertices[i * 3 + 1] = (SDL_Vertex) {{x, y}, center, {0.0f, 0.0f}};
        vertices[i * 3 + 2] = (SDL_Vertex) {{x, y + half}, edge, {0.0f, 0.0f}};
    }
    int n = 0;
    for (int i = 0; i < segments; i++) {
        for (int row = 0; row < 2; row++) {
            int a = i * 3 + row, b = a + 1, c = a + 3, d = a + 4;
            indices[n++] = a; indices[n++] = c; indices[n++] = b;
            indices[n++] = b; indices[n++] = c; indices[n++] = d;
        }
    }
    SDL_RenderGeometry(renderer, NULL, vertices, (segments + 1) * 3, indices, n);
}
#endif

// Sparkles: soft specks of light scattered around the ribbons that drift, twinkle and fade,
// modeled on the particles of the PS3 slim's XMB (measured from a recording). They are born
// near the ribbons but move on their own: most drift slowly sideways, more often right than
// left, and live only a second or two. Some speed up smoothly and whoosh away, fading out or
// leaving the screen. Positions are worked out from the birth time, so there's no per-frame state.
#define NUM_SPARKLES 600
#define SPARKLE_REST 2.5f          // Average dark time between appearances, relative to the time shown
#define SPARKLE_FADE_TIME 0.1f      // Seconds to fade in and out (at most): quick, like a glint
#define SPARKLE_ZOOM_CHANCE 0.10f   // Fraction of sparkles that speed up and whoosh away
#define SPARKLE_LEAVE_CHANCE 0.2f   // Fraction of zooming sparkles that keep going until they leave the screen
#define SPARKLE_DEPTH_CHANCE 0.5f   // Chance that a long-lived sparkle moves towards or away from the viewer
#define SPARKLE_DEPTH_MIN_LIFE 2.0f // Seconds a sparkle must live to move in depth
#define SPARKLE_LEAVE_SPEED 1.0f    // Speed of the sparkles that leave the screen, relative to other zooming sparkles
#define SPARKLE_LEAVE_GROWTH 8.0f   // Final size of a sparkle that approaches while leaving the screen (others double)
#define SPARKLE_SPEED 0.046875f     // Scales all sparkle motion
#define SPARKLE_BLUR_TIME 0.035f    // A moving sparkle's streak shows where it was this long ago
#define SPARKLE_TEXTURE_SIZE 32
#define SPARKLE_GLOW_TEXTURE_SIZE 64
#define SPARKLE_GLOW_SCALE 5.0f     // Glow diameter relative to the speck
#define SPARKLE_GLOW_OPACITY 0.1125f // Glow brightness relative to the speck

typedef struct {
    float born;          // Seconds (in the future while it rests between appearances)
    float rest;          // Seconds it stays dark before appearing
    float life;          // Seconds
    float x;             // Birth position, fraction of screen width
    float y;             // Birth position, fraction of screen height
    float vx;            // Screen widths per second
    float vy;            // Screen widths per second
    float acceleration;  // Screen widths per second squared, along the direction of motion
    float size;          // Diameter, fraction of screen height
    float brightness;    // 0-1
    float twinkle_speed; // Radians per second
    float twinkle_phase;
    bool zoom;
    bool leaves;         // A zooming sparkle that doesn't fade, and flies off the screen
    int depth;           // 1: moves towards the viewer (grows to double size), -1: away (shrinks to half), 0: neither
    float shown;         // Seconds it's expected to be visible
} Sparkle;

static Sparkle sparkles[NUM_SPARKLES];
static bool sparkles_ready = false;
static SDL_Texture *sparkle_texture = NULL;
static SDL_Texture *sparkle_glow_texture = NULL;

static float random_float(float min, float max)
{
    return min + (max - min) * (float) rand() / (float) RAND_MAX;
}

// A random number from -1 to 1 that clusters around 0
static float random_centered()
{
    return (random_float(0.0f, 1.0f) + random_float(0.0f, 1.0f) + random_float(0.0f, 1.0f) - 1.5f) / 1.5f;
}

static void spawn_sparkle(Sparkle *sparkle, float seconds)
{
    sparkle->zoom = random_float(0.0f, 1.0f) < SPARKLE_ZOOM_CHANCE;

    // Most live about a second; a few linger up to three
    float r = random_float(0.0f, 1.0f);
    sparkle->leaves = sparkle->zoom && random_float(0.0f, 1.0f) < SPARKLE_LEAVE_CHANCE;
    sparkle->life = sparkle->leaves ? 60.0f : // Reborn when it leaves the screen
                    sparkle->zoom ? random_float(1.5f, 3.0f) : 0.5f + 2.5f * r * r;

    // Rest in the dark first, so each sparkle only shows now and then
    sparkle->shown = sparkle->leaves ? 18.0f : sparkle->life; // Leaving takes about 18 seconds
    sparkle->rest = random_float(0.0f, 2.0f * SPARKLE_REST) * sparkle->shown;

    // Long-lived sparkles may drift towards or away from the viewer
    sparkle->depth = 0;
    if (sparkle->shown >= SPARKLE_DEPTH_MIN_LIFE && random_float(0.0f, 1.0f) < SPARKLE_DEPTH_CHANCE)
        sparkle->depth = random_float(0.0f, 1.0f) < 0.5f ? 1 : -1;
    sparkle->born = seconds + sparkle->rest;

    // Born near the ribbons, clustered around their center
    sparkle->x = random_float(-0.02f, 1.02f);
    sparkle->y = ribbon_center(&ribbons[0], sparkle->x, sparkle->born, NULL) / (float) geo.screen_height +
                 random_centered() * 0.216f; // Up to 21.6% of the screen height above or below

    // Drift sideways, twice as often right as left; about a fifth barely move
    float direction = random_float(0.0f, 1.0f) < 0.67f ? 1.0f : -1.0f;
    float speed;
    if (sparkle->zoom)
        speed = random_float(0.015f, 0.04f);
    else if (random_float(0.0f, 1.0f) < 0.2f)
        speed = random_float(0.0f, 0.006f);
    else
        speed = 0.01f * expf(random_float(0.0f, 1.8f)); // 0.01-0.06, mostly slow
    sparkle->vx = SPARKLE_SPEED * direction * speed;
    sparkle->vy = SPARKLE_SPEED * random_centered() * 0.02f;
    sparkle->acceleration = sparkle->zoom ? SPARKLE_SPEED * random_float(0.12f, 0.3f) : 0.0f;
    if (sparkle->leaves) {
        sparkle->vx *= SPARKLE_LEAVE_SPEED;
        sparkle->vy *= SPARKLE_LEAVE_SPEED;
        sparkle->acceleration *= SPARKLE_LEAVE_SPEED;
    }

    sparkle->size = random_float(0.004f, 0.009f);
    sparkle->brightness = sparkle->zoom ? random_float(0.75f, 1.0f) : random_float(0.5f, 1.0f);
    sparkle->twinkle_speed = random_float(3.0f, 8.0f);
    sparkle->twinkle_phase = random_float(0.0f, 2.0f * PI_F);
}

// A function to get a sparkle's size at an age, relative to its size when born. A sparkle
// moving in depth travels at a steady speed, so with perspective (size = 1 / distance) one
// coming closer grows faster as it nears, reaching double size (eight times for one flying off
// the screen, which comes much closer), and one moving away shrinks
// ever more slowly to half size.
static float sparkle_scale(const Sparkle *sparkle, float age)
{
    if (sparkle->depth == 0)
        return 1.0f;
    float t = fminf(age / sparkle->shown, 1.0f);
    float growth = sparkle->leaves ? SPARKLE_LEAVE_GROWTH : 2.0f;
    float distance = sparkle->depth > 0 ? 1.0f - (1.0f - 1.0f / growth) * t : 1.0f + t;
    return 1.0f / distance;
}

// A function to get a sparkle's position at an age, in pixels
static void sparkle_position(const Sparkle *sparkle, float age, float *x, float *y)
{
    float w = (float) geo.screen_width;
    float dx = sparkle->vx * age;
    float dy = sparkle->vy * age;

    // Zooming sparkles speed up smoothly along their direction of motion
    if (sparkle->acceleration > 0.0f) {
        float speed = sqrtf(sparkle->vx * sparkle->vx + sparkle->vy * sparkle->vy);
        float extra = 0.5f * sparkle->acceleration * age * age;
        dx += extra * sparkle->vx / speed;
        dy += extra * sparkle->vy / speed;
    }
    *x = w * (sparkle->x + dx);
    *y = (float) geo.screen_height * sparkle->y + w * dy;
}

// A function to draw a moving sparkle's streak: a tapered wedge of light fading back along its path
static void draw_streak(const Sparkle *sparkle, float age, float x, float y, float size, float alpha, SDL_Color light)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    float tx, ty;
    sparkle_position(sparkle, fmaxf(age - SPARKLE_BLUR_TIME, 0.0f), &tx, &ty);
    float dx = x - tx, dy = y - ty;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 2.0f * size) // Too slow to blur
        return;
    float half = size * 0.3f;
    float px = -dy / length * half, py = dx / length * half;
    SDL_Color head = light, tail = light;
    head.a = (Uint8) (alpha * 0.45f);
    tail.a = 0;
    SDL_Vertex vertices[3] = {
        {{x + px, y + py}, head, {0.0f, 0.0f}},
        {{x - px, y - py}, head, {0.0f, 0.0f}},
        {{tx, ty}, tail, {0.0f, 0.0f}}
    };
    SDL_RenderGeometry(renderer, NULL, vertices, 3, NULL, 0);
#else
    (void) sparkle; (void) age; (void) x; (void) y; (void) size; (void) alpha; (void) light;
#endif
}

static void draw_speck(float x, float y, float size, float alpha)
{
    // The glow blooms with the speck as it twinkles
    if (sparkle_glow_texture != NULL) {
        float glow_size = size * SPARKLE_GLOW_SCALE;
        SDL_FRect glow_rect = {x - glow_size / 2.0f, y - glow_size / 2.0f, glow_size, glow_size};
        SDL_SetTextureAlphaMod(sparkle_glow_texture, (Uint8) (alpha * SPARKLE_GLOW_OPACITY));
        SDL_RenderCopyF(renderer, sparkle_glow_texture, NULL, &glow_rect);
    }
    SDL_FRect rect = {x - size / 2.0f, y - size / 2.0f, size, size};
    SDL_SetTextureAlphaMod(sparkle_texture, (Uint8) alpha);
    SDL_RenderCopyF(renderer, sparkle_texture, NULL, &rect);
}

// A function to create the soft round speck texture: a bright core with a glowing falloff
static SDL_Texture *create_sparkle_texture()
{
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, SPARKLE_TEXTURE_SIZE, SPARKLE_TEXTURE_SIZE, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == NULL)
        return NULL;
    Uint32 *pixels = surface->pixels;
    float radius = (float) SPARKLE_TEXTURE_SIZE / 2.0f;
    for (int y = 0; y < SPARKLE_TEXTURE_SIZE; y++) {
        for (int x = 0; x < SPARKLE_TEXTURE_SIZE; x++) {
            float dx = ((float) x + 0.5f - radius) / radius;
            float dy = ((float) y + 0.5f - radius) / radius;
            float d = sqrtf(dx * dx + dy * dy);
            float glow = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
            float core = d >= 0.45f ? 0.0f : 1.0f - d / 0.45f;
            float a = fminf(0.55f * glow + core, 1.0f);
            Uint8 alpha = (Uint8) (a * 255.0f + 0.5f);
            pixels[y * (surface->pitch / 4) + x] = SDL_MapRGBA(surface->format, 0xFF, 0xFF, 0xFF, alpha);
        }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (texture != NULL)
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    return texture;
}

// A function to create the sparkle glow texture: a wide, soft Gaussian halo
static SDL_Texture *create_sparkle_glow_texture()
{
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, SPARKLE_GLOW_TEXTURE_SIZE, SPARKLE_GLOW_TEXTURE_SIZE, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == NULL)
        return NULL;
    Uint32 *pixels = surface->pixels;
    float radius = (float) SPARKLE_GLOW_TEXTURE_SIZE / 2.0f;
    for (int y = 0; y < SPARKLE_GLOW_TEXTURE_SIZE; y++) {
        for (int x = 0; x < SPARKLE_GLOW_TEXTURE_SIZE; x++) {
            float dx = ((float) x + 0.5f - radius) / radius;
            float dy = ((float) y + 0.5f - radius) / radius;
            float d2 = dx * dx + dy * dy;
            // Fades to zero at the edge so the square texture never shows
            float a = d2 >= 1.0f ? 0.0f : expf(-6.0f * d2) * (1.0f - d2);
            pixels[y * (surface->pitch / 4) + x] = SDL_MapRGBA(surface->format, 0xFF, 0xFF, 0xFF, (Uint8) (a * 255.0f + 0.5f));
        }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (texture != NULL)
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    return texture;
}

// A function to draw the sparkles around the first (widest) ribbon
static void draw_sparkles(float seconds, SDL_Color light)
{
    if (sparkle_texture == NULL) {
        sparkle_texture = create_sparkle_texture();
        if (sparkle_texture == NULL)
            return;
        sparkle_glow_texture = create_sparkle_glow_texture();
    }
    if (!sparkles_ready) {
        // Stagger the first generation so they don't all appear at once
        for (int i = 0; i < NUM_SPARKLES; i++) {
            // Start at a random point in the rest-then-shine cycle
            spawn_sparkle(&sparkles[i], seconds);
            sparkles[i].born -= random_float(0.0f, sparkles[i].rest + fminf(sparkles[i].life, 3.0f));
        }
        sparkles_ready = true;
    }

    float w = (float) geo.screen_width;
    float h = (float) geo.screen_height;
    SDL_SetTextureColorMod(sparkle_texture, light.r, light.g, light.b);
    if (sparkle_glow_texture != NULL)
        SDL_SetTextureColorMod(sparkle_glow_texture, light.r, light.g, light.b);
    for (int i = 0; i < NUM_SPARKLES; i++) {
        Sparkle *sparkle = &sparkles[i];
        float age = seconds - sparkle->born;
        if (age < 0.0f) // Resting
            continue;
        if (age >= sparkle->life) {
            spawn_sparkle(sparkle, seconds);
            continue;
        }
        float x, y;
        sparkle_position(sparkle, age, &x, &y);
        float size = fmaxf(h * sparkle->size * sparkle_scale(sparkle, age), 3.0f);
        float margin = size * SPARKLE_GLOW_SCALE;
        if (x < -margin || x > w + margin || y < -margin || y > h + margin) {
            sparkle->life = age; // Gone off the screen: reborn next frame
            continue;
        }

        // Fade in and out at the ends of its life. A zooming sparkle fades out
        // over the last eighth of its life, so it dims as it speeds away, unless
        // it's one that leaves the screen.
        float fade = fminf(SPARKLE_FADE_TIME, sparkle->life * 0.075f);
        float envelope = fminf(age / fade, 1.0f);
        if (sparkle->leaves)
            ; // Stays bright until it's off the screen
        else if (sparkle->zoom)
            envelope = fminf(envelope, 8.0f * (1.0f - age / sparkle->life));
        else
            envelope = fminf(envelope, (sparkle->life - age) / fade);
        envelope = fmaxf(fminf(envelope, 1.0f), 0.0f);
        envelope = envelope * envelope * (3.0f - 2.0f * envelope);
        // Twinkle between nearly dark and full brightness; squaring gives short, bright flashes.
        // Zooming sparkles twinkle less, so they read as a steady whoosh.
        float twinkle = 0.5f + 0.5f * sinf(sparkle->twinkle_speed * age + sparkle->twinkle_phase);
        twinkle = sparkle->zoom ? 0.6f + 0.4f * twinkle : 0.08f + 0.92f * twinkle * twinkle;
        float alpha = 255.0f * sparkle->brightness * envelope * twinkle;
        if (alpha < 1.0f)
            continue;

        if (sparkle->zoom)
            draw_streak(sparkle, age, x, y, size, alpha, light);
        draw_speck(x, y, size, alpha);
    }
}

// A function to free the sparkle texture
void quit_wave_background()
{
    if (sparkle_texture != NULL)
        SDL_DestroyTexture(sparkle_texture);
    sparkle_texture = NULL;
    if (sparkle_glow_texture != NULL)
        SDL_DestroyTexture(sparkle_glow_texture);
    sparkle_glow_texture = NULL;
}

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
    if (config.wave_sparkles)
        draw_sparkles(seconds, light);

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
