#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include <launcher_config.h>
#include "nowplaying.h"
#include "image.h"
#include "clock.h"
#include "json.h"
#include "util.h"
#include "debug.h"

extern Config config;
extern Geometry geo;
extern SDL_Renderer *renderer;
extern Clock *clk;

// A "now playing" flyout under the clock, in the clock's style: the title and artist of the
// track a media player is playing. Players publish this over D-Bus (MPRIS); a thread asks
// every couple of seconds with busctl, and the main thread renders the text (SDL_ttf isn't
// thread-safe). The flyout slides in from the screen edge when a track starts playing,
// fades between tracks, and slides back out when playback stops.

#define POLL_INTERVAL 2000       // ms between checks
#define MAX_TEXT_CHARS 512
#define MAX_WIDTH 0.4f           // Longer text is truncated, fraction of the screen width
#define SLIDE_TIME 450           // ms to slide in or out
#define FADE_TIME 300            // ms to fade to a new track
#define NOTE "\xE2\x99\xAA  "    // U+266A (a musical note) before the title
#define BUSCTL "busctl --user --json=short "
#define MPRIS_PREFIX "org.mpris.MediaPlayer2."

static SDL_Thread *poll_thread = NULL;
static SDL_mutex *text_mutex = NULL;
static SDL_atomic_t quit_polling;
static SDL_atomic_t polling_paused;
static char polled_text[MAX_TEXT_CHARS] = "";  // Written by the poll thread
static bool text_changed = false;
static char shown_text[MAX_TEXT_CHARS] = "";   // Main thread

static SDL_Texture *texture = NULL;
static SDL_Rect rect = {0, 0, 0, 0};
static SDL_Texture *old_texture = NULL;        // The previous track, fading out
static SDL_Rect old_rect = {0, 0, 0, 0};
static Uint32 fade_start = 0;
static bool visible = false;                   // Sliding in or shown (false: sliding out or hidden)
static Uint32 slide_start = 0;
static float slide_from = 0.0f;                // Slide position when the slide started (0 out, 1 in)

// A function to run a command and read its whole output; returns a new string or NULL
static char *run(const char *command)
{
    FILE *pipe = popen(command, "r");
    if (pipe == NULL)
        return NULL;
    size_t size = 0, capacity = 4096;
    char *out = malloc(capacity);
    size_t n;
    while ((n = fread(out + size, 1, capacity - size - 1, pipe)) > 0) {
        size += n;
        if (capacity - size < 2) {
            char *bigger = realloc(out, capacity * 2);
            if (bigger == NULL)
                break;
            out = bigger;
            capacity *= 2;
        }
    }
    out[size] = '\0';
    pclose(pipe);
    return out;
}

// A function to check that a D-Bus name is safe to put in a shell command
static bool safe_name(const char *name)
{
    return strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == strlen(name);
}

// A function to get a property of a player's org.mpris.MediaPlayer2.Player interface
static char *player_property(const char *name, const char *property)
{
    char command[512];
    snprintf(command, sizeof(command), BUSCTL "get-property %s /org/mpris/MediaPlayer2 "
        "org.mpris.MediaPlayer2.Player %s 2>/dev/null", name, property);
    return run(command);
}

// A function to find what's playing: "Title — Artist" from the first player that's playing
static void poll_players(char *text, size_t size)
{
    text[0] = '\0';
    char *names = run(BUSCTL "call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ListNames 2>/dev/null");
    if (names == NULL)
        return;
    for (int i = 0; text[0] == '\0'; i++) {
        char path[32];
        snprintf(path, sizeof(path), "data.0.%i", i);
        char *name = json_get_string(names, path);
        if (name == NULL)
            break;
        if (!strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) && safe_name(name)) {
            char *status_json = player_property(name, "PlaybackStatus");
            char *status = status_json != NULL ? json_get_string(status_json, "data") : NULL;
            if (status != NULL && !strcmp(status, "Playing")) {
                char *metadata = player_property(name, "Metadata");
                char *title = metadata != NULL ? json_get_string(metadata, "data.xesam:title.data") : NULL;
                char *artist = metadata != NULL ? json_get_string(metadata, "data.xesam:artist.data.0") : NULL;
                if (title != NULL && title[0] != '\0') {
                    if (artist != NULL && artist[0] != '\0')
                        snprintf(text, size, NOTE "%s \xE2\x80\x94 %s", title, artist); // Em dash
                    else
                        snprintf(text, size, NOTE "%s", title);
                }
                free(title);
                free(artist);
                free(metadata);
            }
            free(status);
            free(status_json);
        }
        free(name);
    }
    free(names);
}

// Poll thread: check the players every POLL_INTERVAL ms
static int poll_loop(void *data)
{
    (void) data;
    char text[MAX_TEXT_CHARS];
    while (!SDL_AtomicGet(&quit_polling)) {
        if (!SDL_AtomicGet(&polling_paused)) {
            poll_players(text, sizeof(text));
            SDL_LockMutex(text_mutex);
            if (strcmp(text, polled_text)) {
                strcpy(polled_text, text);
                text_changed = true;
            }
            SDL_UnlockMutex(text_mutex);
        }
        for (int waited = 0; waited < POLL_INTERVAL && !SDL_AtomicGet(&quit_polling); waited += 100)
            SDL_Delay(100);
    }
    return 0;
}

void init_now_playing()
{
    if (!config.now_playing_enabled)
        return;
    if (!config.clock_enabled || clk == NULL) {
        log_error("The now playing flyout needs the clock, which it sits under");
        config.now_playing_enabled = false;
        return;
    }
    text_mutex = SDL_CreateMutex();
    SDL_AtomicSet(&quit_polling, 0);
    SDL_AtomicSet(&polling_paused, 0);
    poll_thread = SDL_CreateThread(poll_loop, "Now Playing Thread", NULL);
}

// A function to pause checking the players while an application is running
void pause_now_playing(bool pause)
{
    if (poll_thread != NULL)
        SDL_AtomicSet(&polling_paused, pause ? 1 : 0);
}

// The slide position now: 0 is out of sight, 1 is in place (eased)
static float slide_position()
{
    float target = visible ? 1.0f : 0.0f;
    float t = (float) (SDL_GetTicks() - slide_start) / SLIDE_TIME;
    if (t >= 1.0f)
        return target;
    t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); // Ease out
    return slide_from + (target - slide_from) * t;
}

static void start_slide(bool in)
{
    slide_from = slide_position();
    visible = in;
    slide_start = SDL_GetTicks();
}

// A function to render the flyout's text in the clock's style
static void render_now_playing(const char *text)
{
    TextInfo info = clk->text_info;
    info.max_width = (int) ((float) geo.screen_width * MAX_WIDTH);
    info.oversize_mode = OVERSIZE_TRUNCATE;
    texture = render_text_texture(text, &info, &rect, NULL);
}

// A function to pick up a new track (or the end of playback) from the poll thread
static void update_text()
{
    char text[MAX_TEXT_CHARS];
    bool changed = false;
    SDL_LockMutex(text_mutex);
    if (text_changed) {
        strcpy(text, polled_text);
        text_changed = false;
        changed = true;
    }
    SDL_UnlockMutex(text_mutex);
    if (!changed || !strcmp(text, shown_text))
        return;
    log_debug("Now playing: %s", text[0] != '\0' ? text : "(nothing)");

    // Playback stopped: slide out (the text stays until it's out of sight)
    if (text[0] == '\0') {
        start_slide(false);
        shown_text[0] = '\0';
        return;
    }

    // A new track while the flyout is showing: fade from the old text to the new
    if (visible && texture != NULL) {
        if (old_texture != NULL)
            SDL_DestroyTexture(old_texture);
        old_texture = texture;
        old_rect = rect;
        fade_start = SDL_GetTicks();
    }
    else {
        if (texture != NULL)
            SDL_DestroyTexture(texture);
        start_slide(true);
    }
    texture = NULL;
    render_now_playing(text);
    copy_string(shown_text, text, sizeof(shown_text));
}

static void draw_text(SDL_Texture *t, const SDL_Rect *r, float slide, Uint8 alpha)
{
    if (t == NULL || alpha == 0)
        return;

    // In place under the date, aligned like the clock; it slides in from the screen edge
    int line_y = config.clock_show_date ? clk->date_rect.y + clk->y_advance : clk->time_rect.y + clk->y_advance;
    int in_x, out_x;
    if (config.clock_alignment == ALIGNMENT_LEFT) {
        in_x = config.clock_margin;
        out_x = -r->w;
    }
    else {
        in_x = geo.screen_width - config.clock_margin - r->w;
        out_x = geo.screen_width;
    }
    SDL_Rect dst = {(int) ((float) out_x + (float) (in_x - out_x) * slide), line_y, r->w, r->h};
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderCopy(renderer, t, NULL, &dst);
}

// A function to draw the flyout for the current frame
void draw_now_playing()
{
    if (!config.now_playing_enabled || text_mutex == NULL)
        return;
    update_text();
    float slide = slide_position();
    if (slide <= 0.0f)
        return;

    // Fade out with the slide, and between tracks
    Uint8 alpha = (Uint8) (255.0f * slide);
    if (old_texture != NULL) {
        Uint32 elapsed = SDL_GetTicks() - fade_start;
        if (elapsed >= FADE_TIME) {
            SDL_DestroyTexture(old_texture);
            old_texture = NULL;
        }
        else {
            float t = (float) elapsed / FADE_TIME;
            draw_text(old_texture, &old_rect, slide, (Uint8) ((float) alpha * (1.0f - t)));
            draw_text(texture, &rect, slide, (Uint8) ((float) alpha * t));
            return;
        }
    }
    draw_text(texture, &rect, slide, alpha);
}

void quit_now_playing()
{
    if (poll_thread != NULL) {
        SDL_AtomicSet(&quit_polling, 1);
        SDL_WaitThread(poll_thread, NULL);
        poll_thread = NULL;
    }
    if (texture != NULL)
        SDL_DestroyTexture(texture);
    if (old_texture != NULL)
        SDL_DestroyTexture(old_texture);
    texture = old_texture = NULL;
    if (text_mutex != NULL)
        SDL_DestroyMutex(text_mutex);
    text_mutex = NULL;
}
