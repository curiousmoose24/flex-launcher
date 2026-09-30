#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include <launcher_config.h>
#include "layouts.h"
#include "image.h"
#include "sound.h"
#include "util.h"
#include "debug.h"

extern Config config;
extern SDL_Renderer *renderer;
extern Geometry geo;

// Layout schemes modeled on game console home menus. The launcher's settings
// describe the layout, so a scheme is only a name here: the built-in PS3 scheme
// is the one the settings produce.
typedef struct {
    const char *id;          // Value of the Scheme setting
    const char *name;        // Shown in the popup
    const char *description; // Shown beside the name
} Layout;

static const Layout layouts[] = {
    {"PS3", "PlayStation 3", "Cross Media Bar"}
};

#define NUM_LAYOUTS ((int) (sizeof(layouts) / sizeof(layouts[0])))
#define POPUP_TITLE "Layout"
#define POPUP_HINT "Select: Enter / A      Close: Back / B"
#define POPUP_OPEN_TIME 180
#define POPUP_CLOSE_TIME 140
#define POPUP_DIM_ALPHA 150
#define POPUP_BAND_ALPHA 225
#define POPUP_LINE_ALPHA 170
#define POPUP_HIGHLIGHT_ALPHA 45
#define POPUP_DESCRIPTION_ALPHA 150
#define RADIO_OFF_FORMAT "<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"9\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\"/></svg>"
#define RADIO_ON_FORMAT "<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"9\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\"/><circle cx=\"12\" cy=\"12\" r=\"5\" fill=\"#FFFFFF\"/></svg>"

typedef struct {
    SDL_Texture *texture;
    SDL_Rect rect;
} Text;

typedef enum {
    POPUP_CLOSED,
    POPUP_OPEN,
    POPUP_CLOSING
} PopupState;

static struct {
    PopupState state;
    Uint32 start; // Ticks when the popup started opening or closing
    int selected;
    SDL_Rect band;
    int row_height;
    int row_x;
    int row_width;
    int rows_y;
    Text title;
    Text hint;
    Text names[NUM_LAYOUTS];
    Text descriptions[NUM_LAYOUTS];
    SDL_Texture *radio_off;
    SDL_Texture *radio_on;
    int radio_size;
} popup = {.state = POPUP_CLOSED};

// A function to find a layout scheme by its id; returns its index or -1
int find_layout(const char *id)
{
    for (int i = 0; i < NUM_LAYOUTS; i++) {
        if (!SDL_strcasecmp(id, layouts[i].id))
            return i;
    }
    return -1;
}

const char *layout_id(int layout)
{
    return layouts[layout >= 0 && layout < NUM_LAYOUTS ? layout : 0].id;
}

static void render_popup_text(Text *text, TTF_Font *font, const char *string, Uint8 alpha)
{
    SDL_Color white = {0xFF, 0xFF, 0xFF, 0xFF};
    text->texture = NULL;
    text->rect = (SDL_Rect) {0, 0, 0, 0};
    if (font == NULL)
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, string, white);
    if (surface == NULL)
        return;
    text->rect.w = surface->w;
    text->rect.h = surface->h;
    text->texture = load_texture(surface);
    if (text->texture != NULL)
        SDL_SetTextureAlphaMod(text->texture, alpha);
}

static TTF_Font *open_popup_font(float screen_fraction)
{
    int size = (int) ((float) geo.screen_height * screen_fraction + 0.5f);
    TTF_Font *font = TTF_OpenFont(config.title_font_path, size > 8 ? size : 8);
    if (font == NULL)
        log_error("Could not open the font for the layout popup\n%s", TTF_GetError());
    return font;
}

static void destroy_text(Text *text)
{
    if (text->texture != NULL)
        SDL_DestroyTexture(text->texture);
    text->texture = NULL;
}

static void free_popup()
{
    destroy_text(&popup.title);
    destroy_text(&popup.hint);
    for (int i = 0; i < NUM_LAYOUTS; i++) {
        destroy_text(&popup.names[i]);
        destroy_text(&popup.descriptions[i]);
    }
    if (popup.radio_off != NULL)
        SDL_DestroyTexture(popup.radio_off);
    if (popup.radio_on != NULL)
        SDL_DestroyTexture(popup.radio_on);
    popup.radio_off = NULL;
    popup.radio_on = NULL;
    popup.state = POPUP_CLOSED;
}

// A function to render the popup's text and lay it out as a band across the screen
static void render_popup()
{
    TTF_Font *title_font = open_popup_font(0.050f);
    TTF_Font *item_font = open_popup_font(0.038f);
    TTF_Font *small_font = open_popup_font(0.024f);

    render_popup_text(&popup.title, title_font, POPUP_TITLE, 0xFF);
    render_popup_text(&popup.hint, small_font, POPUP_HINT, POPUP_DESCRIPTION_ALPHA);
    for (int i = 0; i < NUM_LAYOUTS; i++) {
        render_popup_text(&popup.names[i], item_font, layouts[i].name, 0xFF);
        render_popup_text(&popup.descriptions[i], small_font, layouts[i].description, POPUP_DESCRIPTION_ALPHA);
    }
    int item_height = item_font != NULL ? TTF_FontHeight(item_font) : geo.screen_height / 26;
    if (title_font != NULL)
        TTF_CloseFont(title_font);
    if (item_font != NULL)
        TTF_CloseFont(item_font);
    if (small_font != NULL)
        TTF_CloseFont(small_font);

    popup.radio_size = item_height * 3 / 4;
    char *svg = strdup(RADIO_OFF_FORMAT);
    popup.radio_off = rasterize_svg(svg, popup.radio_size, popup.radio_size, NULL);
    free(svg);
    svg = strdup(RADIO_ON_FORMAT);
    popup.radio_on = rasterize_svg(svg, popup.radio_size, popup.radio_size, NULL);
    free(svg);

    // Geometry: title on the left of the band, the list of layouts to its right
    int margin = geo.screen_height / 30;
    popup.row_height = item_height * 17 / 10;
    popup.row_x = geo.screen_width * 30 / 100;
    popup.row_width = geo.screen_width * 45 / 100;
    int rows_height = popup.row_height * NUM_LAYOUTS;
    int content_height = popup.title.rect.h > rows_height ? popup.title.rect.h : rows_height;
    popup.band.w = geo.screen_width;
    popup.band.h = margin + content_height + margin / 2 + popup.hint.rect.h + margin;
    popup.band.x = 0;
    popup.band.y = (geo.screen_height - popup.band.h) / 2;
    popup.rows_y = popup.band.y + margin;
    popup.title.rect.x = geo.screen_width * 8 / 100;
    popup.title.rect.y = popup.rows_y + (popup.row_height - popup.title.rect.h) / 2;
    popup.hint.rect.x = popup.row_x + popup.row_width - popup.hint.rect.w;
    popup.hint.rect.y = popup.band.y + popup.band.h - margin - popup.hint.rect.h;
    for (int i = 0; i < NUM_LAYOUTS; i++) {
        int row_y = popup.rows_y + i * popup.row_height;
        Text *name = &popup.names[i];
        Text *description = &popup.descriptions[i];
        name->rect.x = popup.row_x + popup.row_height / 3 + popup.radio_size + popup.row_height / 3;
        name->rect.y = row_y + (popup.row_height - name->rect.h) / 2;
        description->rect.x = popup.row_x + popup.row_width - popup.row_height / 3 - description->rect.w;
        description->rect.y = row_y + (popup.row_height - description->rect.h) / 2;
    }
}

bool layout_popup_active()
{
    return popup.state != POPUP_CLOSED;
}

// A function to open the layout popup, with the current scheme selected
void open_layout_popup()
{
    if (popup.state == POPUP_CLOSING)
        free_popup();
    if (popup.state == POPUP_OPEN)
        return;
    render_popup();
    popup.selected = config.layout_scheme >= 0 && config.layout_scheme < NUM_LAYOUTS ? config.layout_scheme : 0;
    popup.state = POPUP_OPEN;
    popup.start = SDL_GetTicks();
    log_debug("Opened the layout popup");
}

static void close_popup()
{
    popup.state = POPUP_CLOSING;
    popup.start = SDL_GetTicks();
}

// A function to apply the selected layout scheme and save it to the config file
static void choose_layout(int layout)
{
    if (layout != config.layout_scheme) {
        config.layout_scheme = layout;
        log_debug("Layout scheme: %s", layouts[layout].id);
        if (config.config_file_path != NULL &&
        !save_config_setting(config.config_file_path, "Layout", SETTING_LAYOUT_SCHEME, layouts[layout].id))
            log_error("Could not save the layout setting to the config file");
    }
}

// A function to handle a navigation command while the popup is open
void layout_popup_command(const char *command)
{
    if (popup.state != POPUP_OPEN)
        return;
    if (!strcmp(command, SCMD_UP) || !strcmp(command, SCMD_DOWN)) {
        int selected = popup.selected + (!strcmp(command, SCMD_UP) ? -1 : 1);
        if (selected >= 0 && selected < NUM_LAYOUTS) {
            popup.selected = selected;
            play_sound(SOUND_MOVE);
        }
    }
    else if (!strcmp(command, SCMD_SELECT)) {
        play_sound(SOUND_CONFIRM);
        choose_layout(popup.selected);
        close_popup();
    }
    else if (!strcmp(command, SCMD_BACK) || !strcmp(command, SCMD_LEFT) || !strcmp(command, SCMD_HOME)) {
        play_sound(SOUND_BACK);
        close_popup();
    }
}

static void fill_rect(const SDL_Rect *rect, Uint8 alpha)
{
    SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, alpha);
    SDL_RenderFillRect(renderer, rect);
}

static void draw_text(const Text *text, float fade)
{
    if (text->texture == NULL)
        return;
    Uint8 alpha;
    SDL_GetTextureAlphaMod(text->texture, &alpha);
    SDL_SetTextureAlphaMod(text->texture, (Uint8) ((float) alpha * fade));
    SDL_RenderCopy(renderer, text->texture, NULL, &text->rect);
    SDL_SetTextureAlphaMod(text->texture, alpha);
}

// A function to draw the popup over the menu
void draw_layout_popup()
{
    if (popup.state == POPUP_CLOSED)
        return;

    // Fade in when opening and out when closing
    Uint32 elapsed = SDL_GetTicks() - popup.start;
    float fade;
    if (popup.state == POPUP_OPEN)
        fade = elapsed >= POPUP_OPEN_TIME ? 1.0f : (float) elapsed / POPUP_OPEN_TIME;
    else {
        if (elapsed >= POPUP_CLOSE_TIME) {
            free_popup();
            return;
        }
        fade = 1.0f - (float) elapsed / POPUP_CLOSE_TIME;
    }
    fade = fade * fade * (3.0f - 2.0f * fade);

    Uint8 r, g, b, a;
    SDL_BlendMode mode;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_GetRenderDrawBlendMode(renderer, &mode);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Dim the menu, then draw a dark band edged with thin lines, like the PS3's dialogs
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8) (POPUP_DIM_ALPHA * fade));
    SDL_RenderFillRect(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x14, (Uint8) (POPUP_BAND_ALPHA * fade));
    SDL_RenderFillRect(renderer, &popup.band);
    int line = geo.screen_height / 540 > 0 ? geo.screen_height / 540 : 1;
    SDL_Rect top_line = {0, popup.band.y, geo.screen_width, line};
    SDL_Rect bottom_line = {0, popup.band.y + popup.band.h - line, geo.screen_width, line};
    fill_rect(&top_line, (Uint8) (POPUP_LINE_ALPHA * fade));
    fill_rect(&bottom_line, (Uint8) (POPUP_LINE_ALPHA * fade));

    draw_text(&popup.title, fade);
    for (int i = 0; i < NUM_LAYOUTS; i++) {
        int row_y = popup.rows_y + i * popup.row_height;
        if (i == popup.selected) {
            SDL_Rect highlight = {popup.row_x, row_y, popup.row_width, popup.row_height};
            fill_rect(&highlight, (Uint8) (POPUP_HIGHLIGHT_ALPHA * fade));
            SDL_Rect edge = {popup.row_x, row_y, line * 3, popup.row_height};
            fill_rect(&edge, (Uint8) (0xFF * fade));
        }
        SDL_Texture *radio = i == config.layout_scheme ? popup.radio_on : popup.radio_off;
        if (radio != NULL) {
            SDL_Rect radio_rect = {
                popup.row_x + popup.row_height / 3,
                row_y + (popup.row_height - popup.radio_size) / 2,
                popup.radio_size,
                popup.radio_size
            };
            SDL_SetTextureAlphaMod(radio, (Uint8) (0xFF * fade));
            SDL_RenderCopy(renderer, radio, NULL, &radio_rect);
        }
        draw_text(&popup.names[i], fade);
        draw_text(&popup.descriptions[i], fade);
    }
    draw_text(&popup.hint, fade);

    SDL_SetRenderDrawBlendMode(renderer, mode);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
}

void quit_layout_popup()
{
    free_popup();
}
