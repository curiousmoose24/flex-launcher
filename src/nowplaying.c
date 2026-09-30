#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif
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
// fades between tracks, and slides back out when playback stops. The track's cover art (the
// player's mpris:artUrl, a file or a web address) is shown beside the text, on the side
// above the text, left-justified with it; the thread loads it and scales it.

#define POLL_INTERVAL 2000       // ms between checks
#define MAX_TEXT_CHARS 512
#define MAX_WIDTH 0.4f           // Longer text is truncated, fraction of the screen width
#define SLIDE_TIME 450           // ms to slide in or out
#define FADE_TIME 300            // ms to fade to a new track
#define NOTE "\xE2\x99\xAA  "    // U+266A (a musical note) before the title
#define BUSCTL "busctl --user --json=short "
#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MAX_ART_URL_CHARS 2048
#define MAX_ART_BYTES (20 * 1024 * 1024)
#define ART_TIMEOUT_SECONDS 10L
#define ART_SCALE 3.0f          // Cover art size, times the text's line height
#define ART_GAP 0.1f            // Space between the art and the text, times the line height

static SDL_Thread *poll_thread = NULL;
static SDL_mutex *text_mutex = NULL;
static SDL_atomic_t quit_polling;
static SDL_atomic_t polling_paused;
static char polled_text[MAX_TEXT_CHARS] = "";  // Written by the poll thread
static bool text_changed = false;
static SDL_Surface *polled_art = NULL;         // Written by the poll thread (NULL: no art)
static bool art_changed = false;
static int art_size = 0;                       // Pixels, set before the thread starts
static char shown_text[MAX_TEXT_CHARS] = "";   // Main thread

// A track as drawn: its text and cover art
typedef struct {
    SDL_Texture *text;
    SDL_Rect text_rect;
    SDL_Texture *art;
} Item;

static Item current = {NULL, {0, 0, 0, 0}, NULL};
static Item old = {NULL, {0, 0, 0, 0}, NULL}; // The previous track, fading out
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

// A function to decode a file:// URL's %XX escapes into a path
static bool file_url_path(const char *url, char *path, size_t size)
{
    const char *p = url + strlen("file://");
    if (!strncmp(p, "localhost/", 10))
        p += strlen("localhost");
    if (*p != '/')
        return false;
    size_t n = 0;
    for (; *p != '\0' && n + 1 < size; p++) {
        unsigned int c;
        if (*p == '%' && sscanf(p + 1, "%2x", &c) == 1) {
            path[n++] = (char) c;
            p += 2;
        }
        else
            path[n++] = *p;
    }
    path[n] = '\0';
    return *p == '\0';
}

// A function to get the process ID of a D-Bus name's owner, or 0
static long owner_pid(const char *name)
{
    char command[512];
    snprintf(command, sizeof(command), BUSCTL "call org.freedesktop.DBus /org/freedesktop/DBus "
        "org.freedesktop.DBus GetConnectionUnixProcessID s %s 2>/dev/null", name);
    char *out = run(command);
    long pid = 0;
    if (out != NULL) {
        char *data = strstr(out, "\"data\":[");
        if (data != NULL)
            pid = strtol(data + strlen("\"data\":["), NULL, 10);
        free(out);
    }
    return pid;
}

#ifdef HAVE_CURL
typedef struct {
    char *data;
    size_t size;
} Buffer;

static size_t write_callback(char *ptr, size_t size, size_t count, void *userdata)
{
    Buffer *buffer = (Buffer*) userdata;
    size_t length = size * count;
    if (buffer->size + length > MAX_ART_BYTES)
        return 0;
    char *data = realloc(buffer->data, buffer->size + length);
    if (data == NULL)
        return 0;
    memcpy(data + buffer->size, ptr, length);
    buffer->data = data;
    buffer->size += length;
    return length;
}

// Stops a download when the launcher quits
static int progress_callback(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void) userdata; (void) dltotal; (void) dlnow; (void) ultotal; (void) ulnow;
    return SDL_AtomicGet(&quit_polling) ? 1 : 0;
}

static SDL_Surface *download_art(const char *url)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL)
        return NULL;
    Buffer buffer = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ART_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, EXECUTABLE_TITLE);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    SDL_Surface *surface = NULL;
    if (result == CURLE_OK && buffer.size > 0)
        surface = IMG_Load_RW(SDL_RWFromConstMem(buffer.data, (int) buffer.size), 1);
    else if (result != CURLE_OK)
        log_debug("Could not download the album art %s: %s", url, curl_easy_strerror(result));
    free(buffer.data);
    return surface;
}
#endif

// A function to crop a cover to a square around its center, and scale it to the art size
static SDL_Surface *fit_art(SDL_Surface *surface)
{
    SDL_Rect crop = {0, 0, surface->w, surface->h};
    if (surface->w > surface->h) {
        crop.w = surface->h;
        crop.x = (surface->w - crop.w) / 2;
    }
    else {
        crop.h = surface->w;
        crop.y = (surface->h - crop.h) / 2;
    }
    SDL_Surface *source = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_Surface *fitted = SDL_CreateRGBSurfaceWithFormat(0, art_size, art_size, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_FreeSurface(surface);
    if (source == NULL || fitted == NULL || crop.w < 1 || crop.h < 1) {
        SDL_FreeSurface(source);
        SDL_FreeSurface(fitted);
        return NULL;
    }
#if SDL_VERSION_ATLEAST(2, 0, 16)
    int error = SDL_SoftStretchLinear(source, &crop, fitted, NULL);
#else
    int error = SDL_BlitScaled(source, &crop, fitted, NULL);
#endif
    SDL_FreeSurface(source);
    if (error < 0) {
        SDL_FreeSurface(fitted);
        return NULL;
    }
    return fitted;
}

// A function to load a player's cover art, scaled to the art size; NULL if there's none
static SDL_Surface *load_art(const char *url, const char *name)
{
    SDL_Surface *surface = NULL;
    if (!strncmp(url, "file://", 7)) {
        char path[MAX_PATH_CHARS + 1];
        if (!file_url_path(url, path, sizeof(path)))
            return NULL;
        surface = IMG_Load(path);

        // A sandboxed player (e.g. a Flatpak) may save the art in its own /tmp; it can be
        // reached through the player's process
        if (surface == NULL) {
            long pid = owner_pid(name);
            if (pid > 0) {
                char sandbox_path[MAX_PATH_CHARS + 32];
                snprintf(sandbox_path, sizeof(sandbox_path), "/proc/%li/root%s", pid, path);
                surface = IMG_Load(sandbox_path);
            }
        }
    }
#ifdef HAVE_CURL
    else if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
        surface = download_art(url);
#endif
    if (surface == NULL) {
        log_debug("Could not load the album art %s", url);
        return NULL;
    }
    return fit_art(surface);
}

// A function to find what's playing: "Title — Artist" from the first player that's playing,
// and the address of its cover art
static void poll_players(char *text, size_t size, char *art_url, size_t art_url_size, char *player, size_t player_size)
{
    text[0] = '\0';
    art_url[0] = '\0';
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
                char *art = metadata != NULL ? json_get_string(metadata, "data.mpris:artUrl.data") : NULL;
                if (title != NULL && title[0] != '\0') {
                    if (artist != NULL && artist[0] != '\0')
                        snprintf(text, size, NOTE "%s \xE2\x80\x94 %s", title, artist); // Em dash
                    else
                        snprintf(text, size, NOTE "%s", title);
                    if (art != NULL)
                        copy_string(art_url, art, art_url_size);
                    copy_string(player, name, player_size);
                }
                free(title);
                free(artist);
                free(art);
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
    char art_url[MAX_ART_URL_CHARS];
    char player[256] = "";
    char loaded_art_url[MAX_ART_URL_CHARS] = "";
    while (!SDL_AtomicGet(&quit_polling)) {
        if (!SDL_AtomicGet(&polling_paused)) {
            poll_players(text, sizeof(text), art_url, sizeof(art_url), player, sizeof(player));

            // Load the art when the track's art changes (it stays while the flyout slides out)
            bool new_art = config.now_playing_album_art && text[0] != '\0' && strcmp(art_url, loaded_art_url);
            SDL_Surface *art = NULL;
            if (new_art) {
                copy_string(loaded_art_url, art_url, sizeof(loaded_art_url));
                if (art_url[0] != '\0')
                    art = load_art(art_url, player);
            }

            SDL_LockMutex(text_mutex);
            if (strcmp(text, polled_text)) {
                strcpy(polled_text, text);
                text_changed = true;
            }
            if (new_art) {
                SDL_FreeSurface(polled_art);
                polled_art = art;
                art_changed = true;
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
    if (config.now_playing_album_art) {
        art_size = (int) ((float) TTF_FontHeight(clk->text_info.font) * ART_SCALE);
        if (art_size < 1)
            config.now_playing_album_art = false;
#ifdef HAVE_CURL
        else
            curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
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
    current.text = render_text_texture(text, &info, &current.text_rect, NULL);
}

// Tracks from the same album share their art, so it's freed when no track has it
static void destroy_art(Item *item)
{
    Item *other = item == &current ? &old : &current;
    if (item->art != NULL && item->art != other->art)
        SDL_DestroyTexture(item->art);
    item->art = NULL;
}

static void destroy_item(Item *item)
{
    if (item->text != NULL)
        SDL_DestroyTexture(item->text);
    item->text = NULL;
    destroy_art(item);
}

// A function to pick up a new track (or the end of playback) from the poll thread
static void update_text()
{
    char text[MAX_TEXT_CHARS];
    bool changed = false;
    SDL_Surface *art = NULL;
    bool new_art = false;
    SDL_LockMutex(text_mutex);
    if (text_changed) {
        strcpy(text, polled_text);
        text_changed = false;
        changed = true;
    }
    if (art_changed) {
        art = polled_art;
        polled_art = NULL;
        art_changed = false;
        new_art = true;
    }
    SDL_UnlockMutex(text_mutex);

    if (changed && strcmp(text, shown_text)) {
        log_debug("Now playing: %s", text[0] != '\0' ? text : "(nothing)");

        // Playback stopped: slide out (the text stays until it's out of sight)
        if (text[0] == '\0') {
            start_slide(false);
            shown_text[0] = '\0';
        }
        else {
            // A new track while the flyout is showing: fade from the old track to the new
            if (visible && current.text != NULL) {
                destroy_item(&old);
                old = current; // The art stays with the new track too, unless new art came
                fade_start = SDL_GetTicks();
            }
            else {
                if (current.text != NULL)
                    SDL_DestroyTexture(current.text);
                start_slide(true);
            }
            current.text = NULL;
            render_now_playing(text);
            copy_string(shown_text, text, sizeof(shown_text));
        }
    }

    // New art (or none) for the track
    if (new_art) {
        destroy_art(&current);
        current.art = art != NULL ? load_texture(art) : NULL;
    }
}

static void draw_item(const Item *item, float slide, Uint8 alpha)
{
    if (item->text == NULL || alpha == 0)
        return;

    // In place under the date, aligned like the clock, with the art above the text and
    // left-justified with it (beside the clock when there's room); it slides in from the screen edge
    const SDL_Rect *r = &item->text_rect;
    int art_w = item->art != NULL ? art_size : 0;
    int art_h = item->art != NULL ? art_size + (int) ((float) art_size / ART_SCALE * ART_GAP) : 0;
    int width = r->w > art_w ? r->w : art_w;
    int line_y = config.clock_show_date ? clk->date_rect.y + clk->y_advance : clk->time_rect.y + clk->y_advance;
    int in_x, out_x;
    if (config.clock_alignment == ALIGNMENT_LEFT) {
        in_x = config.clock_margin;
        out_x = -width;
    }
    else {
        in_x = geo.screen_width - config.clock_margin - width;
        out_x = geo.screen_width;
    }
    int x = (int) ((float) out_x + (float) (in_x - out_x) * slide);
    int text_y = line_y;
    if (item->art != NULL) {
        // The art goes as high as the top of the time when it clears the clock's lines, and
        // otherwise under the date; its top lines up with the top of the letters
        int art_y = line_y + clk->y_offset;
        int gap = art_h - art_size;
        int clock_left = clk->time_rect.x;
        if (config.clock_show_date && clk->date_rect.x < clock_left)
            clock_left = clk->date_rect.x;
        if (config.clock_alignment != ALIGNMENT_LEFT && in_x + art_size + gap <= clock_left)
            art_y = clk->time_rect.y + clk->y_offset;
        SDL_Rect art_dst = {x, art_y, art_size, art_size};
        SDL_SetTextureAlphaMod(item->art, alpha);
        SDL_RenderCopy(renderer, item->art, NULL, &art_dst);

        // The text goes under the art, but never above its own line under the date
        text_y = art_y + art_h - clk->y_offset;
        if (text_y < line_y)
            text_y = line_y;
    }
    SDL_Rect dst = {x, text_y, r->w, r->h};
    SDL_SetTextureAlphaMod(item->text, alpha);
    SDL_RenderCopy(renderer, item->text, NULL, &dst);
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
    if (old.text != NULL) {
        Uint32 elapsed = SDL_GetTicks() - fade_start;
        if (elapsed >= FADE_TIME)
            destroy_item(&old);
        else {
            float t = (float) elapsed / FADE_TIME;
            draw_item(&old, slide, (Uint8) ((float) alpha * (1.0f - t)));
            draw_item(&current, slide, (Uint8) ((float) alpha * t));
            return;
        }
    }
    draw_item(&current, slide, alpha);
}

void quit_now_playing()
{
    if (poll_thread != NULL) {
        SDL_AtomicSet(&quit_polling, 1);
        SDL_WaitThread(poll_thread, NULL);
        poll_thread = NULL;
    }
    destroy_item(&current);
    destroy_item(&old);
    SDL_FreeSurface(polled_art);
    polled_art = NULL;
#ifdef HAVE_CURL
    if (config.now_playing_album_art && art_size > 0)
        curl_global_cleanup();
#endif
    if (text_mutex != NULL)
        SDL_DestroyMutex(text_mutex);
    text_mutex = NULL;
}
