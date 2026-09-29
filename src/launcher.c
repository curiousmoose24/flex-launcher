#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <SDL_thread.h>
#include "launcher.h"
#include <launcher_config.h>
#include "image.h"
#include "sound.h"
#include "wave.h"
#include "webimage.h"
#include "layouts.h"
#include "util.h"
#include "debug.h"
#include "clock.h"
#include "platform/platform.h"

static void init_sdl(void);
static void init_sdl_image(void);
static void create_window(void);
static void init_sdl_ttf(void);
static int load_menu(Menu *menu, bool set_back_menu, bool reset_position);
static int load_menu_by_name(const char *menu_name, bool set_back_menu, bool reset_position);
static void update_slideshow(void);
static void resume_slideshow(void);
static void update_screensaver(void);
static void update_clock(bool block);
static void init_slideshow(void);
static void init_screensaver(void);
static void calculate_button_geometry(Entry *entry, int buttons);
static bool carousel_active(void);
static Entry *next_entry_wrapped(Entry *entry);
static Entry *previous_entry_wrapped(Entry *entry);
static int carousel_offset(void);
static void carousel_layout(void);
static void carousel_move(Direction direction);
static void draw_carousel_buttons(void);
static void render_copy_alpha(SDL_Texture *texture, const SDL_Rect *rect, Uint8 alpha);
static SDL_Texture *entry_icon(Entry *entry, bool selected);
static void draw_title_glow(Entry *entry, const SDL_Rect *text_rect, float strength);
static void load_picture_background(void);
static void toggle_background(void);
static bool column_active(void);
static void update_column(void);
static float column_offset(void);
static void column_move(bool down);
static void draw_column(float category_center, float category_closeness);
static Entry *selected_entry(void);
static void move_up(void);
static void move_down(void);
static void render_buttons(Menu *menu);
static void move_left(void);
static void move_right(void);
static void load_submenu(const char *submenu);
static void load_back_menu(Menu *menu);
static void draw_screen(void);
static void start_fade_in(void);
static void draw_black_overlay(Uint8 alpha);
static void handle_keypress(SDL_Keysym *key);
static void execute_command(const char *command);
static void poll_gamepad(void);
static void init_gamepad(Gamepad **gamepad, int device_index);
static void connect_gamepad(int device_index, bool open, bool raise_error);
static void disconnect_gamepad(int id, bool disconnect, bool remove);
static void open_controller(Gamepad *gamepad, bool raise_error);
static void cleanup(void);

// Initialize default settings
Config config = {
    .default_menu                     = NULL,
    .background_image                 = NULL,
    .slideshow_directory              = NULL,
    .title_font_path                  = NULL,
    .vsync                            = true,
    .fps_limit                        = -1,
    .application_timeout              = DEFAULT_APPLICATION_TIMEOUT * 1000,
    .titles_enabled                   = DEFAULT_TITLES_ENABLED,
    .title_font_size                  = DEFAULT_FONT_SIZE,
    .title_font_color.r               = DEFAULT_TITLE_FONT_COLOR_R,
    .title_font_color.g               = DEFAULT_TITLE_FONT_COLOR_G,
    .title_font_color.b               = DEFAULT_TITLE_FONT_COLOR_B,
    .title_font_color.a               = DEFAULT_TITLE_FONT_COLOR_A,
    .title_shadows                    = DEFAULT_TITLE_SHADOWS,
    .title_shadow_color.r             = DEFAULT_TITLE_SHADOW_COLOR_R,
    .title_shadow_color.g             = DEFAULT_TITLE_SHADOW_COLOR_G,
    .title_shadow_color.b             = DEFAULT_TITLE_SHADOW_COLOR_B,
    .title_shadow_color.a             = DEFAULT_TITLE_SHADOW_COLOR_A,
    .background_mode                  = BACKGROUND_COLOR,
    .background_color.r               = DEFAULT_BACKGROUND_COLOR_R,
    .background_color.g               = DEFAULT_BACKGROUND_COLOR_G,
    .background_color.b               = DEFAULT_BACKGROUND_COLOR_B,
    .chroma_key_color.r               = DEFAULT_CHROMA_KEY_COLOR_R,
    .chroma_key_color.g               = DEFAULT_CHROMA_KEY_COLOR_G,
    .chroma_key_color.b               = DEFAULT_CHROMA_KEY_COLOR_B,
    .chroma_key_color.a               = DEFAULT_CHROMA_KEY_COLOR_A,
    .background_color.a               = 0xFF,
    .background_overlay               = DEFAULT_BACKGROUND_OVERLAY,
    .background_overlay_color.r       = DEFAULT_BACKGROUND_OVERLAY_COLOR_R,
    .background_overlay_color.g       = DEFAULT_BACKGROUND_OVERLAY_COLOR_G,
    .background_overlay_color.b       = DEFAULT_BACKGROUND_OVERLAY_COLOR_B,
    .background_overlay_color.a       = DEFAULT_BACKGROUND_OVERLAY_COLOR_A,
    .background_overlay_opacity[0]    = '\0',
    .highlight                        = true,
    .icon_size                        = DEFAULT_ICON_SIZE,
    .highlight_fill_color.r           = DEFAULT_HIGHLIGHT_FILL_COLOR_R,
    .highlight_fill_color.g           = DEFAULT_HIGHLIGHT_FILL_COLOR_G,
    .highlight_fill_color.b           = DEFAULT_HIGHLIGHT_FILL_COLOR_B,
    .highlight_fill_color.a           = DEFAULT_HIGHLIGHT_FILL_COLOR_A,
    .highlight_outline_color.r        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_R,
    .highlight_outline_color.g        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_G,
    .highlight_outline_color.b        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_B,
    .highlight_outline_color.a        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_A,
    .highlight_outline_size           = DEFAULT_HIGHLIGHT_OUTLINE_SIZE,
    .highlight_rx                     = DEFAULT_HIGHLIGHT_CORNER_RADIUS,
    .title_padding                    = -1,
    .max_buttons                      = DEFAULT_MAX_BUTTONS,
    .icon_spacing                     = -1,
    .highlight_vpadding               = -1,
    .highlight_hpadding               = -1,
    .title_opacity[0]                 = '\0',
    .highlight_fill_opacity[0]        = '\0',
    .highlight_outline_opacity[0]     = '\0',
    .vcenter[0]             = '\0',
    .icon_spacing_str[0]              = '\0',
    .scroll_indicators                = DEFAULT_SCROLL_INDICATORS,
    .scroll_indicator_fill_color.r    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_R,
    .scroll_indicator_fill_color.g    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_G,
    .scroll_indicator_fill_color.b    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_B,
    .scroll_indicator_fill_color.a    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_A,
    .scroll_indicator_outline_size    = DEFAULT_SCROLL_INDICATOR_OUTLINE_SIZE,
    .scroll_indicator_outline_color.r = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_R,
    .scroll_indicator_outline_color.g = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_G,
    .scroll_indicator_outline_color.b = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_B,
    .scroll_indicator_outline_color.a = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_A,
    .scroll_indicator_opacity[0]      = '\0',
    .title_oversize_mode              = OVERSIZE_TRUNCATE,
    .wrap_entries                     = DEFAULT_WRAP_ENTRIES,
    .scroll_mode                      = SCROLL_MODE_PAGED,
    .scroll_time                      = DEFAULT_SCROLL_TIME,
    .focus_scale                      = 1.0f,
    .focus_position                   = 0.5f,
    .unfocused_alpha                  = 0xFF,
    .titles_focused_only              = DEFAULT_TITLE_FOCUSED_ONLY,
    .title_glow                       = DEFAULT_TITLE_GLOW,
    .title_glow_color                 = {0xFF, 0xFF, 0xFF, 0x99},
    .submenu_mode                     = SUBMENU_MODE_SCREEN,
    .layout_scheme                    = 0,
    .column_icon_scale                = 0.6f,
    .column_focus_scale               = 0.0f,
    .fade_time                        = DEFAULT_FADE_TIME,
    .wave_color_mode                  = WAVE_COLOR_MONTH,
    .image_refresh                    = 0,
    .image_json                       = NULL,
    .image_keywords                   = NULL,
    .image_blur                       = 0.0f,
    .image_brightness                 = 1.0f,
    .image_saturation                 = 1.0f,
    .image_opacity                    = 1.0f,
    .wave_color                       = {0x2D, 0x6F, 0xD6, 0xFF},
    .wave_time_of_day                 = DEFAULT_WAVE_TIME_OF_DAY,
    .sounds_enabled                   = DEFAULT_SOUNDS_ENABLED,
    .sound_volume                     = SDL_MIX_MAXVOLUME / 2,
    .sound_paths                      = {NULL},
    .config_file_path                 = NULL,
    .reset_on_back                    = DEFAULT_RESET_ON_BACK,
    .mouse_select                     = DEFAULT_MOUSE_SELECT,
    .inhibit_os_screensaver           = DEFAULT_INHIBIT_OS_SCREENSAVER,
    .startup_cmd                      = NULL,
    .quit_cmd                         = NULL,
    .screensaver_enabled              = false,
    .screensaver_idle_time            = DEFAULT_SCREENSAVER_IDLE_TIME*1000,
    .screensaver_intensity_str[0]     = '\0',
    .screensaver_pause_slideshow      = DEFAULT_SCREENSAVER_PAUSE_SLIDESHOW,
    .gamepad_enabled                  = DEFAULT_GAMEPAD_ENABLED,
    .gamepad_device                   = DEFAULT_GAMEPAD_DEVICE,
    .gamepad_mappings_file            = NULL,
    .on_launch                        = ON_LAUNCH_BLANK,
    .debug                            = false,
    .exe_path                         = NULL,
    .first_menu                       = NULL,
    .num_menus                        = 0,
    .clock_enabled                    = DEFAULT_CLOCK_ENABLED,
    .clock_show_date                  = DEFAULT_CLOCK_SHOW_DATE,
    .clock_alignment                  = DEFAULT_CLOCK_ALIGNMENT,
    .clock_font_path                  = NULL,
    .clock_margin_str[0]              = '\0',
    .clock_margin                     = -1,
    .clock_font_color.r               = DEFAULT_CLOCK_FONT_COLOR_R,
    .clock_font_color.g               = DEFAULT_CLOCK_FONT_COLOR_G,
    .clock_font_color.b               = DEFAULT_CLOCK_FONT_COLOR_B,
    .clock_font_color.a               = DEFAULT_CLOCK_FONT_COLOR_A,
    .clock_shadows                    = DEFAULT_CLOCK_SHADOWS,
    .clock_shadow_color.r             = DEFAULT_CLOCK_SHADOW_COLOR_R,
    .clock_shadow_color.g             = DEFAULT_CLOCK_SHADOW_COLOR_G,
    .clock_shadow_color.b             = DEFAULT_CLOCK_SHADOW_COLOR_B,
    .clock_shadow_color.a             = DEFAULT_CLOCK_SHADOW_COLOR_A,
    .clock_opacity[0]                 = '\0',
    .clock_font_size                  = DEFAULT_CLOCK_FONT_SIZE,
    .clock_time_format                = DEFAULT_CLOCK_TIME_FORMAT,
    .clock_date_format                = DEFAULT_CLOCK_DATE_FORMAT,
    .clock_include_weekday            = DEFAULT_CLOCK_INCLUDE_WEEKDAY,
    .slideshow_image_duration         = DEFAULT_SLIDESHOW_IMAGE_DURATION,
    .slideshow_transition_time        = DEFAULT_SLIDESHOW_TRANSITION_TIME
};

// Initialize default states
State state = { false };

// Global variables
SDL_Window *window                    = NULL;
SDL_Renderer *renderer                = NULL;
SDL_Texture *background_texture       = NULL;
SDL_Texture *background_overlay       = NULL;
Menu *default_menu                    = NULL;
Menu *current_menu                    = NULL;
Entry *current_entry                  = NULL;
Highlight *highlight                  = NULL;
Scroll *scroll                        = NULL;
Slideshow *slideshow                  = NULL;
Screensaver *screensaver              = NULL;
FILE *log_file                        = NULL;
Gamepad *gamepads                     = NULL;
GamepadControl *gamepad_controls      = NULL;
Hotkey *hotkeys                       = NULL;
Clock *clk                            = NULL;
TTF_Font *clock_font                  = NULL;
SDL_Thread *Slideshowhread            = NULL;
SDL_Thread *clock_thread              = NULL;
SDL_Event event;
SDL_SysWMinfo wm_info;
SDL_DisplayMode display_mode;
TextInfo title_info;
Ticks ticks;
Geometry geo;
Uint32 refresh_period;
Uint32 delay_period;
Uint32 repeat_period;


// A function to initialize SDL
static void init_sdl()
{    
    // Set flags, hints
    Uint32 sdl_flags = SDL_INIT_VIDEO;
#ifdef __unix__
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, config.inhibit_os_screensaver ? "0" : "1");
    if (config.gamepad_enabled)
        sdl_flags |= SDL_INIT_GAMECONTROLLER;

    // Initialize SDL
    if (SDL_Init(sdl_flags) < 0)
        log_fatal("Could not initialize SDL\n%s", SDL_GetError());

    SDL_GetDesktopDisplayMode(0, &display_mode);
    geo.screen_width = display_mode.w;
    geo.screen_height = display_mode.h;
    refresh_period = 1000 / (Uint32) display_mode.refresh_rate;
    geo.screen_margin = (int) (SCREEN_MARGIN * (float) geo.screen_height);
}

// A function to create the window and renderer
static void create_window()
{
    window = SDL_CreateWindow(PROJECT_NAME,
                 SDL_WINDOWPOS_UNDEFINED,
                 SDL_WINDOWPOS_UNDEFINED,
                 0,
                 0,
                 SDL_WINDOW_FULLSCREEN_DESKTOP
             );
    if (window == NULL)
        log_fatal("Could not create SDL Window\n%s", SDL_GetError());
    SDL_ShowCursor(SDL_DISABLE);

    // Create HW accelerated renderer, get screen resolution for geometry calculations
    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (!config.vsync) {
        if (config.fps_limit > MIN_FPS_LIMIT && config.fps_limit <= display_mode.refresh_rate)
            refresh_period = 1000 / (Uint32) config.fps_limit;
        else
            config.vsync = true;
    }
    if (config.vsync) {
        refresh_period = 1000 / (Uint32) display_mode.refresh_rate;
        renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
    }
    if (config.gamepad_enabled) {
        delay_period = GAMEPAD_REPEAT_DELAY / refresh_period;
        repeat_period = GAMEPAD_REPEAT_INTERVAL / refresh_period;
        if (!repeat_period)
            repeat_period = 1;
    }
    if (slideshow != NULL)
        slideshow->transition_change_rate = 255.0f / ((float) config.slideshow_transition_time / (float) refresh_period);

    renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (renderer == NULL)
        log_fatal("Could not initialize renderer\n%s", SDL_GetError());

    // Set background color
    set_draw_color();

#ifdef _WIN32
    SDL_VERSION(&wm_info.version);
    SDL_GetWindowWMInfo(window, &wm_info);
    if (config.background_mode == BACKGROUND_TRANSPARENT)
        make_window_transparent();
#endif
}

// A function to initialize the SDL_image library
static void init_sdl_image()
{
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
    if (!(IMG_Init(img_flags) & img_flags))
        log_fatal("Could not initialize SDL_image\n%s", IMG_GetError());
}

// A function to set the color of the renderer
void set_draw_color()
{
    SDL_Color *color = NULL;
    if (config.background_mode != BACKGROUND_TRANSPARENT)
        color = &config.background_color; // Also shown behind images until they have loaded
    else if (config.background_mode == BACKGROUND_TRANSPARENT)
        color = &config.chroma_key_color;

    if (color == NULL)
        SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, 0xFF);
    else
        SDL_SetRenderDrawColor(renderer,
            color->r,
            color->g,
            color->b,
            color->a
        );
}

// A function to initialize SDL's TTF subsystem
static void init_sdl_ttf()
{
    if (TTF_Init() == -1)
        log_fatal("Could not initialize SDL_ttf\n%s", TTF_GetError());
    
    title_info = (TextInfo) { 
        .font_size = (int) config.title_font_size,
        .shadow = config.title_shadows,
        .font_path = &config.title_font_path,
        .max_width = config.icon_size,
        .oversize_mode = config.title_oversize_mode,
        .color = &config.title_font_color
    };
    if (config.title_shadows) {
        title_info.shadow_color = &config.title_shadow_color;
        calculate_shadow_alpha(title_info);
    }
    else
        title_info.shadow_color = NULL;

    int error = load_font(&title_info, FILENAME_DEFAULT_FONT);
    if (error)
        log_fatal("Could not load title font");
    geo.font_height = config.titles_enabled ? TTF_FontHeight(title_info.font) : 0;
}

// A function to close subsystems and free memory before quitting
static void cleanup()
{
    // Wait until all threads have completed
    quit_web_background();
    quit_layout_popup();
    SDL_WaitThread(Slideshowhread, NULL);
    SDL_WaitThread(clock_thread, NULL);
    
    // Destroy renderer and window
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
        window = NULL;
    }

    // Quit subsystems
    quit_sounds();
    SDL_Quit();
    IMG_Quit();
    TTF_Quit();
    quit_svg();
    quit_slideshow();

    // Close log file if open
    if (log_file != NULL)
        fclose(log_file);

    // Free dynamically allocated memory
    free(config.default_menu);
    free(config.background_image);
    free(config.title_font_path);
    free(config.exe_path);
    free(config.slideshow_directory);
    free(config.clock_font_path);
    free(config.gamepad_mappings_file);
    free(config.startup_cmd);
    free(config.quit_cmd);
    free(config.config_file_path);
    free(config.image_json);
    free(config.image_keywords);
    free(highlight);
    free(scroll);
    free(screensaver);
    free(clk);

    // Free menu and entry linked lists
    Entry *entry = NULL;
    Entry *tmp_entry = NULL;
    Menu *menu = config.first_menu;
    Menu *tmp_menu = NULL;
    for (size_t i = 0; i < config.num_menus; i++) {
        free(menu->name);
        entry = menu->first_entry;
        for(size_t j = 0; j < menu->num_entries; j++) {
            free(entry->title);
            free(entry->icon_path);
            free(entry->icon_selected_path);
            free(entry->cmd);
            tmp_entry = entry;
            entry = entry->next;
            free(tmp_entry);
        }
        tmp_menu = menu;
        menu = menu->next;
        free(tmp_menu);
    }

    // Free hotkey linked list
    Hotkey *tmp_hotkey = NULL;
    for(Hotkey *i = hotkeys; i != NULL; i = i->next) {
        free(tmp_hotkey);
        free(i->cmd);
        tmp_hotkey = i;
    }
    free(tmp_hotkey);

    // Free gamepad control linked list
    GamepadControl *tmp_gamepad = NULL;
    for (GamepadControl *i = gamepad_controls; i != NULL; i = i->next) {
        free(tmp_gamepad);
        free(i->cmd);
        tmp_gamepad = i;
    }
    free(tmp_gamepad);

    if (config.gamepad_enabled)
        disconnect_gamepad(-1, false, true);
}

// A function to handle key presses from keyboard
static void handle_keypress(SDL_Keysym *key)
{
    if (config.debug)
        log_debug("Key %s (#%X) detected", SDL_GetKeyName(key->sym), key->sym);

    // The layout popup takes all input while it is open
    if (layout_popup_active()) {
        if (key->sym == SDLK_UP)
            layout_popup_command(SCMD_UP);
        else if (key->sym == SDLK_DOWN)
            layout_popup_command(SCMD_DOWN);
        else if (key->sym == SDLK_RETURN)
            layout_popup_command(SCMD_SELECT);
        else if (key->sym == SDLK_BACKSPACE || key->sym == SDLK_ESCAPE || key->sym == SDLK_LEFT)
            layout_popup_command(SCMD_BACK);
        return;
    }

    // Check default keys
    if (key->sym == SDLK_LEFT)
        move_left();
    else if (key->sym == SDLK_RIGHT)
        move_right();
    else if (key->sym == SDLK_UP)
        move_up();
    else if (key->sym == SDLK_DOWN)
        move_down();
    else if (key->sym == SDLK_RETURN) {
        Entry *entry = selected_entry();
        log_debug("Selected Entry:\n"
            "Title: %s\n"
            "Icon Path: %s\n"
            "Command: %s", 
            entry->title, 
            entry->icon_path, 
            entry->cmd
        );
        
        play_sound(SOUND_SELECT);
        execute_command(entry->cmd);
    }
    else if (key->sym == SDLK_BACKSPACE)
        load_back_menu(current_menu);

    //Check hotkeys
    else {
        for (Hotkey *i = hotkeys; i != NULL; i = i->next) {
            if (key->sym == i->keycode) {
                execute_command(i->cmd);
                break;
            }
        }
    }
}

// A function to quit the slideshow mode in case of error or program exit
void quit_slideshow()
{
    // The slideshow may not be initialized yet if the program exits early (e.g. SDL init failure)
    if (slideshow == NULL)
        return;

    // Free allocated image paths
    for (int i = 0; i < slideshow->num_images; i++)
        free(slideshow->images[i]);
    free(slideshow->images);
    free(slideshow->order);
    free(slideshow);
    slideshow = NULL;
}

// A function to initialize the slideshow background mode
static void init_slideshow()
{
    if (!directory_exists(config.slideshow_directory)) {
        log_error("Slideshow directory '%s' does not exist, "
            "Switching to color background mode",
            config.slideshow_directory
        );
        config.background_mode = BACKGROUND_COLOR;
        set_draw_color();
        return;
    }
    // Allocate and initialize slideshow struct
    slideshow = malloc(sizeof(Slideshow));
    *slideshow = (Slideshow) {
        .i = -1,
        .num_images = 0,
        .transition_surface = NULL,
        .transition_texture = NULL,
        .transition_alpha = 0.f,
        .transition_change_rate = 0.f,
        .images = NULL,
        .order = NULL
    };

    // Find background images from directory
    scan_slideshow_directory(slideshow, config.slideshow_directory);
    
    // Handle errors
    if (!slideshow->num_images) {
        log_error("No images found in slideshow directory '%s', "
            "Changing background mode to color", 
            config.slideshow_directory
        );
        config.background_mode = BACKGROUND_COLOR;
        quit_slideshow();
    } 
    else if (slideshow->num_images == 1) {
        log_error("Only one image found in slideshow directory %s"
            "Changing background mode to single image", 
            config.slideshow_directory
        );
        free(config.background_image);
        config.background_image = strdup(slideshow->images[0]);
        config.background_mode = BACKGROUND_IMAGE;
        quit_slideshow();
    }

    // Generate array of random numbers for image order, load first image
    else {
        slideshow->order = malloc(sizeof(int) * (size_t) slideshow->num_images);
        random_array(slideshow->order, slideshow->num_images);
        if (config.debug)
            debug_slideshow(slideshow);
    }
}

// A function to initialize the screensaver feature
static void init_screensaver()
{
    // Allocate memory for structure
    screensaver = malloc(sizeof(Screensaver));
    
    // Convert intensity string to float
    char intensity[PERCENT_MAX_CHARS];
    if (config.screensaver_intensity_str[0] != '\0')
        copy_string(intensity, config.screensaver_intensity_str, sizeof(intensity));
    else
        copy_string(intensity, DEFAULT_SCREENSAVER_INTENSITY, sizeof(intensity));
    size_t length = strlen(intensity);
    intensity[length - 1] = '\0';
    float percent = (float) atof(intensity);

    // Calculate alpha end value
    screensaver->alpha_end_value = 255.0f * percent / 100.0f;
    if (screensaver->alpha_end_value < 1.0f) {
        log_error("Invalid screensaver intensity value, disabling feature");
        config.screensaver_enabled = false;
        free(screensaver);
        screensaver = NULL;
        return;
    }
    else if (screensaver->alpha_end_value >= 255.0f)
        screensaver->alpha_end_value = 255.0f;

    screensaver->transition_change_rate = screensaver->alpha_end_value / ((float) SCREENSAVER_TRANSITION_TIME / (float) refresh_period);
    
    // Render texture
    SDL_Surface *surface = NULL;
    surface = SDL_CreateRGBSurfaceWithFormat(0, 
                  geo.screen_width, 
                  geo.screen_height, 
                  32,
                  SDL_PIXELFORMAT_ARGB8888
              );
    Uint32 color = SDL_MapRGBA(surface->format, 0, 0, 0, 0xFF);
    SDL_FillRect(surface, NULL, color);
    screensaver->texture = load_texture(surface);
    screensaver->alpha = 0.0f;
    SDL_SetTextureAlphaMod(screensaver->texture, 0.0f);
}

// A function to resume the slideshow after a launched application returns
static void resume_slideshow()
{
    ticks.slideshow_load = ticks.main;
}

// Carousel animation state: the row is drawn shifted by an offset that eases to 0
static int carousel_offset_start = 0;
static Uint32 carousel_anim_start = 0;

// Carousel mode applies to all menus when it doesn't wrap. A wrapping carousel
// only applies to menus with more entries than fit on screen, so that entries
// are not repeated on screen.
static bool carousel_active()
{
    return config.scroll_mode == SCROLL_MODE_CAROUSEL &&
           (!config.wrap_entries || current_menu->num_entries > config.max_buttons);
}

// Get the next entry in the current menu, wrapping around to the first
static Entry *next_entry_wrapped(Entry *entry)
{
    return entry->next != NULL ? entry->next : current_menu->first_entry;
}

// Get the previous entry in the current menu, wrapping around to the last
static Entry *previous_entry_wrapped(Entry *entry)
{
    if (entry->previous != NULL)
        return entry->previous;
    return advance_entries(current_menu->first_entry, (int) current_menu->num_entries - 1, DIRECTION_RIGHT);
}

// Get the current horizontal offset of the carousel scroll animation (ease-out)
static int carousel_offset()
{
    if (carousel_offset_start == 0 || config.scroll_time == 0)
        return 0;
    Uint32 elapsed = ticks.main - carousel_anim_start;
    if (elapsed >= config.scroll_time) {
        carousel_offset_start = 0;
        return 0;
    }
    float remaining = 1.0f - (float) elapsed / (float) config.scroll_time;
    return (int) ((float) carousel_offset_start * remaining * remaining);
}

// Lay out the carousel so the selected entry is in the focus slot
static void carousel_layout()
{
    unsigned int center = (config.max_buttons - 1) / 2;
    Entry *root = current_entry;
    for (unsigned int i = 0; i < center; i++)
        root = previous_entry_wrapped(root);
    current_menu->root_entry = root;
    current_menu->highlight_position = center;
    current_menu->page = 0;
    calculate_button_geometry(root, (int) config.max_buttons);

    // The highlight sits at the focus position, which may be off center
    if (config.highlight) {
        int focus_left = (int) ((float) geo.screen_width * config.focus_position) - config.icon_size / 2;
        highlight->rect.x = focus_left - config.highlight_hpadding;
        highlight->rect.y = current_entry->icon_rect.y - config.highlight_vpadding;
    }
    update_column();
}

// Scroll the carousel one entry left or right
static void carousel_move(Direction direction)
{
    // Continue from the current animation offset so rapid presses stay smooth
    if (!config.wrap_entries &&
    ((direction == DIRECTION_RIGHT && current_entry->next == NULL) ||
    (direction == DIRECTION_LEFT && current_entry->previous == NULL)))
        return;

    int offset = carousel_offset();
    if (direction == DIRECTION_RIGHT) {
        current_entry = next_entry_wrapped(current_entry);
        offset += geo.x_advance;
    }
    else {
        current_entry = previous_entry_wrapped(current_entry);
        offset -= geo.x_advance;
    }
    carousel_layout();
    carousel_offset_start = offset;
    carousel_anim_start = ticks.main;
}

// Get the icon to draw for an entry: the "off" icon of a toggle entry while its setting is off
// (:togglesounds while sounds are off, :togglebackground while the Wave background is off),
// otherwise the selected icon if available
static SDL_Texture *entry_icon(Entry *entry, bool selected)
{
    if (entry->icon_off != NULL) {
        bool off = !strcmp(entry->cmd, SCMD_TOGGLE_SOUNDS) ? !config.sounds_enabled :
                   config.background_mode != BACKGROUND_WAVE;
        if (off)
            return entry->icon_off;
    }
    if (selected && entry->icon_selected != NULL)
        return entry->icon_selected;
    return entry->icon;
}

// Draw the glow behind a title; strength (0-1) scales the configured glow opacity
static void draw_title_glow(Entry *entry, const SDL_Rect *text_rect, float strength)
{
    if (entry->title_glow == NULL || strength <= 0.0f)
        return;
    int w, h;
    SDL_QueryTexture(entry->title_glow, NULL, NULL, &w, &h);
    SDL_Rect glow_rect = {
        .x = text_rect->x - entry->title_glow_padding,
        .y = text_rect->y - entry->title_glow_padding,
        .w = w,
        .h = h
    };
    render_copy_alpha(entry->title_glow, &glow_rect, (Uint8) ((float) config.title_glow_color.a * fminf(strength, 1.0f) + 0.5f));
}

// Draw a texture with a temporary opacity
static void render_copy_alpha(SDL_Texture *texture, const SDL_Rect *rect, Uint8 alpha)
{
    if (alpha == 0)
        return;
    if (alpha < 0xFF) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(texture, alpha);
    }
    SDL_RenderCopy(renderer, texture, NULL, rect);
    if (alpha < 0xFF)
        SDL_SetTextureAlphaMod(texture, 0xFF);
}

// Draw the carousel buttons, filling the screen edge to edge.
// Size, opacity and title visibility follow each entry's distance from the focus
// position, so they animate smoothly while the row scrolls.
static void draw_carousel_buttons()
{
    int offset = carousel_offset();
    float focus_center = (float) geo.screen_width * config.focus_position;
    float half_growth = (config.focus_scale - 1.0f) * (float) config.icon_size / 2.0f;
    int reach = geo.screen_width / geo.x_advance + 2;

    // Start from the leftmost entry that can be on screen
    Entry *entry = current_entry;
    int k = 0;
    while (k > -reach) {
        Entry *previous = config.wrap_entries ? previous_entry_wrapped(entry) : entry->previous;
        if (previous == NULL)
            break;
        entry = previous;
        k--;
    }

    float focused_center = focus_center;
    float focused_closeness = 0.0f;
    for (; entry != NULL && k <= reach; k++, entry = config.wrap_entries ? next_entry_wrapped(entry) : entry->next) {
        // Distance from the focus position in slots (fractional while scrolling)
        float distance = (float) (k*geo.x_advance + offset) / (float) geo.x_advance;
        float clamped_distance = fminf(fabsf(distance), 1.0f);
        float closeness = 1.0f - clamped_distance;

        // Neighbors move outward to make room for the enlarged focused icon
        float center = focus_center + (float) (k*geo.x_advance + offset) +
                       (distance < 0.0f ? -clamped_distance : clamped_distance) * half_growth;
        int size = (int) ((float) config.icon_size * (1.0f + (config.focus_scale - 1.0f) * closeness) + 0.5f);
        SDL_Rect icon_rect = {
            .x = (int) center - size / 2,
            .y = geo.y_margin + (config.icon_size - size) / 2,
            .w = size,
            .h = size
        };
        if (k == 0) {
            focused_center = center;
            focused_closeness = closeness;
        }
        if (icon_rect.x + icon_rect.w <= 0 || icon_rect.x >= geo.screen_width)
            continue;

        Uint8 alpha = (Uint8) ((float) config.unfocused_alpha + (float) (0xFF - config.unfocused_alpha) * closeness + 0.5f);
        SDL_Texture *icon = entry_icon(entry, entry == current_entry);
        render_copy_alpha(icon, &icon_rect, alpha);

        if (config.titles_enabled) {
            Uint8 title_alpha = config.titles_focused_only ? (Uint8) (255.0f * closeness + 0.5f) : alpha;
            SDL_Rect text_rect = entry->text_rect;
            text_rect.x = (int) center - text_rect.w / 2;
            text_rect.y = icon_rect.y + size + entry->title_offset + config.title_padding;
            if (entry == current_entry && !column_active())
                draw_title_glow(entry, &text_rect, closeness);
            render_copy_alpha(entry->title_texture, &text_rect, title_alpha);
        }
    }
    draw_column(focused_center, focused_closeness);
}

// Column state: the submenu of the selected entry, shown vertically below it (SubmenuMode=Column)
static Menu *column_menu = NULL;
static Entry *column_entry = NULL;
static float column_offset_start = 0.0f;
static Uint32 column_anim_start = 0;

static bool column_active()
{
    return column_menu != NULL && column_entry != NULL &&
           config.submenu_mode == SUBMENU_MODE_COLUMN && carousel_active();
}

// Set the column to the submenu of the selected entry, if it opens one
static void update_column()
{
    column_menu = NULL;
    column_entry = NULL;
    column_offset_start = 0.0f;
    if (config.submenu_mode != SUBMENU_MODE_COLUMN || !carousel_active())
        return;

    const char *cmd = current_entry->cmd;
    size_t length = strlen(SCMD_SUBMENU);
    if (strncmp(cmd, SCMD_SUBMENU, length) != 0 || cmd[length] != ' ')
        return;
    Menu *menu = get_menu(cmd + length + 1);
    if (menu == NULL || menu == current_menu || menu->num_entries == 0)
        return;
    if (menu->rendered == false)
        render_buttons(menu);

    // Each column remembers its selected entry
    column_menu = menu;
    column_entry = menu->last_selected_entry != NULL ? menu->last_selected_entry : menu->first_entry;
}

// Get the current offset of the column scroll animation, in entries (ease-out)
static float column_offset()
{
    if (column_offset_start == 0.0f || config.scroll_time == 0)
        return 0.0f;
    Uint32 elapsed = ticks.main - column_anim_start;
    if (elapsed >= config.scroll_time) {
        column_offset_start = 0.0f;
        return 0.0f;
    }
    float remaining = 1.0f - (float) elapsed / (float) config.scroll_time;
    return column_offset_start * remaining * remaining;
}

// Move the column selection up or down (the column does not wrap)
static void column_move(bool down)
{
    Entry *target = down ? column_entry->next : column_entry->previous;
    if (target == NULL)
        return;
    float offset = column_offset();
    column_entry = target;
    column_menu->last_selected_entry = target;
    column_offset_start = offset + (down ? -1.0f : 1.0f);
    column_anim_start = ticks.main;
    play_sound(SOUND_MOVE);
}

// Draw the column of the focused entry. The selected column entry sits just below
// the row; entries before it move up above the row, like the XMB.
static void draw_column(float category_center, float category_closeness)
{
    if (!column_active() || category_closeness <= 0.0f)
        return;

    int item_size = (int) ((float) config.icon_size * config.column_icon_scale + 0.5f);
    int gap = item_size / 4;
    int item_advance = item_size + gap;
    int row_middle = geo.y_margin + config.icon_size / 2;
    int category_half = (int) ((float) config.icon_size * config.focus_scale) / 2;
    float below_start = (float) (row_middle + category_half + gap +
                        (config.titles_enabled ? config.title_padding + geo.font_height : 0));
    float above_start = (float) (row_middle - category_half - gap - item_size);

    // Size of the selected column icon (automatic: half of the row's FocusScale growth)
    float focus_scale = config.column_focus_scale > 0.0f ? config.column_focus_scale :
                        1.0f + (config.focus_scale - 1.0f) * 0.5f;
    float half_growth = (focus_scale - 1.0f) * (float) item_size / 2.0f;

    // Icon centers: the enlarged selected entry sits just below the row, the entries after it
    // move down to make room for it, and the entries before it stack up above the row
    float below_center = below_start + (float) item_size / 2.0f + half_growth;
    float above_center = above_start + (float) item_size / 2.0f;

    int selected = 0;
    for (Entry *e = column_menu->first_entry; e != column_entry; e = e->next)
        selected++;
    float position = (float) selected + column_offset();

    int i = 0;
    for (Entry *entry = column_menu->first_entry; entry != NULL; entry = entry->next, i++) {
        // Position relative to the selection (fractional while scrolling)
        float f = (float) i - position;
        float center_y;
        if (f >= 0.0f)
            center_y = below_center + f * (float) item_advance + fminf(f, 1.0f) * half_growth;
        else if (f <= -1.0f)
            center_y = above_center + (f + 1.0f) * (float) item_advance;
        else
            center_y = below_center - f * (above_center - below_center);

        float closeness = 1.0f - fminf(fabsf(f), 1.0f);
        int size = (int) ((float) item_size * (1.0f + (focus_scale - 1.0f) * closeness) + 0.5f);
        if (center_y + (float) size / 2.0f < 0.0f || center_y - (float) size / 2.0f > (float) geo.screen_height)
            continue;
        SDL_Rect icon_rect = {
            .x = (int) category_center - size / 2,
            .y = (int) center_y - size / 2,
            .w = size,
            .h = size
        };
        Uint8 alpha = (Uint8) (((float) config.unfocused_alpha + (float) (0xFF - config.unfocused_alpha) * closeness) *
                               category_closeness + 0.5f);
        render_copy_alpha(entry_icon(entry, false), &icon_rect, alpha);

        // Column titles are shown to the right of the icons
        if (config.titles_enabled) {
            SDL_Rect text_rect = entry->text_rect;
            text_rect.x = icon_rect.x + size + gap;
            text_rect.y = icon_rect.y + (size - text_rect.h) / 2;
            draw_title_glow(entry, &text_rect, closeness * category_closeness);
            render_copy_alpha(entry->title_texture, &text_rect, alpha);
        }
    }
}

// Get the entry that should be executed on select
static Entry *selected_entry()
{
    return column_active() ? column_entry : current_entry;
}

// Move the selection up or down (only used by the column)
static void move_up()
{
    if (column_active())
        column_move(false);
}

static void move_down()
{
    if (column_active())
        column_move(true);
}

// A function to load a menu
static int load_menu(Menu *menu, bool set_back_menu, bool reset_position)
{
    if (menu == NULL)
        return 1;

    unsigned int buttons;
    Menu *previous_menu = current_menu;

    current_menu = menu;
    column_menu = NULL;
    log_debug("Loading menu '%s'", current_menu->name);

    // Return error if the menu doesn't contain entires
    if (current_menu->num_entries == 0) {
        log_error("No valid entries found for Menu '%s'", current_menu->name);
        current_menu = previous_menu;
        return 1;
    }

    // Render the menu if not already rendered
    if (current_menu->rendered == false)
        render_buttons(current_menu);

    // Set menu properties
    if (set_back_menu)
        current_menu->back = previous_menu;

    if (reset_position) {
        current_entry = current_menu->first_entry;
        current_menu->root_entry = current_entry;
        current_menu->highlight_position = 0;
        current_menu->page = 0;
    }
    else
        current_entry = current_menu->last_selected_entry;

    if (carousel_active()) {
        carousel_offset_start = 0;
        carousel_layout();
        return 0;
    }

    buttons = current_menu->num_entries - (current_menu->page)*config.max_buttons;
    if (buttons > config.max_buttons)
        buttons = config.max_buttons;
    
    // Recalculate the screen geometry
    calculate_button_geometry(current_menu->root_entry, (int) buttons);
    if (config.highlight) {
        highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        highlight->rect.y = current_entry->icon_rect.y - config.highlight_vpadding;
    }
    return 0;
}

// A function to load a menu by its name
static int load_menu_by_name(const char *menu_name, bool set_back_menu, bool reset_position)
{
    Menu *menu = get_menu(menu_name);
    return load_menu(menu, set_back_menu, reset_position);
}

// A function to calculate the layout of the buttons
static void calculate_button_geometry(Entry *entry, int buttons)
{
    // Calculate proper spacing
    geo.x_margin = (geo.screen_width - config.icon_size*buttons -
                   buttons*config.icon_spacing + config.icon_spacing) / 2;
    geo.x_advance = config.icon_size + config.icon_spacing;
    geo.num_buttons = buttons;

    // Assign values to entries
    for (int i = 0; i < geo.num_buttons; i++) {
            entry->icon_rect.x = geo.x_margin + i*geo.x_advance;
            entry->icon_rect.y = geo.y_margin;
            entry->icon_rect.w = config.icon_size;
            entry->icon_rect.h = config.icon_size;
            entry->text_rect.x = entry->icon_rect.x +
                                 (entry->icon_rect.w - entry->text_rect.w) / 2;
            entry->text_rect.y = entry->icon_rect.y + config.icon_size + entry->title_offset + 
                                 config.title_padding;
            entry = next_entry_wrapped(entry);
    }
}

// A function to render all buttons (icon and text) for a menu
static void render_buttons(Menu *menu)
{
    Entry *entry;
    int h;
    for (entry = menu->first_entry; entry != NULL; entry = entry->next) {
        entry->icon = load_texture_from_file(entry->icon_path);
        entry->icon_selected = (entry->icon_selected_path != NULL) ? load_texture_from_file(entry->icon_selected_path) : NULL;
        entry->icon_off = NULL;
        if (!strcmp(entry->cmd, SCMD_TOGGLE_SOUNDS) || !strcmp(entry->cmd, SCMD_TOGGLE_BACKGROUND)) {
            char *off_path = suffixed_path(entry->icon_path, OFF_SUFFIX);
            if (off_path != NULL) {
                entry->icon_off = load_texture_from_file(off_path);
                free(off_path);
            }
        }
        if (config.titles_enabled) {
            SDL_Surface *title_surface = render_text(entry->title, &title_info, &entry->text_rect, &h);
            entry->title_glow = NULL;
            if (config.title_glow && title_surface != NULL)
                entry->title_glow = render_glow_texture(title_surface, config.title_glow_color, &entry->title_glow_padding);
            entry->title_texture = load_texture(title_surface);
            if (config.title_oversize_mode == OVERSIZE_SHRINK && h != geo.font_height)
                entry->title_offset = (geo.font_height - h) / 2;
        }
    }
    menu->rendered = true;
}

// A function to move the selection left when clicked by user
static void move_left_entry()
{
    if (carousel_active()) {
        carousel_move(DIRECTION_LEFT);
        return;
    }

    // If we are not in leftmost position, move highlight left
    if (current_menu->highlight_position > 0) {
        if (config.highlight)
            highlight->rect.x -= geo.x_advance;
        current_menu->highlight_position--;
        current_entry = current_entry->previous;
    }

    // If we are in leftmost position...
    else if (current_menu->highlight_position == 0 && (current_menu->page > 0 || config.wrap_entries)) {
        unsigned int buttons;
        current_entry = current_entry->previous;

        // Load the previous page if there is a valid previous entry
        if (current_entry) {
            buttons = config.max_buttons;
            current_menu->root_entry = advance_entries(current_menu->root_entry, (int) buttons, DIRECTION_LEFT);
            current_menu->page--;
        }

        // If the user has the wrap entries setting, select the last entry in the menu
        else {
            current_entry = advance_entries(current_menu->first_entry, (int) current_menu->num_entries - 1, DIRECTION_RIGHT);
            unsigned int num_pages = DIV_ROUND_UP(current_menu->num_entries, config.max_buttons);
            current_menu->root_entry = advance_entries(current_menu->root_entry,
                (int) ((num_pages - 1 - current_menu->page) * config.max_buttons),
                DIRECTION_RIGHT
            );
            current_menu->page = num_pages - 1;
            buttons = current_menu->num_entries - current_menu->page * config.max_buttons;
        }

        calculate_button_geometry(current_menu->root_entry, (int) buttons);
        if (config.highlight)
            highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        current_menu->highlight_position = buttons - 1;
    }
}

// A function to move the selection right when clicked by the user
static void move_right_entry()
{
    if (carousel_active()) {
        carousel_move(DIRECTION_RIGHT);
        return;
    }

    // If we are not in the rightmost position, move highlight right
    if ((int) current_menu->highlight_position < (geo.num_buttons - 1)) {
        if (config.highlight)
            highlight->rect.x += geo.x_advance;
        current_menu->highlight_position++;
        current_entry = current_entry->next;
    }

    // If we are in the rightmost postion, but there are more entries in the menu, load next page
    else if (current_menu->highlight_position + current_menu->page*config.max_buttons <
    (current_menu->num_entries - 1)) {
        unsigned int buttons = current_menu->num_entries - (current_menu->page + 1)*config.max_buttons;
        if (buttons > config.max_buttons)
            buttons = config.max_buttons;
        current_entry = current_entry->next;
        current_menu->root_entry = current_entry;
        calculate_button_geometry(current_menu->root_entry, (int) buttons);
        if (config.highlight)
            highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        current_menu->page++;
        current_menu->highlight_position = 0;
    }

    // If user has the wrap entries setting, reset menu to first entry
    else if (config.wrap_entries) {
        current_entry = current_menu->first_entry;
        current_menu->root_entry = current_entry;
        current_menu->highlight_position = 0;
        current_menu->page = 0;
        if (config.highlight)
            highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        calculate_button_geometry(current_menu->root_entry, (int) MIN(current_menu->num_entries, config.max_buttons));
    }
}

// Move the selection left or right, with a sound if it moved
static void move_left()
{
    Entry *previous = current_entry;
    move_left_entry();
    if (current_entry != previous)
        play_sound(SOUND_MOVE);
}

static void move_right()
{
    Entry *previous = current_entry;
    move_right_entry();
    if (current_entry != previous)
        play_sound(SOUND_MOVE);
}

// A function to load a submenu
static void load_submenu(const char *submenu)
{
    current_menu->last_selected_entry = current_entry;
    load_menu_by_name(submenu, true, true);
}

// A function to load the previous menu
static void load_back_menu(Menu *menu)
{
    if (menu->back != NULL)
        play_sound(SOUND_BACK);
    load_menu(menu->back, false, config.reset_on_back);
}

// Fade-in state (after returning from an application, and at startup)
static bool fade_in_active = false;
static Uint32 fade_in_start = 0;

// A function to start fading the screen in from black
static void start_fade_in()
{
    if (config.fade_time > 0) {
        fade_in_active = true;
        fade_in_start = ticks.main;
    }
}

// A function to draw a black overlay over the whole screen
static void draw_black_overlay(Uint8 alpha)
{
    Uint8 r, g, b, a;
    SDL_BlendMode mode;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_GetRenderDrawBlendMode(renderer, &mode);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
    SDL_RenderFillRect(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, mode);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
}

// A function to update the screen with all visible textures
static void draw_screen()
{
    // When launching an application with OnLaunch=Blank, fade to black instead of blanking at once
    Uint32 launch_elapsed = ticks.main - ticks.application_launched;
    bool launch_fading = state.application_launching && config.on_launch == ON_LAUNCH_BLANK &&
                         config.fade_time > 0 && launch_elapsed < config.fade_time;

    // Draw background
    SDL_RenderClear(renderer);
    if (!(state.application_launching && config.on_launch == ON_LAUNCH_BLANK) || launch_fading) {
        if (config.background_mode == BACKGROUND_WAVE)
            draw_wave_background(ticks.main);
        else if ((config.background_mode == BACKGROUND_IMAGE || config.background_mode == BACKGROUND_SLIDESHOW) &&
        background_texture != NULL)
            SDL_RenderCopy(renderer, background_texture, NULL, NULL);
        if (config.background_mode == BACKGROUND_IMAGE)
            draw_web_background_transition();

        if (config.background_mode == BACKGROUND_SLIDESHOW && state.slideshow_transition)
            SDL_RenderCopy(renderer, slideshow->transition_texture, NULL, NULL);

        // Draw background overlay
        if (config.background_overlay)
            SDL_RenderCopy(renderer, background_overlay, NULL, NULL);

        // Draw scroll indicators (not in carousel mode: the row fills the screen edge to edge)
        if (config.scroll_indicators && !carousel_active() &&
        (current_menu->page*config.max_buttons + (unsigned int) geo.num_buttons) <= (current_menu->num_entries - 1))
            SDL_RenderCopy(renderer, scroll->texture, NULL, &scroll->rect_right);

        if (config.scroll_indicators && !carousel_active() && current_menu->page > 0)
            SDL_RenderCopyEx(renderer, scroll->texture, NULL, &scroll->rect_left, 0, NULL, SDL_FLIP_HORIZONTAL);

        // Draw clock
        if (config.clock_enabled) {
            SDL_RenderCopy(renderer, clk->time_texture, NULL, &clk->time_rect);
            if (config.clock_show_date)
                SDL_RenderCopy(renderer, clk->date_texture, NULL, &clk->date_rect);
        }

        // Draw highlight
        if (config.highlight)
            SDL_RenderCopy(renderer,
                highlight->texture,
                NULL,
                &highlight->rect
            );

        // Draw buttons
        if (carousel_active())
            draw_carousel_buttons();
        Entry *entry = current_menu->root_entry;
        SDL_Texture *icon;
        for (int i = 0; !carousel_active() && i < geo.num_buttons; i++) {
            icon = entry_icon(entry, i == (int) current_menu->highlight_position);
            SDL_RenderCopy(renderer, icon, NULL, &entry->icon_rect);
            if (config.titles_enabled && (!config.titles_focused_only || i == (int) current_menu->highlight_position)) {
                if (i == (int) current_menu->highlight_position)
                    draw_title_glow(entry, &entry->text_rect, 1.0f);
                SDL_RenderCopy(renderer, entry->title_texture, NULL, &entry->text_rect);
            }
            entry = entry-> next;
        }

        // Draw the layout popup over the menu
        draw_layout_popup();

        // Draw screensaver
        if (state.screensaver_active)
            SDL_RenderCopy(renderer, screensaver->texture, NULL, NULL);

        // Fade out when launching, fade in after returning
        if (launch_fading)
            draw_black_overlay((Uint8) (255 * launch_elapsed / config.fade_time));
        else if (fade_in_active) {
            Uint32 elapsed = ticks.main - fade_in_start;
            if (elapsed >= config.fade_time)
                fade_in_active = false;
            else
                draw_black_overlay((Uint8) (255 - 255 * elapsed / config.fade_time));
        }
    }
    else
        SDL_RenderFillRect(renderer, NULL);

    // Output to screen
    SDL_RenderPresent(renderer);
    if (!config.vsync) {
        Uint32 elapsed = SDL_GetTicks() - ticks.main;
        if (elapsed < refresh_period)
            SDL_Delay(refresh_period - elapsed);
    }
}

// A function to execute the user's command
static void execute_command(const char *command)
{
    // Copy command into separate buffer
    char *cmd = strdup(command);

    // The layout popup takes all input while it is open (e.g. from the gamepad)
    if (layout_popup_active()) {
        layout_popup_command(strtok(cmd, " "));
        free(cmd);
        return;
    }

    // Parse special commands
    if (cmd[0] == ':') {
        char *delimiter = " ";
        char *special_command = strtok(cmd, delimiter);
        if (!strcmp(special_command, SCMD_SUBMENU)) {
            char *submenu = strtok(NULL, "");
            if (submenu != NULL)
                load_submenu(submenu);
        }
        else if (!strcmp(special_command, SCMD_FORK)) {
            char *fork_command = strtok(NULL, "");
            if (fork_command != NULL)
                start_process(fork_command, false);
        }
        else if (!strcmp(special_command, SCMD_LEFT))
            move_left();
        else if (!strcmp(special_command, SCMD_RIGHT))
            move_right();
        else if (!strcmp(special_command, SCMD_UP))
            move_up();
        else if (!strcmp(special_command, SCMD_DOWN))
            move_down();
        else if (!strcmp(special_command, SCMD_SELECT)) {
            play_sound(SOUND_SELECT);
            execute_command(selected_entry()->cmd);
        }
        else if (!strcmp(special_command, SCMD_TOGGLE_SOUNDS))
            toggle_sounds();
        else if (!strcmp(special_command, SCMD_TOGGLE_BACKGROUND))
            toggle_background();
        else if (!strcmp(special_command, SCMD_WALLPAPER)) {
            // Optional keywords, then show the picture background with a new image
            new_web_background(strtok(NULL, ""));
            if (config.background_mode == BACKGROUND_WAVE && is_web_image(config.background_image))
                toggle_background();
        }
        else if (!strcmp(special_command, SCMD_LAYOUTS))
            open_layout_popup();
        else if (!strcmp(special_command, SCMD_HOME))
            load_menu(default_menu, false, true);
        else if (!strcmp(special_command, SCMD_BACK))
            load_back_menu(current_menu);
        else if (!strcmp(special_command, SCMD_QUIT))
            quit(EXIT_SUCCESS);
        else if (!strcmp(special_command, SCMD_SHUTDOWN))
            scmd_shutdown();
        else if (!strcmp(special_command, SCMD_RESTART))
            scmd_restart();
        else if (!strcmp(special_command, SCMD_SLEEP))
            scmd_sleep();
    }

    // Launch external application
    else {
        SDL_Delay(50);
        if (start_process(cmd, true)) {
            state.application_launching = true;
            ticks.application_launched = ticks.main;
            if (config.on_launch == ON_LAUNCH_BLANK)
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
            else if (config.on_launch == ON_LAUNCH_QUIT)
                quit(EXIT_SUCCESS);
        }
    }
    free(cmd);
}

// A function to initialize the gamepad struct
static void init_gamepad(Gamepad **gamepad, int device_index)
{
    *gamepad = malloc(sizeof(Gamepad));
    **gamepad = (Gamepad) {
        .device_index = device_index,
        .id = (int) SDL_JoystickGetDeviceInstanceID(device_index),
        .controller = NULL,
        .next = NULL,
        .previous = NULL,
    };
}

// A function to open the SDL controller
static void open_controller(Gamepad *gamepad, bool raise_error)
{
    gamepad->controller = SDL_GameControllerOpen(gamepad->device_index);
    if (gamepad->controller == NULL) {
        if (raise_error)
            log_error("Could not open gamepad at device index %i", config.gamepad_device);
        return;
    }
    if (config.debug && raise_error) {
        char *mapping = SDL_GameControllerMapping(gamepad->controller);
        log_debug("Gamepad Mapping:\n%s", mapping);
        SDL_free(mapping);
    }
}

// A function to connect gamepad(s)
static void connect_gamepad(int device_index, bool open, bool raise_error)
{
    if (device_index >= 0) {
        Gamepad *gamepad = NULL;
        for (gamepad = gamepads; gamepad != NULL; gamepad = gamepad->next) {
            if (gamepad->id == device_index)
                break;
        }
        if (gamepad == NULL) {
            init_gamepad(&gamepad, device_index);
            if (gamepads == NULL)
                gamepads = gamepad;

            // Add to end of the linked list
            else {
                Gamepad *i;
                for (i = gamepads; i->next != NULL; i = i->next);
                i->next = gamepad;
                gamepad->previous = i;
            }
        }
        if (open)
            open_controller(gamepad, raise_error);
    }
    else if (open) {
        for (Gamepad *i = gamepads; i != NULL; i = i->next)
            open_controller(i, raise_error);
    }
}

// A function to disconnect gamepad(s)
static void disconnect_gamepad(int id, bool disconnect, bool remove)
{
    for (Gamepad *i = gamepads; i != NULL;) {
        if (id < 0 || i->id == id) {
            if (disconnect)
                SDL_GameControllerClose(i->controller);
            if (remove) {
                if (i->next != NULL)
                    i->next->previous = i->previous;
                if (i->previous != NULL)
                    i->previous->next = i->next;
                if (i == gamepads)
                    gamepads = i->next;
                Gamepad *tmp = i->next;
                free(i);
                i = tmp;
            }
            else {
                i->controller = NULL;
                i = i->next;
            }
        }
        else
            i = i->next;
    }
}

// A function to poll the connected gamepad for commands
static void poll_gamepad()
{
    int value_multiplier; // Handles positive or negative axis
    bool pressed;
    for (GamepadControl *i = gamepad_controls; i != NULL; i = i->next) {
        pressed = false;
        for (Gamepad *gamepad = gamepads; gamepad != NULL; gamepad = gamepad->next) {

            // Check if axis value exceeds dead zone
            if (i->type == TYPE_AXIS_POS || i->type == TYPE_AXIS_NEG) {
                if (i->type == TYPE_AXIS_POS)
                    value_multiplier = 1;
                else if (i->type == TYPE_AXIS_NEG)
                    value_multiplier = -1;
                if (value_multiplier*SDL_GameControllerGetAxis(gamepad->controller, i->index) > GAMEPAD_DEADZONE) {
                    i->repeat++;
                    pressed = true;
                    break;
                }
            }

            // Check buttons
            else if (i->type == TYPE_BUTTON) {
                if (SDL_GameControllerGetButton(gamepad->controller, i->index)) {
                    i->repeat++;
                    pressed = true;
                    break;
                }
            }
        }
        if (!pressed) {
            i->repeat = 0;
            continue;
        }

        // Execute command if first press or valid repeat
        if (i->repeat == 1) {
            log_debug("Gamepad %s detected", i->label);
            ticks.last_input = ticks.main;
            execute_command(i->cmd);
        }
        else if (i->repeat == delay_period) {
            ticks.last_input = ticks.main;
            execute_command(i->cmd);
            i->repeat -= repeat_period;
        }
    }
}

// A function to update the slideshow
static void update_slideshow()
{
    // If image duration time has elapsed, load the next image and start the transition
    if (!state.slideshow_transition && (ticks.main - ticks.slideshow_load > config.slideshow_image_duration) &&
    !state.slideshow_paused) {
        
        // Render the new background image in a separate thread so we don't block the main thread
        if (!state.slideshow_background_rendering && !state.slideshow_background_ready) {
            Slideshowhread = SDL_CreateThread(load_next_slideshow_background_async, "Slideshow Thread", (void*) slideshow);
            state.slideshow_background_rendering = true;
        }

        // Convert background to texture after the rendering thread has completed
        else if (state.slideshow_background_ready) {
            SDL_WaitThread(Slideshowhread, NULL);
            Slideshowhread = NULL;
            if (config.slideshow_transition_time > 0) {
                slideshow->transition_texture = load_texture(slideshow->transition_surface);
                SDL_SetTextureAlphaMod(slideshow->transition_texture, 0);
                state.slideshow_transition = true;
            }
            else {
                SDL_DestroyTexture(background_texture);
                background_texture = load_texture(slideshow->transition_surface);
                ticks.slideshow_load = ticks.main;
            }
        slideshow->transition_surface = NULL;
        state.slideshow_background_ready = false;
        }
    }
    else if (state.slideshow_transition) {
        
        // Increase the transparency
        slideshow->transition_alpha += slideshow->transition_change_rate;
        
        // If transition is done, destroy old background and replace it with the new one
        if (slideshow->transition_alpha >= 255.0f) {
            SDL_SetTextureAlphaMod(slideshow->transition_texture, 0xFF);
            slideshow->transition_alpha = 0.0f;
            SDL_DestroyTexture(background_texture);
            background_texture = slideshow->transition_texture;
            slideshow->transition_texture = NULL;
            state.slideshow_transition = false;
            ticks.slideshow_load = ticks.main;
        }
        else
            SDL_SetTextureAlphaMod(slideshow->transition_texture, (Uint8) slideshow->transition_alpha);
    }
}

// A function to update the screensaver
static void update_screensaver()
{
    // Activate the screensaver if the launcher has been idle for the required time
    if (!state.screensaver_active && ticks.main - ticks.last_input > config.screensaver_idle_time) {
        state.screensaver_active = true;
        state.screensaver_transition = true;
        if (config.background_mode == BACKGROUND_SLIDESHOW && config.screensaver_pause_slideshow)
            state.slideshow_paused = true;
    }
    else {

        // Transition the screen to dark
        if (state.screensaver_transition) {
            screensaver->alpha += screensaver->transition_change_rate;
            if (screensaver->alpha >= screensaver->alpha_end_value) {
                SDL_SetTextureAlphaMod(screensaver->texture, (Uint8) screensaver->alpha_end_value);
                state.screensaver_transition = false;
            }
            else
                SDL_SetTextureAlphaMod(screensaver->texture, (Uint8) screensaver->alpha);
        }

        // User has pressed input, deactivate the screensaver
        if (state.screensaver_active && ticks.last_input == ticks.main) {
            SDL_SetTextureAlphaMod(screensaver->texture, 0);
            screensaver->alpha = 0.0f;
            state.screensaver_active = false;
            state.screensaver_transition = false;
            if (config.background_mode == BACKGROUND_SLIDESHOW) {
                state.slideshow_paused = false;
                
                // Reset the slideshow time so we don't have a transition immediately 
                // after coming out of screensaver mode
                ticks.slideshow_load = ticks.main;
            }
        }
    }
}

// A function to update the clock display
static void update_clock(bool block)
{
    if (ticks.main - ticks.clock_update > CLOCK_UPDATE_PERIOD) {
        if (!state.clock_rendering) {

            // Check to see if the time has changed
            get_time(clk);
            if (clk->render_time) {
                state.clock_rendering = true;
                if (block)
                    render_clock(clk);
                else
                    clock_thread = SDL_CreateThread(render_clock_async, "Clock Thread", (void*) clk); 
            }
            else
                ticks.clock_update = ticks.main;
        }

        // Render texture
        if (state.clock_ready) {
            SDL_WaitThread(clock_thread, NULL);
            clock_thread = NULL;
            SDL_DestroyTexture(clk->time_texture);
            clk->time_texture = load_texture(clk->time_surface);
            clk->time_surface = NULL;
            if (clk->render_date) {
                SDL_DestroyTexture(clk->date_texture);
                clk->date_texture = load_texture(clk->date_surface);
                clk->date_surface = NULL;
            }
            ticks.clock_update = ticks.main;
            clk->render_time = false;
            clk->render_date = false;
            state.clock_rendering = false;
            state.clock_ready = false;
        }
    }
}

static inline void pre_launch()
{
    pause_sounds(true);
    if (gamepads != NULL)
        disconnect_gamepad(-1, true, false);

// Initialize exit hotkey for Windows
#ifdef _WIN32
    if (has_exit_hotkey())
        SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
}

static inline void post_launch()
{
    // Rebaseline the timing after the program is done
    ticks.main = SDL_GetTicks();
    ticks.last_input = ticks.main;

    // Post-application updates
    if (config.gamepad_enabled)
        connect_gamepad(-1, true, false);
    if (config.clock_enabled)
        update_clock(true);
    if (config.background_mode == BACKGROUND_SLIDESHOW)
        resume_slideshow();
    if (config.on_launch == ON_LAUNCH_BLANK)
        set_draw_color();
    start_fade_in();
    pause_sounds(false);

#ifdef _WIN32
    SDL_EventState(SDL_SYSWMEVENT, SDL_DISABLE);
    if (config.background_mode == BACKGROUND_TRANSPARENT)
        hide_cursor(current_entry);
#endif
}

// A function to quit the launcher
void quit(int status)
{
    log_debug("Quitting program");
    if (status != EXIT_SUCCESS)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, 
            PROJECT_NAME, 
            "A critical error occurred. Check the log file for details.", 
            NULL
        );
    if (config.quit_cmd != NULL) {
        execute_command(config.quit_cmd);
        free(config.quit_cmd);
    }
    cleanup();
    exit(status);
}

// A function to print the version and other info to command line
void print_version(FILE *stream)
{
    SDL_version sdl_version;
    SDL_GetVersion(&sdl_version);
    const SDL_version *img_version = IMG_Linked_Version();
    const SDL_version *ttf_version = TTF_Linked_Version();
    fprintf(stream, PROJECT_NAME " version " PROJECT_VERSION ", using:" endline);
    fprintf(stream, "  SDL       %u.%u.%u" endline, sdl_version.major, sdl_version.minor, sdl_version.patch);
    fprintf(stream, "  SDL_image %u.%u.%u" endline, img_version->major, img_version->minor, img_version->patch);
    fprintf(stream, "  SDL_ttf   %u.%u.%u" endline, ttf_version->major, ttf_version->minor, ttf_version->patch);
}

// The picture background (Image or Slideshow) that :togglebackground switches to from Wave
static ModeBackground picture_mode = BACKGROUND_COLOR;
static bool picture_loaded = false;

// A function to load the Image or Slideshow background
static void load_picture_background()
{
    picture_loaded = true;
    if (config.background_mode == BACKGROUND_IMAGE) {
        if (config.background_image == NULL)
            log_error("Background 'Image' setting not specified in config file");
        else if (is_web_image(config.background_image))
            init_web_background();
        else {
            SDL_Surface *surface = IMG_Load(config.background_image);
            if (surface == NULL)
                log_error("Could not load image %s\n%s", config.background_image, IMG_GetError());
            background_texture = load_texture(apply_background_filters(surface));
        }

        // Switch to color mode if loading background image failed (a URL image shows the
        // background color until its first download finishes)
        if (background_texture == NULL && !is_web_image(config.background_image)) {
            config.background_mode = BACKGROUND_COLOR;
            log_error("Couldn't load background image, defaulting to color background");
            set_draw_color();
        }
    }

    // Render first slideshow image
    else if (config.background_mode == BACKGROUND_SLIDESHOW) {
        if (slideshow == NULL)
            init_slideshow();
        if (config.background_mode == BACKGROUND_SLIDESHOW) {
            SDL_Surface *surface = load_next_slideshow_background(slideshow, false);
            background_texture = load_texture(surface);
            ticks.slideshow_load = ticks.main;
        }
    }
}

// A function to switch between the Wave background and the picture background,
// and save the choice to the config file
static void toggle_background()
{
    if (config.background_mode == BACKGROUND_WAVE) {
        if (picture_mode == BACKGROUND_COLOR) {
            log_error("No background Image or SlideshowDirectory set to switch to");
            return;
        }
        config.background_mode = picture_mode;
        if (!picture_loaded)
            load_picture_background();
    }
    else if (config.background_mode == BACKGROUND_IMAGE || config.background_mode == BACKGROUND_SLIDESHOW) {
        if (!wave_background_supported()) {
            log_error("The Wave background needs SDL 2.0.18 or newer");
            return;
        }
        config.background_mode = BACKGROUND_WAVE;
    }
    else
        return;
    set_draw_color();
    start_fade_in();
    log_debug("Background mode: %s", get_mode_setting(MODE_SETTING_BACKGROUND, config.background_mode));

    if (config.config_file_path != NULL &&
    !save_config_setting(config.config_file_path, "Background", SETTING_BACKGROUND_MODE,
        get_mode_setting(MODE_SETTING_BACKGROUND, config.background_mode)))
        log_error("Could not save the background setting to the config file");
}

int main(int argc, char *argv[]) 
{
    int error;
    char *config_file_path = NULL;
    config.exe_path = SDL_GetBasePath();

    // Handle command line arguments, find config file
    handle_arguments(argc, argv, &config_file_path);

    // Parse config file for settings and menu entries
    parse_config_file(config_file_path);
    config.config_file_path = config_file_path;

    // Get default menu
    if (config.default_menu == NULL)
        log_fatal("No default menu defined in config file");
    default_menu = get_menu(config.default_menu);
    if (default_menu == NULL)
        log_fatal("Default menu %s not found in config file", config.default_menu);

    // Initialize SDL, verify all settings are in their allowable range
    init_sdl();
    init_sdl_image();
    init_sdl_ttf();
    validate_settings(&geo);
    
    // Remember the picture background that :togglebackground switches to from the Wave background
    if (config.background_mode == BACKGROUND_IMAGE || config.background_mode == BACKGROUND_SLIDESHOW)
        picture_mode = config.background_mode;
    else if (config.background_image != NULL)
        picture_mode = BACKGROUND_IMAGE;
    else if (config.slideshow_directory != NULL)
        picture_mode = BACKGROUND_SLIDESHOW;

    // Initialize slideshow
    if (config.background_mode == BACKGROUND_SLIDESHOW)
        init_slideshow();

    // Initialize Nanosvg, create window and renderer
    init_svg();
    init_sounds();
    if (config.background_mode == BACKGROUND_WAVE && !wave_background_supported()) {
        log_error("The Wave background needs SDL 2.0.18 or newer, using a color background");
        config.background_mode = BACKGROUND_COLOR;
        set_draw_color();
    }
    create_window();

    // Initialize timing
    ticks.main = SDL_GetTicks();
    ticks.last_input = ticks.main;
    ticks.program_start = ticks.main;
    srand((unsigned int) time(NULL));
    start_fade_in();

    // Load gamepad overrides
    if (config.gamepad_enabled && config.gamepad_mappings_file != NULL) {
        error = SDL_GameControllerAddMappingsFromFile(config.gamepad_mappings_file);
        if (error < 0) {
            log_error("Could not load gamepad mappings from %s\n%s", 
                config.gamepad_mappings_file,
                SDL_GetError()
            );
        }
    }

    // Render background
    if (config.background_mode == BACKGROUND_IMAGE || config.background_mode == BACKGROUND_SLIDESHOW)
        load_picture_background();

    // Initialize screensaver
    if (config.screensaver_enabled)
        init_screensaver();

    // Initialize clock
    if (config.clock_enabled) {
        clk = malloc(sizeof(Clock));
        init_clock(clk);
        ticks.clock_update = ticks.main;
    }
    
    // Render highlight
    if (config.highlight) {
        int button_height = config.icon_size + config.title_padding + geo.font_height;
        highlight = malloc(sizeof(Highlight));
        highlight->texture = render_highlight(config.icon_size + 2*config.highlight_hpadding,
                                button_height + 2*config.highlight_vpadding,
                                &highlight->rect
                            );
    }

    // Render scroll indicators
    if (config.scroll_indicators) {
        scroll = malloc(sizeof(Scroll));
        scroll->texture = NULL;
        int scroll_indicator_height = (int) ((float) geo.screen_height * SCROLL_INDICATOR_HEIGHT);
        render_scroll_indicators(scroll, scroll_indicator_height, &geo);
    }

    // Render background overlay
    if (config.background_overlay) {
        SDL_Surface *overlay_surface = NULL;
        overlay_surface = SDL_CreateRGBSurfaceWithFormat(0, 
                              geo.screen_width, 
                              geo.screen_height, 
                              32,
                              SDL_PIXELFORMAT_ARGB8888
                          );
        Uint32 overlay_color = SDL_MapRGBA(overlay_surface->format, 
                                   config.background_overlay_color.r, 
                                   config.background_overlay_color.g, 
                                   config.background_overlay_color.b, 
                                   config.background_overlay_color.a
                               );
        SDL_FillRect(overlay_surface, NULL, overlay_color);
        background_overlay = load_texture(overlay_surface);
    }

    // Register exit hotkey with Windows
#ifdef _WIN32
    if (has_exit_hotkey())
        register_exit_hotkey();
#endif

    // Print debug info to log
    if (config.debug) {
        debug_video(renderer, &display_mode);
        debug_settings();
        debug_gamepad(gamepad_controls);
        debug_hotkeys(hotkeys);    
        debug_menu_entries(config.first_menu, config.num_menus);
    }

    // Load the default menu and display it
    error = load_menu(default_menu, false, true);
    if (error)
        log_fatal("Could not load default menu %s", config.default_menu);

    // Execute startup command
    if (config.startup_cmd != NULL)
        execute_command(config.startup_cmd);
    
    // Main program loop
    log_debug("Begin program loop");
    while (1) {
        ticks.main = SDL_GetTicks();
        while (SDL_PollEvent(&event)) {
            switch(event.type) {
                case SDL_QUIT:
                    quit(EXIT_SUCCESS);
                    break;

                case SDL_KEYDOWN:
                    ticks.last_input = ticks.main;
                    handle_keypress(&event.key.keysym);
                    break;
                
                case SDL_MOUSEBUTTONDOWN:
                    if (config.mouse_select && event.button.button == SDL_BUTTON_LEFT) {
                        ticks.last_input = ticks.main;
                        if (layout_popup_active())
                            layout_popup_command(SCMD_SELECT);
                        else {
                            play_sound(SOUND_SELECT);
                            execute_command(selected_entry()->cmd);
                        }
                    }
                    break;

                case SDL_JOYDEVICEADDED:
                    if (SDL_IsGameController(event.jdevice.which) == SDL_TRUE) {
                        log_debug("Gamepad connected with device index %i", event.jdevice.which);
                        if (config.gamepad_device < 0 || config.gamepad_device == event.jdevice.which)
                            connect_gamepad(event.jdevice.which, !state.application_running, true);
                    }
                    break;

                case SDL_JOYDEVICEREMOVED:
                    if (SDL_IsGameController(event.jdevice.which) == SDL_TRUE || 1) {
                        log_debug("Gamepad disconnected");
                        if (config.gamepad_device < 0 || config.gamepad_device == event.jdevice.which)
                            disconnect_gamepad(event.jdevice.which, true, true);
                    }
                    break;

                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        log_debug("Lost keyboard focus");
                        state.has_focus = false;
                        if (state.application_launching) {
                            log_debug("Application detected");
                            state.application_launching = false;
                            state.application_running = true;
                            pre_launch();
                        }
#ifdef _WIN32
                        // Sometimes the launcher will lose focus on Windows when autostarting
                        // So if we lose the window focus within 10 seconds of the launcher starting
                        // we will grab back th Window focus. This is a bit of a hack
                        else if (ticks.main - ticks.program_start < 10000)
                            set_foreground_window();
#endif
                    }
                    else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        log_debug("Gained keyboard focus");
                        state.has_focus = true;
                    }
                    else if (event.window.event == SDL_WINDOWEVENT_LEAVE)
                        log_debug("Lost mouse focus");
                    break;
#ifdef _WIN32
                case SDL_SYSWMEVENT:
                    check_exit_hotkey(event.syswm.msg);
                    break;
#endif
            }
        }

        // Update application state
        if (state.application_running && state.has_focus) {
            state.application_running = false;
            post_launch();
            log_debug("Application finished");
        }

        // Post-event loop updates
        if (!(state.application_running || state.application_launching)) {
            if (gamepads != NULL)
                poll_gamepad();
            if (config.background_mode == BACKGROUND_SLIDESHOW)
                update_slideshow();
            else if (config.background_mode == BACKGROUND_IMAGE)
                update_web_background();
            if (config.screensaver_enabled)
                update_screensaver();
            if (config.clock_enabled)
                update_clock(false);
        }
        if (state.application_launching &&
        ticks.main - ticks.application_launched > config.application_timeout) {
            state.application_launching = false;
            if (config.on_launch == ON_LAUNCH_BLANK)
                set_draw_color();
            start_fade_in();
        }
        if (state.application_running)
            SDL_Delay(APPLICATION_WAIT_PERIOD);
        else
            draw_screen();
    }
    quit(EXIT_SUCCESS);
}
