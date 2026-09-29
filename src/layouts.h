#pragma once

int find_layout(const char *id);
const char *layout_id(int layout);
bool layout_popup_active(void);
void open_layout_popup(void);
void layout_popup_command(const char *command);
void draw_layout_popup(void);
void quit_layout_popup(void);
