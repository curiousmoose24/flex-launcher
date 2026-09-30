#pragma once

bool is_web_image(const char *path);
void init_web_background(void);
void update_web_background(void);
void draw_web_background_transition(void);
void new_web_background(const char *keywords);
bool has_keyword(const char *keyword);
bool toggle_keyword(const char *keyword);
void quit_web_background(void);
