#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#ifdef __unix__
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif
#include "launcher.h"
#include <launcher_config.h>
#include "webimage.h"
#include "image.h"
#include "util.h"
#include "debug.h"
#include "json.h"

// Background images from a URL, like Homepage's background setting. With ImageJson, the URL
// returns JSON (e.g. a Wallhaven search) and ImageJson is the path to the image URL in it.
// {width} and {height} are replaced with the screen size, and {keywords} with one of the
// comma-separated ImageKeywords.
// The last image is cached so it shows immediately at startup; a fresh image is then
// downloaded in a separate thread and faded in, and again every ImageRefresh minutes.

extern Config config;
extern Ticks ticks;
extern Geometry geo;
extern SDL_Renderer *renderer;
extern SDL_Texture *background_texture;

#define MAX_IMAGE_BYTES (50 * 1024 * 1024)
#define DOWNLOAD_TIMEOUT_SECONDS 60L
#define RETRY_PERIOD (5 * 60 * 1000)

enum { DOWNLOAD_IDLE, DOWNLOAD_RUNNING, DOWNLOAD_DONE };

static char *url = NULL; // The Image setting (URL template); NULL until initialized
static char *request_url = NULL; // The URL of the current download, with placeholders filled in
static bool download_again = false; // A new download was requested while one was running
static char cache_path[MAX_PATH_CHARS + 1] = "";
static SDL_Thread *download_thread = NULL;
static SDL_atomic_t download_state;
static SDL_atomic_t abort_download;
static SDL_Surface *downloaded_surface = NULL; // Set by the download thread
static Uint32 next_download = 0;
static SDL_Texture *fade_texture = NULL;
static Uint32 fade_start = 0;

static void temp_cache_path(char *path, size_t size);
static bool save_cache_file(const char *data, size_t size);

bool is_web_image(const char *path)
{
    return path != NULL && (!strncmp(path, "http://", 7) || !strncmp(path, "https://", 8));
}

// A function to append one of the comma-separated ImageKeywords, picked at random and URL-encoded
static void append_keyword(char *buffer, size_t size)
{
    if (config.image_keywords == NULL)
        return;
    int count = 1;
    for (const char *c = config.image_keywords; *c != '\0'; c++)
        if (*c == ',')
            count++;
    int pick = rand() % count;
    const char *start = config.image_keywords;
    for (int i = 0; i < pick; i++)
        start = strchr(start, ',') + 1;
    const char *end = strchr(start, ',');
    if (end == NULL)
        end = start + strlen(start);
    while (start < end && (*start == ' ' || *start == '\t'))
        start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    for (const char *c = start; c < end && strlen(buffer) + 4 < size; c++) {
        size_t length = strlen(buffer);
        unsigned char ch = (unsigned char) *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch))
            buffer[length] = (char) ch, buffer[length + 1] = '\0';
        else
            snprintf(buffer + length, size - length, "%%%02X", ch);
    }
}

// A function to replace {width}, {height} and {keywords} in the URL
static char *expand_url(const char *template)
{
    char buffer[2048] = "";
    const char *p = template;
    while (*p != '\0' && strlen(buffer) < sizeof(buffer) - 16) {
        if (!strncmp(p, "{width}", 7)) {
            snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "%i", geo.screen_width);
            p += 7;
        }
        else if (!strncmp(p, "{height}", 8)) {
            snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "%i", geo.screen_height);
            p += 8;
        }
        else if (!strncmp(p, "{keywords}", 10)) {
            append_keyword(buffer, sizeof(buffer));
            p += 10;
        }
        else {
            size_t length = strlen(buffer);
            buffer[length] = *p++;
            buffer[length + 1] = '\0';
        }
    }
    return strdup(buffer);
}

// A function to find (and create) the cache file location
static void init_cache_path()
{
#ifdef __unix__
    char dir[MAX_PATH_CHARS + 1];
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg != NULL && xdg[0] != '\0')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else if (home != NULL)
        snprintf(dir, sizeof(dir), "%s/.cache", home);
    else
        return;
    mkdir(dir, 0755);
    size_t length = strlen(dir);
    snprintf(dir + length, sizeof(dir) - length, "/%s", EXECUTABLE_TITLE);
    mkdir(dir, 0755);
    snprintf(cache_path, sizeof(cache_path), "%s/wallpaper", dir);

    // Remove a temporary file left behind if a previous save was interrupted
    char tmp_path[MAX_PATH_CHARS + 8];
    temp_cache_path(tmp_path, sizeof(tmp_path));
    remove(tmp_path);
#endif
}

// A function to get the path of the temporary file used while saving the cache
static void temp_cache_path(char *path, size_t size)
{
    snprintf(path, size, "%s.tmp", cache_path);
}

// A function to save a downloaded image to the cache safely: the data is written to a
// temporary file, flushed to disk, and then renamed over the cache file in one step, so an
// interrupted write never leaves a partial image behind
static bool save_cache_file(const char *data, size_t size)
{
    char tmp_path[MAX_PATH_CHARS + 8];
    temp_cache_path(tmp_path, sizeof(tmp_path));
    FILE *file = fopen(tmp_path, "wb");
    if (file == NULL)
        return false;
    bool ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
#ifdef __unix__
    ok = ok && fsync(fileno(file)) == 0;
#endif
    ok = (fclose(file) == 0) && ok;
    if (ok)
        ok = rename(tmp_path, cache_path) == 0;
    if (!ok)
        remove(tmp_path);
    return ok;
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
    if (buffer->size + length > MAX_IMAGE_BYTES)
        return 0;
    char *data = realloc(buffer->data, buffer->size + length);
    if (data == NULL)
        return 0;
    memcpy(data + buffer->size, ptr, length);
    buffer->data = data;
    buffer->size += length;
    return length;
}

// Stops the transfer when the launcher quits
static int progress_callback(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void) userdata; (void) dltotal; (void) dlnow; (void) ultotal; (void) ulnow;
    return SDL_AtomicGet(&abort_download) ? 1 : 0;
}

// A function to download a URL into a buffer (NUL-terminated, so JSON can be parsed)
static bool fetch(const char *address, Buffer *buffer)
{
    bool ok = false;
    CURL *curl = curl_easy_init();
    if (curl == NULL)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, address);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, EXECUTABLE_TITLE);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK)
        log_error("Could not download %s\n%s", address, curl_easy_strerror(result));
    else if (write_callback("", 1, 1, buffer) == 1) { // NUL terminator
        buffer->size--;
        ok = true;
    }
    curl_easy_cleanup(curl);
    return ok;
}

// A function to make a relative image URL (e.g. "/image.jpg") absolute, using the request URL's origin
static char *absolute_url(const char *image_url)
{
    if (image_url[0] != '/')
        return strdup(image_url);
    if (image_url[1] == '/') { // Protocol-relative: "//host/path"
        char *out = malloc(strlen(image_url) + 7);
        strcpy(out, "https:");
        strcat(out, image_url);
        return out;
    }
    const char *host = strstr(request_url, "://");
    if (host == NULL)
        return NULL;
    const char *path = strchr(host + 3, '/');
    size_t origin = path != NULL ? (size_t) (path - request_url) : strlen(request_url);
    char *out = malloc(origin + strlen(image_url) + 1);
    memcpy(out, request_url, origin);
    strcpy(out + origin, image_url);
    return out;
}

// Download thread: fetch the image (via the JSON response if ImageJson is set), cache it,
// and apply the background filters
static int download_image(void *data)
{
    (void) data;
    SDL_Surface *surface = NULL;
    Buffer buffer = {NULL, 0};
    bool ok = fetch(request_url, &buffer);

    // The URL returned JSON: find the image URL in it, then download the image
    if (ok && config.image_json != NULL) {
        char *image_url = json_get_string(buffer.data, config.image_json);
        char *absolute = image_url != NULL ? absolute_url(image_url) : NULL;
        free(image_url);
        free(buffer.data);
        buffer = (Buffer) {NULL, 0};
        if (absolute == NULL) {
            log_error("No image URL at '%s' in the response from %s", config.image_json, request_url);
            ok = false;
        }
        else {
            log_debug("Background image: %s", absolute);
            ok = fetch(absolute, &buffer);
            free(absolute);
        }
    }

    if (ok) {
        surface = IMG_Load_RW(SDL_RWFromConstMem(buffer.data, (int) buffer.size), 1);
        if (surface == NULL)
            log_error("Could not decode background image from %s\n%s", request_url, IMG_GetError());
        else if (cache_path[0] != '\0' && !save_cache_file(buffer.data, buffer.size))
            log_error("Could not save background image to cache %s", cache_path);
    }
    free(buffer.data);
    downloaded_surface = apply_background_filters(surface);
    SDL_AtomicSet(&download_state, DOWNLOAD_DONE);
    return 0;
}
#endif

static void start_download()
{
#ifdef HAVE_CURL
    if (SDL_AtomicGet(&download_state) != DOWNLOAD_IDLE) {
        download_again = true;
        return;
    }
    download_again = false;
    free(request_url);
    request_url = expand_url(url);
    log_debug("Background image request: %s", request_url);
    SDL_AtomicSet(&download_state, DOWNLOAD_RUNNING);
    download_thread = SDL_CreateThread(download_image, "Background Download Thread", NULL);
    if (download_thread == NULL)
        SDL_AtomicSet(&download_state, DOWNLOAD_IDLE);
#endif
}

// A function to show the cached image right away and start downloading a new one
void init_web_background()
{
#ifdef HAVE_CURL
    if (url != NULL) // Already initialized (e.g. by :wallpaper before switching to the picture background)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    SDL_AtomicSet(&download_state, DOWNLOAD_IDLE);
    SDL_AtomicSet(&abort_download, 0);
    url = strdup(config.background_image);
    init_cache_path();
    if (cache_path[0] != '\0') {
        SDL_Surface *cached = IMG_Load(cache_path);
        if (cached != NULL)
            background_texture = load_texture(apply_background_filters(cached));
    }
    start_download();
#else
    log_error("This build can't load background images from URLs (built without libcurl)");
#endif
}

// A function to fade in a finished download, and start new downloads when they are due
void update_web_background()
{
    if (url == NULL)
        return;

    // A download finished: fade the new image in
    if (SDL_AtomicGet(&download_state) == DOWNLOAD_DONE) {
        SDL_WaitThread(download_thread, NULL);
        download_thread = NULL;
        if (downloaded_surface != NULL) {
            SDL_DestroyTexture(fade_texture);
            fade_texture = load_texture(downloaded_surface);
            fade_start = ticks.main;
            next_download = config.image_refresh > 0 ? ticks.main + config.image_refresh : 0;
        }
        else
            next_download = ticks.main + (config.image_refresh > 0 && config.image_refresh < RETRY_PERIOD ?
                                          config.image_refresh : RETRY_PERIOD);
        downloaded_surface = NULL;
        SDL_AtomicSet(&download_state, DOWNLOAD_IDLE);

        // A new image was requested while this one was downloading (e.g. new keywords)
        if (download_again)
            next_download = ticks.main;
    }

    // Finish the fade
    if (fade_texture != NULL && ticks.main - fade_start >= config.slideshow_transition_time) {
        SDL_DestroyTexture(background_texture);
        background_texture = fade_texture;
        SDL_SetTextureAlphaMod(background_texture, 0xFF);
        fade_texture = NULL;
    }

    // Time for a new image
    if (next_download != 0 && fade_texture == NULL && (Sint32) (ticks.main - next_download) >= 0) {
        next_download = 0;
        start_download();
    }
}

// A function to draw the new image fading in over the current one
void draw_web_background_transition()
{
    if (fade_texture == NULL)
        return;
    Uint32 elapsed = ticks.main - fade_start;
    Uint32 duration = config.slideshow_transition_time > 0 ? config.slideshow_transition_time : 1;
    Uint8 alpha = elapsed >= duration ? 0xFF : (Uint8) (255 * elapsed / duration);
    SDL_SetTextureBlendMode(fade_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(fade_texture, alpha);
    SDL_RenderCopy(renderer, fade_texture, NULL, NULL);
}

// A function to download a new URL background image now (:wallpaper), optionally with new
// keywords, which are saved to the config file
void new_web_background(const char *keywords)
{
    if (keywords != NULL) {
        while (*keywords == ' ')
            keywords++;
        if (*keywords != '\0') {
            free(config.image_keywords);
            config.image_keywords = strdup(keywords);
            log_debug("Wallpaper keywords: %s", keywords);
            if (config.config_file_path != NULL &&
            !save_config_setting(config.config_file_path, "Background", SETTING_IMAGE_KEYWORDS, keywords))
                log_error("Could not save the wallpaper keywords to the config file");
        }
    }
    if (!is_web_image(config.background_image))
        return;
    if (url == NULL)
        init_web_background();
    else
        start_download();
}

// A function to stop any download in progress and free the background resources
void quit_web_background()
{
    if (url == NULL)
        return;
    SDL_AtomicSet(&abort_download, 1);
    if (download_thread != NULL) {
        SDL_WaitThread(download_thread, NULL);
        download_thread = NULL;
    }
    if (downloaded_surface != NULL) {
        SDL_FreeSurface(downloaded_surface);
        downloaded_surface = NULL;
    }
    if (fade_texture != NULL) {
        SDL_DestroyTexture(fade_texture);
        fade_texture = NULL;
    }
    free(url);
    url = NULL;
    free(request_url);
    request_url = NULL;
#ifdef HAVE_CURL
    curl_global_cleanup();
#endif
}
