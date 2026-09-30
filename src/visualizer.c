#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#ifdef __unix__
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include "launcher.h"
#include <launcher_config.h>
#include "visualizer.h"
#include "debug.h"

extern Config config;
extern SDL_Renderer *renderer;
extern Geometry geo;

// An audio visualizer drawn over the background: a scrolling spectrogram (time moves left,
// pitch goes up) or spectrum bars, of whatever is playing on the default audio output.
// The audio comes from the output's monitor, recorded by parec (PulseAudio or PipeWire)
// in a separate process; a thread reads it into a ring buffer, and each frame the newest
// samples go through an FFT and are grouped into log-spaced frequency bands.

#define SAMPLE_RATE 48000
#define FFT_SIZE 2048          // 23 Hz resolution at 48 kHz
#define FFT_BITS 11
#define RING_SIZE 8192
#define NUM_BANDS 128
#define MIN_FREQUENCY 30.0f
#define MAX_FREQUENCY 16000.0f
#define MIN_DB -65.0f          // Quieter than this is transparent
#define MAX_DB -12.0f          // Louder than this is full intensity
#define RELEASE_TIME 0.15f     // Seconds for a band to fall back after a peak
#define HISTORY 480            // Spectrogram columns (8 seconds at 60 columns a second)
#define COLUMN_RATE 60.0f      // Spectrogram columns a second
#define NUM_BARS 64
#define LINE_SMOOTHING 2       // Bands averaged on each side, for a smooth curve
#define LINE_WIDTH 0.005f      // Half thickness of the line's glow, fraction of screen height
#define BAR_GAP 0.25f          // Gap between bars, relative to the bar width
#define CAPTURE_COMMAND "parec"

static const char *capture_args[] = {
    CAPTURE_COMMAND, "-d", "@DEFAULT_MONITOR@", "--raw", "--format=float32le",
    "--channels=1", "--rate=48000", "--latency-msec=20", NULL
};

#ifdef __unix__
static pid_t capture_pid = -1;
#endif
static int capture_fd = -1;
static SDL_Thread *reader_thread = NULL;
static SDL_mutex *ring_mutex = NULL;
static float ring[RING_SIZE];
static unsigned int ring_head = 0; // Total samples written (wraps)
static bool running = false;

static float bands[NUM_BANDS];      // Smoothed intensities, 0-1
static int band_start[NUM_BANDS];   // FFT bins of each band
static int band_end[NUM_BANDS];
static float band_center[NUM_BANDS]; // Center frequency of each band, in FFT bins
static float window[FFT_SIZE];
static bool tables_ready = false;
static Uint32 last_update = 0;
static double column_debt = 0.0;    // Spectrogram columns due

static SDL_Texture *spectrogram = NULL;
static int spectrogram_head = 0;    // Column written last
static Uint32 column_pixels[NUM_BANDS];

// Reader thread: moves samples from the capture process into the ring buffer
static int read_samples(void *data)
{
    (void) data;
#ifdef __unix__
    float buffer[512];
    size_t pending = 0; // Bytes of a partial sample carried over
    unsigned char *bytes = (unsigned char*) buffer;
    for (;;) {
        ssize_t n = read(capture_fd, bytes + pending, sizeof(buffer) - pending);
        if (n <= 0)
            break;
        pending += (size_t) n;
        size_t count = pending / sizeof(float);
        SDL_LockMutex(ring_mutex);
        for (size_t i = 0; i < count; i++)
            ring[(ring_head + i) % RING_SIZE] = buffer[i];
        ring_head += (unsigned int) count;
        SDL_UnlockMutex(ring_mutex);
        size_t leftover = pending - count * sizeof(float);
        memmove(bytes, bytes + count * sizeof(float), leftover);
        pending = leftover;
    }
#endif
    return 0;
}

// A function to start recording the default output's monitor
static void start_capture()
{
#ifdef __unix__
    if (running)
        return;
    int fds[2];
    if (pipe(fds) < 0) {
        log_error("Could not create a pipe for the audio visualizer");
        return;
    }
    capture_pid = fork();
    if (capture_pid < 0) {
        close(fds[0]);
        close(fds[1]);
        log_error("Could not start the audio visualizer's capture process");
        return;
    }
    if (capture_pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(CAPTURE_COMMAND, (char* const*) capture_args);
        _exit(127);
    }
    close(fds[1]);
    capture_fd = fds[0];
    if (ring_mutex == NULL)
        ring_mutex = SDL_CreateMutex();
    reader_thread = SDL_CreateThread(read_samples, "Visualizer Thread", NULL);
    running = true;
    log_debug("Audio visualizer started (%s)", CAPTURE_COMMAND);
#else
    log_error("The audio visualizer is only available on Linux");
#endif
}

// A function to stop recording; the reader thread ends when the pipe closes
static void stop_capture()
{
#ifdef __unix__
    if (!running)
        return;
    kill(capture_pid, SIGTERM);
    waitpid(capture_pid, NULL, 0);
    capture_pid = -1;
    SDL_WaitThread(reader_thread, NULL);
    reader_thread = NULL;
    close(capture_fd);
    capture_fd = -1;
    running = false;
    memset(bands, 0, sizeof(bands));
#endif
}

// A function to work out the FFT window and which FFT bins make up each band
static void init_tables()
{
    for (int i = 0; i < FFT_SIZE; i++)
        window[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * (float) i / (float) (FFT_SIZE - 1));
    float bin_width = (float) SAMPLE_RATE / (float) FFT_SIZE;
    for (int b = 0; b < NUM_BANDS; b++) {
        float low = MIN_FREQUENCY * powf(MAX_FREQUENCY / MIN_FREQUENCY, (float) b / NUM_BANDS);
        float high = MIN_FREQUENCY * powf(MAX_FREQUENCY / MIN_FREQUENCY, (float) (b + 1) / NUM_BANDS);
        band_center[b] = sqrtf(low * high) / bin_width;
        band_start[b] = (int) (low / bin_width + 0.5f);
        band_end[b] = (int) (high / bin_width + 0.5f);
        if (band_end[b] <= band_start[b])
            band_end[b] = band_start[b] + 1; // Low bands narrower than a bin share it
        if (band_end[b] > FFT_SIZE / 2)
            band_end[b] = FFT_SIZE / 2;
    }
    tables_ready = true;
}

// An in-place radix-2 FFT
static void fft(float *re, float *im)
{
    for (unsigned int i = 1, j = 0; i < FFT_SIZE; i++) {
        unsigned int bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int length = 2; length <= FFT_SIZE; length <<= 1) {
        float angle = -2.0f * 3.14159265f / (float) length;
        float wr = cosf(angle), wi = sinf(angle);
        for (int i = 0; i < FFT_SIZE; i += length) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < length / 2; k++) {
                int a = i + k, b = a + length / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next;
            }
        }
    }
}

// A function to analyze the newest samples into the smoothed bands
static void update_bands(float elapsed)
{
    static float re[FFT_SIZE], im[FFT_SIZE];
    SDL_LockMutex(ring_mutex);
    unsigned int start = ring_head - FFT_SIZE;
    for (int i = 0; i < FFT_SIZE; i++)
        re[i] = ring[(start + (unsigned int) i) % RING_SIZE] * window[i];
    SDL_UnlockMutex(ring_mutex);
    memset(im, 0, sizeof(im));
    fft(re, im);

    // Band levels in decibels, relative to a full-scale sine through the window (gain 0.5)
    float release = expf(-elapsed / RELEASE_TIME);
    for (int b = 0; b < NUM_BANDS; b++) {
        float peak = 0.0f;
        if (band_end[b] - band_start[b] <= 1) {
            // A band narrower than a bin (the bass): interpolate between the nearest bins,
            // so neighboring bands don't share one value and form steps
            int k = (int) band_center[b];
            float t = band_center[b] - (float) k;
            float m0 = sqrtf(re[k] * re[k] + im[k] * im[k]);
            float m1 = sqrtf(re[k + 1] * re[k + 1] + im[k + 1] * im[k + 1]);
            float magnitude = m0 + (m1 - m0) * t;
            peak = magnitude * magnitude;
        }
        for (int k = band_start[b]; k < band_end[b]; k++) {
            float magnitude = re[k] * re[k] + im[k] * im[k];
            if (magnitude > peak && band_end[b] - band_start[b] > 1)
                peak = magnitude;
        }
        float amplitude = sqrtf(peak) / (FFT_SIZE * 0.25f);
        float db = 20.0f * log10f(amplitude + 1e-9f);
        float level = (db - MIN_DB) / (MAX_DB - MIN_DB);
        level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
        bands[b] = level > bands[b] ? level : bands[b] * release + level * (1.0f - release);
    }
}

// A function to get a spectrogram color for an intensity (0-1): quiet is transparent,
// rising through blue and violet to white
static Uint32 intensity_color(float v)
{
    static const float stops[][4] = {
        {0.00f, 0x30, 0x50, 0xFF},
        {0.45f, 0x70, 0x60, 0xFF},
        {0.75f, 0xE0, 0x70, 0xE0},
        {1.00f, 0xFF, 0xFF, 0xFF}
    };
    int i = 0;
    while (i < 2 && v > stops[i + 1][0])
        i++;
    float t = (v - stops[i][0]) / (stops[i + 1][0] - stops[i][0]);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    Uint8 r = (Uint8) (stops[i][1] + (stops[i + 1][1] - stops[i][1]) * t);
    Uint8 g = (Uint8) (stops[i][2] + (stops[i + 1][2] - stops[i][2]) * t);
    Uint8 b = (Uint8) (stops[i][3] + (stops[i + 1][3] - stops[i][3]) * t);
    Uint8 a = (Uint8) (255.0f * powf(v, 1.6f)); // Quiet levels stay faint, so the background shows through
    return ((Uint32) a << 24) | ((Uint32) r << 16) | ((Uint32) g << 8) | b;
}

// A function to add the current bands as the newest spectrogram column
static void add_column()
{
    if (spectrogram == NULL) {
        spectrogram = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          HISTORY, NUM_BANDS);
        if (spectrogram == NULL)
            return;
        SDL_SetTextureBlendMode(spectrogram, SDL_BLENDMODE_BLEND);
        Uint32 *clear = calloc(HISTORY * NUM_BANDS, sizeof(Uint32));
        SDL_UpdateTexture(spectrogram, NULL, clear, HISTORY * (int) sizeof(Uint32));
        free(clear);
    }
    spectrogram_head = (spectrogram_head + 1) % HISTORY;
    for (int b = 0; b < NUM_BANDS; b++)
        column_pixels[NUM_BANDS - 1 - b] = intensity_color(bands[b]); // Low frequencies at the bottom
    SDL_Rect column = {spectrogram_head, 0, 1, NUM_BANDS};
    SDL_UpdateTexture(spectrogram, &column, column_pixels, (int) sizeof(Uint32));
}

// A function to draw the spectrogram, oldest column at the left
static void draw_spectrogram(const SDL_Rect *area, Uint8 alpha)
{
    if (spectrogram == NULL)
        return;
    SDL_SetTextureAlphaMod(spectrogram, alpha);

    // The texture is a ring of columns: draw the part after the head, then up to the head
    int oldest = (spectrogram_head + 1) % HISTORY;
    int first = HISTORY - oldest;
    float scale = (float) area->w / (float) HISTORY;
    SDL_Rect src1 = {oldest, 0, first, NUM_BANDS};
    SDL_FRect dst1 = {(float) area->x, (float) area->y, first * scale, (float) area->h};
    SDL_RenderCopyF(renderer, spectrogram, &src1, &dst1);
    if (oldest > 0) {
        SDL_Rect src2 = {0, 0, oldest, NUM_BANDS};
        SDL_FRect dst2 = {(float) area->x + first * scale, (float) area->y, oldest * scale, (float) area->h};
        SDL_RenderCopyF(renderer, spectrogram, &src2, &dst2);
    }
}

// A function to draw spectrum bars rising from the bottom of the area
static void draw_bars(const SDL_Rect *area, Uint8 alpha)
{
    SDL_BlendMode mode;
    SDL_GetRenderDrawBlendMode(renderer, &mode);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    float slot = (float) area->w / NUM_BARS;
    float width = slot / (1.0f + BAR_GAP);
    for (int i = 0; i < NUM_BARS; i++) {
        // Each bar is the loudest of its share of the bands
        float level = 0.0f;
        for (int b = i * NUM_BANDS / NUM_BARS; b < (i + 1) * NUM_BANDS / NUM_BARS; b++)
            level = fmaxf(level, bands[b]);
        if (level <= 0.01f)
            continue;
        Uint32 color = intensity_color(level);
        SDL_SetRenderDrawColor(renderer, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF,
            (Uint8) ((float) alpha * fmaxf(level, 0.35f)));
        float h = level * (float) area->h;
        SDL_FRect bar = {(float) area->x + (float) i * slot + (slot - width) / 2.0f,
                         (float) (area->y + area->h) - h, width, h};
        SDL_RenderFillRectF(renderer, &bar);
    }
    SDL_SetRenderDrawBlendMode(renderer, mode);
}

// A function to draw the spectrum as a single glowing white line across the area, like a line graph:
// low pitches on the left, rising with loudness from the bottom of the area
static void draw_line(const SDL_Rect *area, Uint8 alpha)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    static SDL_Vertex vertices[NUM_BANDS * 3];
    static int indices[(NUM_BANDS - 1) * 12];
    float loudest = 0.0f;
    for (int b = 0; b < NUM_BANDS; b++)
        loudest = fmaxf(loudest, bands[b]);
    if (loudest < 0.01f) // Hidden in silence
        return;

    float half = fmaxf((float) geo.screen_height * LINE_WIDTH, 1.5f);
    SDL_Color edge = {0xFF, 0xFF, 0xFF, 0};
    static float xs[NUM_BANDS], ys[NUM_BANDS], levels[NUM_BANDS];
    for (int b = 0; b < NUM_BANDS; b++) {
        // Average the neighboring bands for a smooth curve
        float sum = 0.0f;
        int count = 0;
        for (int k = b - LINE_SMOOTHING; k <= b + LINE_SMOOTHING; k++) {
            if (k >= 0 && k < NUM_BANDS) {
                sum += bands[k];
                count++;
            }
        }
        levels[b] = sum / (float) count;
        xs[b] = (float) area->x + (float) area->w * (float) b / (float) (NUM_BANDS - 1);
        ys[b] = (float) (area->y + area->h) - half - levels[b] * (float) (area->h - 2.0f * half);
    }
    for (int b = 0; b < NUM_BANDS; b++) {
        // Thicken the line across its direction, so steep parts are as thick as flat ones
        int before = b > 0 ? b - 1 : b, after = b < NUM_BANDS - 1 ? b + 1 : b;
        float dx = xs[after] - xs[before], dy = ys[after] - ys[before];
        float length = sqrtf(dx * dx + dy * dy);
        float nx = -dy / length * half, ny = dx / length * half;

        // A white line; louder parts are brighter
        SDL_Color center = {0xFF, 0xFF, 0xFF, (Uint8) fminf((float) alpha * (1.2f + 0.8f * levels[b]), 255.0f)};
        vertices[b * 3] = (SDL_Vertex) {{xs[b] - nx, ys[b] - ny}, edge, {0.0f, 0.0f}};
        vertices[b * 3 + 1] = (SDL_Vertex) {{xs[b], ys[b]}, center, {0.0f, 0.0f}};
        vertices[b * 3 + 2] = (SDL_Vertex) {{xs[b] + nx, ys[b] + ny}, edge, {0.0f, 0.0f}};
    }
    int n = 0;
    for (int b = 0; b < NUM_BANDS - 1; b++) {
        for (int row = 0; row < 2; row++) {
            int a = b * 3 + row, c = a + 3;
            indices[n++] = a; indices[n++] = c; indices[n++] = a + 1;
            indices[n++] = a + 1; indices[n++] = c; indices[n++] = c + 1;
        }
    }
    SDL_BlendMode mode;
    SDL_GetRenderDrawBlendMode(renderer, &mode);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, NULL, vertices, NUM_BANDS * 3, indices, n);
    SDL_SetRenderDrawBlendMode(renderer, mode);
#else
    (void) area; (void) alpha;
#endif
}

// A function to start or stop the visualizer to match its setting
void update_visualizer_state()
{
    if (config.visualizer_enabled)
        start_capture();
    else
        stop_capture();
}

// A function to pause the visualizer while an application is running
void pause_visualizer(bool pause)
{
    if (pause)
        stop_capture();
    else
        update_visualizer_state();
}

// A function to analyze the audio and draw the visualizer for the current frame
void draw_visualizer()
{
    if (!config.visualizer_enabled || !running)
        return;
    if (!tables_ready)
        init_tables();
    Uint32 now = SDL_GetTicks();
    float elapsed = last_update == 0 ? 0.0f : (float) (now - last_update) / 1000.0f;
    last_update = now;
    update_bands(elapsed);

    int height = (int) ((float) geo.screen_height * config.visualizer_height + 0.5f);
    SDL_Rect area = {0, geo.screen_height - height, geo.screen_width, height};
    Uint8 alpha = config.visualizer_alpha;
    if (config.visualizer_style == VISUALIZER_BARS)
        draw_bars(&area, alpha);
    else if (config.visualizer_style == VISUALIZER_LINE)
        draw_line(&area, alpha);
    else {
        // Add columns at a steady rate, whatever the frame rate
        column_debt += elapsed * COLUMN_RATE;
        if (column_debt > HISTORY)
            column_debt = HISTORY;
        while (column_debt >= 1.0) {
            add_column();
            column_debt -= 1.0;
        }
        draw_spectrogram(&area, alpha);
    }
}

// A function to turn the visualizer on or off
void toggle_visualizer()
{
    config.visualizer_enabled = !config.visualizer_enabled;
    last_update = 0;
    update_visualizer_state();
    log_debug("Visualizer %s", config.visualizer_enabled ? "enabled" : "disabled");
}

// A function to stop the capture and free the visualizer's resources
void quit_visualizer()
{
    stop_capture();
    if (spectrogram != NULL)
        SDL_DestroyTexture(spectrogram);
    spectrogram = NULL;
    if (ring_mutex != NULL)
        SDL_DestroyMutex(ring_mutex);
    ring_mutex = NULL;
}
