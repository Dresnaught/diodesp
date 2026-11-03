#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "class/hid/hid_device.h"
#include "class/audio/audio_device.h"
#include "lvgl.h"
#include "include/lovyan_config.h"

// Hardware navigation buttons
#define PSPL GPIO_NUM_4  // OK / Play-Pause
#define NXT  GPIO_NUM_5  // Next / Navigate forward
#define PVS  GPIO_NUM_8  // Prev / Navigate backward

#define CDC_BUF_SIZE 2048
uint8_t cdc_buffer[CDC_BUF_SIZE];

static const char *DTAG = "CDC-DIODESP";

const uint32_t renderCool = 16;

static SemaphoreHandle_t lvgl_mux = NULL;

static inline bool lvgl_lock(int timeout_ms) {
    if (!lvgl_mux) return true;
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static inline void lvgl_unlock(void) {
    if (lvgl_mux) {
        xSemaphoreGive(lvgl_mux);
    }
}

// Page definitions
typedef enum {
    PAGE_MUSIC = 0,
    PAGE_EQ,
    PAGE_COUNT
} app_page_t;

static lv_obj_t *pages[PAGE_COUNT] = {NULL};
static app_page_t active_page = PAGE_MUSIC;

// Input groups for 3-button navigation
static lv_group_t *music_group = NULL;
static lv_group_t *eq_group = NULL;
static lv_group_t *menu_group = NULL;
static lv_group_t *active_group = NULL;

// Music page UI elements
static lv_obj_t *label_title = NULL;
static lv_obj_t *label_artist = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *label_play_icon = NULL;
static lv_obj_t *bar_progress = NULL;
static lv_obj_t *label_time = NULL;
static lv_obj_t *btn_play_obj = NULL;
static lv_obj_t *hint_label = NULL;

// Focus style and hint visibility
static lv_style_t style_focus;
static bool hint_visible = true;
static uint32_t last_input_time = 0;

// Menu popup elements
static lv_obj_t *menu_backdrop = NULL;
static bool menu_is_open = false;

void open_menu_popup(void);
void close_menu_popup(void);

void set_hint_visible(bool show) {
    if (hint_visible == show) return;
    hint_visible = show;

    if (hint_label) {
        if (show) {
            lv_obj_remove_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_style_set_outline_opa(&style_focus, show ? LV_OPA_COVER : LV_OPA_0);
    lv_obj_report_style_change(&style_focus);
}

void switch_page(app_page_t page) {
    if (page >= PAGE_COUNT) return;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (pages[i]) {
            if (i == page) {
                lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    active_page = page;

    if (page == PAGE_MUSIC && music_group) {
        active_group = music_group;
        if (btn_play_obj) {
            lv_group_focus_obj(btn_play_obj);
        }
    } else if (page == PAGE_EQ && eq_group) {
        active_group = eq_group;
    }
}

void send_cdc_cmd(const char *cmd) {
    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        return;
    }
    size_t len = strlen(cmd);
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)cmd, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    ESP_LOGI(DTAG, "Sent to PC: %s", cmd);
}

void ui_music_reset_default(void) {
    if (label_title) {
        lv_label_set_text(label_title, "Waiting for MPRIS...");
    }
    if (label_artist) {
        lv_label_set_text(label_artist, "Python script not initiated");
        lv_obj_set_style_text_color(label_artist, lv_color_hex(0x888888), 0);
    }
    if (label_status) {
        lv_label_set_text(label_status, "DISCONNECTED");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
    }
    if (label_play_icon) {
        lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    }
    if (bar_progress) {
        lv_bar_set_range(bar_progress, 0, 100);
        lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    }
    if (label_time) {
        lv_label_set_text(label_time, "00:00 / 00:00");
    }
}

void ui_music_set_connected(bool connected) {
    if (connected) {
        if (label_status) {
            lv_label_set_text(label_status, "CONNECTED");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0x00E5FF), 0);
        }
        if (label_artist) {
            lv_label_set_text(label_artist, "Waiting for media player...");
            lv_obj_set_style_text_color(label_artist, lv_color_hex(0xAAAAAA), 0);
        }
    } else {
        ui_music_reset_default();
    }
}

void ui_music_set_title(const char *title) {
    if (!label_title) return;
    if (title && strlen(title) > 0) {
        lv_label_set_text(label_title, title);
    }
}

void ui_music_set_artist(const char *artist) {
    if (!label_artist) return;
    if (artist && strlen(artist) > 0) {
        lv_label_set_text(label_artist, artist);
        lv_obj_set_style_text_color(label_artist, lv_color_hex(0x1DB954), 0);
    }
}

void ui_music_set_progress(int cur_sec, int total_sec) {
    if (bar_progress) {
        if (total_sec > 0) {
            lv_bar_set_range(bar_progress, 0, total_sec);
            lv_bar_set_value(bar_progress, cur_sec, LV_ANIM_OFF);
        } else {
            lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
        }
    }
    if (label_time) {
        if (total_sec > 0) {
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d",
                     cur_sec / 60, cur_sec % 60,
                     total_sec / 60, total_sec % 60);
            lv_label_set_text(label_time, time_str);
        } else {
            lv_label_set_text(label_time, "00:00 / 00:00");
        }
    }
}

void ui_music_set_status(const char *status) {
    if (!label_status || !label_play_icon) return;
    if (strcasecmp(status, "Playing") == 0) {
        lv_label_set_text(label_status, "PLAYING");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x1DB954), 0);
        lv_label_set_text(label_play_icon, LV_SYMBOL_PAUSE);
    } else if (strcasecmp(status, "Paused") == 0) {
        lv_label_set_text(label_status, "PAUSED");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF9800), 0);
        lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    } else {
        lv_label_set_text(label_status, status);
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
        lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    }
}

static void btn_mpris_event_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) {
        send_cdc_cmd(cmd);
    }
}

static void btn_menu_event_cb(lv_event_t *e) {
    open_menu_popup();
}

static void menu_item_click_cb(lv_event_t *e) {
    uintptr_t item_id = (uintptr_t)lv_event_get_user_data(e);
    if (item_id == 0) {
        switch_page(PAGE_MUSIC);
    } else if (item_id == 1) {
        switch_page(PAGE_EQ);
    }
    close_menu_popup();
}

void open_menu_popup(void) {
    if (menu_is_open) return;

    lv_obj_t *screen = lv_screen_active();

    menu_backdrop = lv_obj_create(screen);
    lv_obj_set_size(menu_backdrop, 480, 320);
    lv_obj_set_pos(menu_backdrop, 0, 0);
    lv_obj_set_style_bg_color(menu_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(menu_backdrop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(menu_backdrop, 0, 0);
    lv_obj_set_style_radius(menu_backdrop, 0, 0);
    lv_obj_set_style_pad_all(menu_backdrop, 0, 0);
    lv_obj_remove_flag(menu_backdrop, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *modal = lv_obj_create(menu_backdrop);
    lv_obj_set_size(modal, 260, 195);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_width(modal, 1, 0);
    lv_obj_set_style_border_color(modal, lv_color_hex(0x383838), 0);
    lv_obj_set_style_radius(modal, 12, 0);
    lv_obj_set_style_pad_all(modal, 12, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_hdr = lv_label_create(modal);
    lv_label_set_text(lbl_hdr, "SELECT PAGE");
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_hdr, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_align(lbl_hdr, LV_ALIGN_TOP_MID);

    menu_group = lv_group_create();

    // Menu1 Media Player
    lv_obj_t *btn_music = lv_button_create(modal);
    lv_obj_set_size(btn_music, 234, 38);
    lv_obj_set_pos(btn_music, 0, 30);
    lv_obj_set_style_bg_color(btn_music, lv_color_hex(0x282828), 0);
    lv_obj_set_style_radius(btn_music, 8, 0);
    lv_obj_set_style_outline_width(btn_music, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_music, lv_color_hex(0x1DB954), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_music, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_music, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0);

    lv_obj_t *lbl_music = lv_label_create(btn_music);
    lv_label_set_text(lbl_music, LV_SYMBOL_AUDIO "  Media Player");
    lv_obj_center(lbl_music);

    // Menu2 Equalizer (will be focussed to this later)
    lv_obj_t *btn_eq = lv_button_create(modal);
    lv_obj_set_size(btn_eq, 234, 38);
    lv_obj_set_pos(btn_eq, 0, 74);
    lv_obj_set_style_bg_color(btn_eq, lv_color_hex(0x282828), 0);
    lv_obj_set_style_radius(btn_eq, 8, 0);
    lv_obj_set_style_outline_width(btn_eq, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_eq, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_eq, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_eq, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1);

    lv_obj_t *lbl_eq = lv_label_create(btn_eq);
    lv_label_set_text(lbl_eq, LV_SYMBOL_SETTINGS "  Equalizer");
    lv_obj_center(lbl_eq);

    // Menu 3 close, to go back last time you at the manu
    lv_obj_t *btn_close = lv_button_create(modal);
    lv_obj_set_size(btn_close, 234, 34);
    lv_obj_set_pos(btn_close, 0, 118);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x222222), 0);
    lv_obj_set_style_radius(btn_close, 8, 0);
    lv_obj_set_style_outline_width(btn_close, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_close, lv_color_hex(0xFF5555), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_close, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_close, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)2);

    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE "  Close");
    lv_obj_center(lbl_close);

    lv_group_add_obj(menu_group, btn_music);
    lv_group_add_obj(menu_group, btn_eq);
    lv_group_add_obj(menu_group, btn_close);

    if (active_page == PAGE_MUSIC) {
        lv_group_focus_obj(btn_music);
    } else {
        lv_group_focus_obj(btn_eq);
    }

    menu_is_open = true;
}

void close_menu_popup(void) {
    if (!menu_is_open) return;

    if (menu_backdrop) {
        lv_obj_del(menu_backdrop);
        menu_backdrop = NULL;
    }
    if (menu_group) {
        lv_group_delete(menu_group);
        menu_group = NULL;
    }
    menu_is_open = false;

    if (active_page == PAGE_MUSIC && music_group) {
        active_group = music_group;
    } else if (active_page == PAGE_EQ && eq_group) {
        active_group = eq_group;
    }
}

static void create_music_page(lv_obj_t *parent) {
    pages[PAGE_MUSIC] = lv_obj_create(parent);
    lv_obj_t *page = pages[PAGE_MUSIC];
    lv_obj_set_size(page, 480, 320);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    music_group = lv_group_create();

    // Header
    lv_obj_t *header_label = lv_label_create(page);
    lv_label_set_text(header_label, "DIODESP");
    lv_obj_set_style_text_color(header_label, lv_color_hex(0x1DB954), 0);
    lv_obj_set_pos(header_label, 20, 12);

    label_status = lv_label_create(page);
    lv_label_set_text(label_status, "DISCONNECTED");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(label_status, 110, 12);

    // Menu button
    lv_obj_t *btn_menu = lv_button_create(page);
    lv_obj_set_size(btn_menu, 90, 26);
    lv_obj_set_pos(btn_menu, 370, 8);
    lv_obj_set_style_bg_color(btn_menu, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(btn_menu, 1, 0);
    lv_obj_set_style_border_color(btn_menu, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn_menu, 13, 0);
    lv_obj_add_style(btn_menu, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_menu, btn_menu_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_menu_lbl = lv_label_create(btn_menu);
    lv_label_set_text(btn_menu_lbl, LV_SYMBOL_LIST " MENU");
    lv_obj_center(btn_menu_lbl);

    // Separator line
    lv_obj_t *line = lv_obj_create(page);
    lv_obj_set_size(line, 440, 1);
    lv_obj_set_pos(line, 20, 38);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x262626), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    // Album Art container
    lv_obj_t *art_box = lv_obj_create(page);
    lv_obj_set_size(art_box, 130, 130);
    lv_obj_set_pos(art_box, 20, 50);
    lv_obj_set_style_bg_color(art_box, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_width(art_box, 1, 0);
    lv_obj_set_style_border_color(art_box, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_radius(art_box, 12, 0);
    lv_obj_remove_flag(art_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *art_lbl = lv_label_create(art_box);
    lv_label_set_text(art_lbl, LV_SYMBOL_AUDIO "\nMPRIS");
    lv_obj_set_style_text_align(art_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(art_lbl, lv_color_hex(0x555555), 0);
    lv_obj_center(art_lbl);

    // Titit and Artist
    label_title = lv_label_create(page);
    lv_label_set_text(label_title, "Waiting for MPRIS...");
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(label_title, 168, 50);
    lv_obj_set_width(label_title, 290);
    lv_label_set_long_mode(label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    label_artist = lv_label_create(page);
    lv_label_set_text(label_artist, "Python script not initiated");
    lv_obj_set_style_text_font(label_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_artist, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(label_artist, 168, 86);
    lv_obj_set_width(label_artist, 290);
    lv_label_set_long_mode(label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Progress Bar
    bar_progress = lv_bar_create(page);
    lv_obj_set_size(bar_progress, 290, 8);
    lv_obj_set_pos(bar_progress, 168, 122);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x282828), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x1DB954), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 4, 0);

    // Elapsed / Total Time
    label_time = lv_label_create(page);
    lv_label_set_text(label_time, "00:00 / 00:00");
    lv_obj_set_style_text_font(label_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_time, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(label_time, 168, 138);

    // Playback control buttons
    lv_obj_t *btn_prev = lv_button_create(page);
    lv_obj_set_size(btn_prev, 64, 46);
    lv_obj_set_pos(btn_prev, 120, 205);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x242424), 0);
    lv_obj_set_style_radius(btn_prev, 23, 0);
    lv_obj_add_style(btn_prev, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_prev, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"previous\n");

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_center(lbl_prev);

    btn_play_obj = lv_button_create(page);
    lv_obj_set_size(btn_play_obj, 64, 64);
    lv_obj_set_pos(btn_play_obj, 208, 196);
    lv_obj_set_style_bg_color(btn_play_obj, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_radius(btn_play_obj, 32, 0);
    lv_obj_add_style(btn_play_obj, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_play_obj, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"pause:play\n");

    label_play_icon = lv_label_create(btn_play_obj);
    lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(label_play_icon, lv_color_hex(0x000000), 0);
    lv_obj_center(label_play_icon);

    lv_obj_t *btn_next = lv_button_create(page);
    lv_obj_set_size(btn_next, 64, 46);
    lv_obj_set_pos(btn_next, 296, 205);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x242424), 0);
    lv_obj_set_style_radius(btn_next, 23, 0);
    lv_obj_add_style(btn_next, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_next, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"next\n");

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_center(lbl_next);

    // Auto-hiding navigation hint
    hint_label = lv_label_create(page);
    lv_label_set_text(hint_label, "PREV: <   OK: Select   NEXT: >");
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
    lv_obj_set_align(hint_label, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_pad_bottom(hint_label, 10, 0);

    // Add interactive elements to music group
    lv_group_add_obj(music_group, btn_prev);
    lv_group_add_obj(music_group, btn_play_obj);
    lv_group_add_obj(music_group, btn_next);
    lv_group_add_obj(music_group, btn_menu);

    lv_group_focus_obj(btn_play_obj);
    active_group = music_group;
}

static void create_eq_page(lv_obj_t *parent) {
    pages[PAGE_EQ] = lv_obj_create(parent);
    lv_obj_t *page = pages[PAGE_EQ];
    lv_obj_set_size(page, 480, 320);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    eq_group = lv_group_create();

    // Header
    lv_obj_t *header_label = lv_label_create(page);
    lv_label_set_text(header_label, "DIODESP • EQUALIZER");
    lv_obj_set_style_text_color(header_label, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_pos(header_label, 20, 12);

    // Menu button on EQ page
    lv_obj_t *btn_menu_eq = lv_button_create(page);
    lv_obj_set_size(btn_menu_eq, 90, 26);
    lv_obj_set_pos(btn_menu_eq, 370, 8);
    lv_obj_set_style_bg_color(btn_menu_eq, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(btn_menu_eq, 1, 0);
    lv_obj_set_style_border_color(btn_menu_eq, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn_menu_eq, 13, 0);
    lv_obj_add_style(btn_menu_eq, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_menu_eq, btn_menu_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_menu_lbl = lv_label_create(btn_menu_eq);
    lv_label_set_text(btn_menu_lbl, LV_SYMBOL_LIST " MENU");
    lv_obj_center(btn_menu_lbl);

    lv_group_add_obj(eq_group, btn_menu_eq);

    // Placeholder info
    lv_obj_t *eq_center_lbl = lv_label_create(page);
    lv_label_set_text(eq_center_lbl, "EQUALIZER DSP PAGE\n(DSP Biquads & Spectrum)");
    lv_obj_set_style_text_align(eq_center_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(eq_center_lbl, lv_color_hex(0x888888), 0);
    lv_obj_center(eq_center_lbl);

    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static char rx_line_buf[256];
static size_t rx_line_pos = 0;

static void process_serial_line(char *line) {
    if (strncmp(line, "HOST:CONNECTED", 14) == 0) {
        ESP_LOGI(DTAG, "Host connected");
        if (lvgl_lock(50)) {
            ui_music_set_connected(true);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "HOST:DISCONNECTED", 17) == 0) {
        ESP_LOGI(DTAG, "Host disconnected");
        if (lvgl_lock(50)) {
            ui_music_reset_default();
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "TITLE:", 6) == 0) {
        char *title = line + 6;
        ESP_LOGI(DTAG, "Title: %s", title);
        if (lvgl_lock(50)) {
            ui_music_set_title(title);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "ARTIST:", 7) == 0) {
        char *artist = line + 7;
        ESP_LOGI(DTAG, "Artist: %s", artist);
        if (lvgl_lock(50)) {
            ui_music_set_artist(artist);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "TXT:", 4) == 0) {
        char *payload = line + 4;
        ESP_LOGI(DTAG, "TXT: %s", payload);
        char title[128] = {0};
        char artist[128] = {0};
        char *sep = strstr(payload, " - ");
        if (sep) {
            size_t t_len = sep - payload;
            if (t_len >= sizeof(title)) t_len = sizeof(title) - 1;
            strncpy(title, payload, t_len);
            strncpy(artist, sep + 3, sizeof(artist) - 1);
        } else {
            strncpy(title, payload, sizeof(title) - 1);
            strncpy(artist, "Unknown Artist", sizeof(artist) - 1);
        }
        if (lvgl_lock(50)) {
            ui_music_set_title(title);
            ui_music_set_artist(artist);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "POS:", 4) == 0) {
        int cur_sec = 0;
        int total_sec = 0;
        if (sscanf(line + 4, "%d:%d", &cur_sec, &total_sec) >= 2) {
            if (lvgl_lock(50)) {
                ui_music_set_progress(cur_sec, total_sec);
                lvgl_unlock();
            }
        }
    }
    else if (strncmp(line, "STATUS:", 7) == 0) {
        char *status = line + 7;
        ESP_LOGI(DTAG, "Status: %s", status);
        if (lvgl_lock(50)) {
            ui_music_set_status(status);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "IMG:", 4) == 0) {
        ESP_LOGI(DTAG, "Image byte received");
    }
}

static void rx_callback(int itf, cdcacm_event_t *event) {
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(itf, cdc_buffer, CDC_BUF_SIZE - 1, &rx_size);
    if (ret != ESP_OK || rx_size == 0) return;

    for (size_t i = 0; i < rx_size; i++) {
        char c = (char)cdc_buffer[i];
        if (c == '\n' || c == '\r') {
            if (rx_line_pos > 0) {
                rx_line_buf[rx_line_pos] = '\0';
                process_serial_line(rx_line_buf);
                rx_line_pos = 0;
            }
        } else {
            if (rx_line_pos < sizeof(rx_line_buf) - 1) {
                rx_line_buf[rx_line_pos++] = c;
            }
        }
    }
}

static void line_state_changed_callback(int itf, cdcacm_event_t *event) {
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    ESP_LOGI(DTAG, "CDC line state: DTR=%d, RTS=%d", dtr, rts);

    if (lvgl_lock(50)) {
        if (dtr) {
            ui_music_set_connected(true);
        } else {
            ui_music_reset_default();
        }
        lvgl_unlock();
    }
}

static void usb_event_callback(tinyusb_event_t *event, void *arg) {
    if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(DTAG, "USB detached");
        if (lvgl_lock(50)) {
            ui_music_reset_default();
            lvgl_unlock();
        }
    } else if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(DTAG, "USB attached");
    }
}

static void init_buttons(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PSPL) | (1ULL << NXT) | (1ULL << PVS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static void button_task(void *pvParameters) {
    init_buttons();
    int last_pspl = 1, last_nxt = 1, last_pvs = 1;
    uint32_t pspl_press_start = 0;
    bool pspl_long_triggered = false;
    last_input_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        int r_pspl = gpio_get_level(PSPL);
        int r_nxt  = gpio_get_level(NXT);
        int r_pvs  = gpio_get_level(PVS);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Auto-hide hint and focus outline after 4 seconds of idle inactivity
        if (!menu_is_open && hint_visible && (now - last_input_time >= 4000)) {
            if (lvgl_lock(50)) {
                set_hint_visible(false);
                lvgl_unlock();
            }
        }

        // PREV: Navigate backward
        if (last_pvs == 1 && r_pvs == 0) {
            last_input_time = now;
            ESP_LOGI(DTAG, "Button: PREV");
            if (lvgl_lock(50)) {
                set_hint_visible(true);
                lv_group_t *target_grp = (menu_is_open && menu_group) ? menu_group : active_group;
                if (target_grp) {
                    lv_group_focus_prev(target_grp);
                }
                lvgl_unlock();
            }
        }

        // NEXT: Navigate forward
        if (last_nxt == 1 && r_nxt == 0) {
            last_input_time = now;
            ESP_LOGI(DTAG, "Button: NEXT");
            if (lvgl_lock(50)) {
                set_hint_visible(true);
                lv_group_t *target_grp = (menu_is_open && menu_group) ? menu_group : active_group;
                if (target_grp) {
                    lv_group_focus_next(target_grp);
                }
                lvgl_unlock();
            }
        }

        // PSPL: OK / Confirm
        if (last_pspl == 1 && r_pspl == 0) {
            pspl_press_start = now;
            last_input_time = now;
            pspl_long_triggered = false;
            if (lvgl_lock(50)) {
                set_hint_visible(true);
                lvgl_unlock();
            }
        } else if (r_pspl == 0 && !pspl_long_triggered) {
            // Long press (> 600ms) toggles menu popup
            if (now - pspl_press_start >= 600) {
                pspl_long_triggered = true;
                last_input_time = now;
                ESP_LOGI(DTAG, "Button: OK (Long press -> Toggle Menu)");
                if (lvgl_lock(50)) {
                    set_hint_visible(true);
                    if (menu_is_open) {
                        close_menu_popup();
                    } else {
                        open_menu_popup();
                    }
                    lvgl_unlock();
                }
            }
        } else if (last_pspl == 0 && r_pspl == 1) {
            // Short press release: click focused object
            if (!pspl_long_triggered) {
                last_input_time = now;
                ESP_LOGI(DTAG, "Button: OK (Short press)");
                if (lvgl_lock(50)) {
                    set_hint_visible(true);
                    lv_group_t *target_grp = (menu_is_open && menu_group) ? menu_group : active_group;
                    if (target_grp) {
                        lv_obj_t *focused = lv_group_get_focused(target_grp);
                        if (focused) {
                            lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
                        }
                    }
                    lvgl_unlock();
                }
            }
        }

        last_pspl = r_pspl;
        last_nxt = r_nxt;
        last_pvs = r_pvs;

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void render(void *pvParameters) {
    lv_style_init(&style_focus);
    lv_style_set_outline_width(&style_focus, 2);
    lv_style_set_outline_color(&style_focus, lv_color_hex(0x1DB954));
    lv_style_set_outline_pad(&style_focus, 3);
    lv_style_set_outline_opa(&style_focus, LV_OPA_COVER);

    lv_obj_t *screen = lv_screen_active();
    create_music_page(screen);
    create_eq_page(screen);

    TickType_t last_tick = xTaskGetTickCount();
    const TickType_t coolFreq = pdMS_TO_TICKS(renderCool);

    while (1) {
        lv_tick_inc(renderCool);
        if (lvgl_lock(20)) {
            lv_timer_handler();
            lvgl_unlock();
        }
        vTaskDelayUntil(&last_tick, coolFreq);
    }
}

void app_main(void) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    GF_to_LV();

    const tinyusb_config_t cdc_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_callback, NULL);
    ESP_ERROR_CHECK(tinyusb_driver_install(&cdc_cfg));

    tinyusb_config_cdcacm_t amc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &line_state_changed_callback,
        .callback_line_coding_changed = NULL
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&amc_cfg));

    xTaskCreatePinnedToCore(&render, "render_disp", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&button_task, "button_task", 4096, NULL, 4, NULL, 1);
}
