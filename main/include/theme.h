#ifndef THEME_H
#define THEME_H

#include "lvgl.h"

// Base structure defining the color palette for the UI
typedef struct {
    // Screen and surfaces
    lv_color_t bg_screen;
    lv_color_t bg_card;
    lv_color_t bg_btn;
    lv_color_t bg_btn_subtle;
    lv_color_t bg_item;
    lv_color_t bg_overlay;

    // Borders and dividers
    lv_color_t border;
    lv_color_t border_subtle;
    lv_color_t divider;

    // Typography and text
    lv_color_t text_title;
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t text_muted;
    lv_color_t text_dim;
    lv_color_t text_inverse;

    // Accents and status indicators
    lv_color_t accent_primary;
    lv_color_t accent_secondary;
    lv_color_t status_playing;
    lv_color_t status_paused;
    lv_color_t danger;
} app_theme_t;

// Global theme instance
extern app_theme_t theme;

// Initialize the default theme colors
void theme_init_base(void);

#endif // THEME_H

