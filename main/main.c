#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "class/hid/hid_device.h"
#include "class/audio/audio_device.h"
#include "lvgl.h"
#include "include/lovyan_config.h"
#include "include/theme.h"
#include "include/font_cjk_kr.h"

// Hardware navigation buttons
#define PSPL GPIO_NUM_4  // OK / Play-Pause
#define NXT  GPIO_NUM_5  // Next / Navigate forward
#define PVS  GPIO_NUM_8  // Prev / Navigate backward

#define CDC_BUF_SIZE 2048
uint8_t cdc_buffer[CDC_BUF_SIZE];

static const char *DTAG = "CDC-DIODESP";

static uint32_t lvgl_tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

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

// Global active theme instance
app_theme_t theme;

void theme_init_base(void) {
    // Screen and surfaces
    theme.bg_screen         = lv_color_hex(0x121212);
    theme.bg_card           = lv_color_hex(0x1E1E1E);
    theme.bg_btn            = lv_color_hex(0x242424);
    theme.bg_btn_subtle     = lv_color_hex(0x222222);
    theme.bg_item           = lv_color_hex(0x282828);
    theme.bg_overlay        = lv_color_hex(0x000000);

    // Borders and dividers
    theme.border            = lv_color_hex(0x383838);
    theme.border_subtle     = lv_color_hex(0x2E2E2E);
    theme.divider           = lv_color_hex(0x262626);

    // Typography and text
    theme.text_title        = lv_color_hex(0xFFFFFF);
    theme.text_primary      = lv_color_hex(0xFFFFFF);
    theme.text_secondary    = lv_color_hex(0xAAAAAA);
    theme.text_muted        = lv_color_hex(0x888888);
    theme.text_dim          = lv_color_hex(0x555555);
    theme.text_inverse      = lv_color_hex(0x000000);

    // Accents and status indicators
    theme.accent_primary    = lv_color_hex(0x1DB954);
    theme.accent_secondary  = lv_color_hex(0x00E5FF);
    theme.status_playing    = lv_color_hex(0x1DB954);
    theme.status_paused     = lv_color_hex(0xFF9800);
    theme.danger            = lv_color_hex(0xFF5555);
}

// Page definitions
typedef enum {
    PAGE_MUSIC = 0,
    PAGE_LYRICS,
    PAGE_EQ,
    PAGE_COUNT
} app_page_t;

typedef enum {
    VIEW_STANDARD = 0,
    VIEW_LYRICS
} player_view_t;

static lv_obj_t *pages[PAGE_COUNT] = {NULL};
static app_page_t active_page = PAGE_MUSIC;
static player_view_t current_view = VIEW_STANDARD;
static int32_t current_morph_val = 0;
static bool morph_anim_running = false;

// Input groups for 3-button navigation
static lv_group_t *music_group = NULL;
static lv_group_t *eq_group = NULL;
static lv_group_t *menu_group = NULL;
static lv_group_t *active_group = NULL;

// Unified music & lyrics player UI elements
static lv_obj_t *label_title = NULL;
static lv_obj_t *label_artist = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *label_play_icon = NULL;
static lv_obj_t *bar_progress = NULL;
static lv_obj_t *label_time = NULL;
static lv_obj_t *btn_prev = NULL;
static lv_obj_t *btn_play_obj = NULL;
static lv_obj_t *btn_next = NULL;
static lv_obj_t *btn_lyric_obj = NULL;
static lv_obj_t *btn_menu = NULL;
static lv_obj_t *hint_label = NULL;
static lv_obj_t *art_box = NULL;
static lv_obj_t *art_img = NULL;
static lv_obj_t *art_lbl = NULL;

static lv_obj_t *lyric_box = NULL;
static lv_obj_t *label_lyric_prev = NULL;
static lv_obj_t *label_lyric_curr = NULL;
static lv_obj_t *label_lyric_next = NULL;

// Album art definitions (130x130 RGB565)
#define ART_IMG_W 130
#define ART_IMG_H 130
#define ART_IMG_SIZE (ART_IMG_W * ART_IMG_H * 2)
#define ART_MAX_BUFFER_SIZE (130 * 130 * 2)

static uint8_t *art_img_buf = NULL;
static lv_image_dsc_t art_img_dsc = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565,
    .header.flags = 0,
    .header.w = ART_IMG_W,
    .header.h = ART_IMG_H,
    .header.stride = ART_IMG_W * 2,
    .data_size = ART_IMG_SIZE,
    .data = NULL,
};

// Serial RX parser state
typedef enum {
    RX_STATE_TEXT = 0,
    RX_STATE_IMAGE,
} rx_state_t;

static rx_state_t rx_state = RX_STATE_TEXT;
static size_t expected_img_bytes = 0;
static size_t img_bytes_received = 0;

// Font instances with CJK and Korean fallback
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
LV_FONT_DECLARE(lv_font_source_han_sans_sc_14_cjk);
static lv_font_t font_cjk;
#endif

static lv_font_t font_title;
static lv_font_t font_sub;

// Communication watchdog / keep-alive query
static uint32_t last_rx_time = 0;
static bool host_alive = false;
static bool query_sent = false;

// Focus style and hint visibility
static lv_style_t style_focus;
static bool hint_visible = true;
static uint32_t last_input_time = 0;

// Menu popup elements
static lv_obj_t *menu_backdrop = NULL;
static bool menu_is_open = false;

void open_menu_popup(void);
void close_menu_popup(void);
void start_view_morph(player_view_t target_view, bool animate);
void switch_page(app_page_t page);

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

static inline int32_t morph_lerp(int32_t a, int32_t b, int32_t progress) {
    return a + ((b - a) * progress) / 1000;
}

static void apply_morph(int32_t progress) {
    current_morph_val = progress;

    // Album art container & image
    if (art_box && art_img) {
        int32_t art_y = morph_lerp(50, 240, progress);
        int32_t art_size = morph_lerp(130, 70, progress);
        int32_t art_radius = morph_lerp(12, 10, progress);
        lv_obj_set_pos(art_box, 20, art_y);
        lv_obj_set_size(art_box, art_size, art_size);
        lv_obj_set_style_radius(art_box, art_radius, 0);
        lv_obj_set_size(art_img, art_size, art_size);
        lv_obj_center(art_img);
    }

    // Title and Artist
    if (label_title) {
        lv_obj_set_pos(label_title, morph_lerp(168, 20, progress), morph_lerp(50, 40, progress));
        lv_obj_set_width(label_title, morph_lerp(290, 440, progress));
    }
    if (label_artist) {
        lv_obj_set_pos(label_artist, morph_lerp(168, 20, progress), morph_lerp(86, 68, progress));
        lv_obj_set_width(label_artist, morph_lerp(290, 440, progress));
    }

    // Progress bar
    if (bar_progress) {
        lv_obj_set_pos(bar_progress, morph_lerp(168, 20, progress), morph_lerp(122, 218, progress));
        lv_obj_set_size(bar_progress, morph_lerp(290, 440, progress), 6);
    }

    // Time label (glides smoothly from under progress bar in standard view to right-edge in lyric view)
    if (label_time) {
        lv_obj_set_pos(label_time, morph_lerp(168, 375, progress), morph_lerp(138, 226, progress));
    }

    // Playback control buttons
    if (btn_prev) {
        lv_obj_set_pos(btn_prev, morph_lerp(96, 175, progress), morph_lerp(222, 254, progress));
        lv_obj_set_size(btn_prev, morph_lerp(56, 44, progress), morph_lerp(46, 38, progress));
        lv_obj_set_style_radius(btn_prev, morph_lerp(23, 19, progress), 0);
    }

    if (btn_play_obj) {
        int32_t play_size = morph_lerp(64, 52, progress);
        lv_obj_set_pos(btn_play_obj, morph_lerp(168, 231, progress), morph_lerp(213, 247, progress));
        lv_obj_set_size(btn_play_obj, play_size, play_size);
        lv_obj_set_style_radius(btn_play_obj, play_size / 2, 0);
    }

    if (btn_next) {
        lv_obj_set_pos(btn_next, morph_lerp(248, 295, progress), morph_lerp(222, 254, progress));
        lv_obj_set_size(btn_next, morph_lerp(56, 44, progress), morph_lerp(46, 38, progress));
        lv_obj_set_style_radius(btn_next, morph_lerp(23, 19, progress), 0);
    }

    if (btn_lyric_obj) {
        lv_obj_set_pos(btn_lyric_obj, morph_lerp(320, 351, progress), morph_lerp(222, 254, progress));
        lv_obj_set_size(btn_lyric_obj, morph_lerp(56, 44, progress), morph_lerp(46, 38, progress));
        lv_obj_set_style_radius(btn_lyric_obj, morph_lerp(23, 19, progress), 0);
    }

    // Lyric box: fades up and slides into position
    if (lyric_box) {
        if (progress == 0) {
            lv_obj_add_flag(lyric_box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(lyric_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(lyric_box, 20, morph_lerp(110, 94, progress));
            lv_obj_set_style_opa(lyric_box, (lv_opa_t)morph_lerp(0, LV_OPA_COVER, progress), 0);
        }
    }
}

static void morph_anim_exec_cb(lv_anim_t *a, int32_t val) {
    apply_morph(val);
}

static void morph_anim_completed_cb(lv_anim_t *a) {
    morph_anim_running = false;
}

void start_view_morph(player_view_t target_view, bool animate) {
    if (current_view == target_view && !morph_anim_running) {
        return;
    }
    current_view = target_view;
    active_page = (target_view == VIEW_LYRICS) ? PAGE_LYRICS : PAGE_MUSIC;
    int32_t target_val = (target_view == VIEW_LYRICS) ? 1000 : 0;

    if (!animate) {
        lv_anim_delete(&current_morph_val, NULL);
        morph_anim_running = false;
        apply_morph(target_val);
        return;
    }

    lv_anim_delete(&current_morph_val, NULL);
    morph_anim_running = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &current_morph_val);
    lv_anim_set_values(&a, current_morph_val, target_val);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_custom_exec_cb(&a, morph_anim_exec_cb);
    lv_anim_set_completed_cb(&a, morph_anim_completed_cb);
    lv_anim_start(&a);
}

void switch_page(app_page_t page) {
    if (page >= PAGE_COUNT) return;
    active_page = page;

    if (page == PAGE_MUSIC) {
        if (pages[PAGE_EQ]) {
            lv_obj_add_flag(pages[PAGE_EQ], LV_OBJ_FLAG_HIDDEN);
        }
        if (pages[PAGE_MUSIC]) {
            lv_obj_remove_flag(pages[PAGE_MUSIC], LV_OBJ_FLAG_HIDDEN);
        }
        active_group = music_group;
        start_view_morph(VIEW_STANDARD, true);
        if (btn_play_obj) {
            lv_group_focus_obj(btn_play_obj);
        }
    } else if (page == PAGE_LYRICS) {
        if (pages[PAGE_EQ]) {
            lv_obj_add_flag(pages[PAGE_EQ], LV_OBJ_FLAG_HIDDEN);
        }
        if (pages[PAGE_MUSIC]) {
            lv_obj_remove_flag(pages[PAGE_MUSIC], LV_OBJ_FLAG_HIDDEN);
        }
        active_group = music_group;
        start_view_morph(VIEW_LYRICS, true);
        if (btn_play_obj) {
            lv_group_focus_obj(btn_play_obj);
        }
    } else if (page == PAGE_EQ) {
        if (pages[PAGE_MUSIC]) {
            lv_obj_add_flag(pages[PAGE_MUSIC], LV_OBJ_FLAG_HIDDEN);
        }
        if (pages[PAGE_EQ]) {
            lv_obj_remove_flag(pages[PAGE_EQ], LV_OBJ_FLAG_HIDDEN);
        }
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

void ui_music_set_image(const uint8_t *data, uint16_t w, uint16_t h) {
    if (!data) return;

    art_img_dsc.header.w = w;
    art_img_dsc.header.h = h;
    art_img_dsc.header.stride = w * 2;
    art_img_dsc.data_size = w * h * 2;
    art_img_dsc.data = data;

    if (art_img && art_lbl) {
        lv_image_set_src(art_img, &art_img_dsc);
        lv_obj_add_flag(art_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(art_img);
    }
}

void ui_music_clear_image(void) {
    if (art_img) {
        lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (art_lbl) {
        lv_obj_remove_flag(art_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_music_set_lyrics(const char *prev, const char *curr, const char *next) {
    if (label_lyric_prev && prev) {
        lv_label_set_text(label_lyric_prev, prev);
    }
    if (label_lyric_curr && curr) {
        lv_label_set_text(label_lyric_curr, curr);
    }
    if (label_lyric_next && next) {
        lv_label_set_text(label_lyric_next, next);
    }
}

void ui_music_reset_default(void) {
    rx_state = RX_STATE_TEXT;
    expected_img_bytes = 0;
    img_bytes_received = 0;
    host_alive = false;
    query_sent = false;
    ui_music_clear_image();

    if (label_title) {
        lv_label_set_text(label_title, "Waiting for MPRIS...");
    }
    if (label_artist) {
        lv_label_set_text(label_artist, "Python script not initiated");
        lv_obj_set_style_text_color(label_artist, theme.text_muted, 0);
    }
    if (label_status) {
        lv_label_set_text(label_status, "DISCONNECTED");
        lv_obj_set_style_text_color(label_status, theme.text_muted, 0);
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
    ui_music_set_lyrics("• [Previous Lyric Line] •",
                        "♪ Synchronized lyrics will display here ♪",
                        "• [Next Lyric Line] •");
}

void ui_music_set_connected(bool connected) {
    if (connected) {
        if (label_status) {
            lv_label_set_text(label_status, "CONNECTED");
            lv_obj_set_style_text_color(label_status, theme.accent_secondary, 0);
        }
        if (label_artist) {
            lv_label_set_text(label_artist, "Waiting for media player...");
            lv_obj_set_style_text_color(label_artist, theme.text_secondary, 0);
        }
    } else {
        ui_music_reset_default();
    }
}

void ui_music_set_title(const char *title) {
    if (!title || strlen(title) == 0) return;
    if (label_title) {
        lv_label_set_text(label_title, title);
    }
}

void ui_music_set_artist(const char *artist) {
    if (!artist || strlen(artist) == 0) return;
    if (label_artist) {
        lv_label_set_text(label_artist, artist);
        lv_obj_set_style_text_color(label_artist, theme.accent_primary, 0);
    }
}

void ui_music_set_progress(int cur_sec, int total_sec) {
    if (total_sec > 0) {
        if (bar_progress) {
            lv_bar_set_range(bar_progress, 0, total_sec);
            lv_bar_set_value(bar_progress, cur_sec, LV_ANIM_OFF);
        }
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d",
                 cur_sec / 60, cur_sec % 60,
                 total_sec / 60, total_sec % 60);
        if (label_time) {
            lv_label_set_text(label_time, time_str);
        }
    } else {
        if (bar_progress) {
            lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
        }
        if (label_time) {
            lv_label_set_text(label_time, "00:00 / 00:00");
        }
    }
}

void ui_music_set_status(const char *status) {
    if (!status) return;
    const char *icon = LV_SYMBOL_PLAY;
    const char *text = status;
    lv_color_t col = theme.text_muted;

    if (strcasecmp(status, "Playing") == 0) {
        text = "PLAYING";
        col = theme.status_playing;
        icon = LV_SYMBOL_PAUSE;
    } else if (strcasecmp(status, "Paused") == 0) {
        text = "PAUSED";
        col = theme.status_paused;
        icon = LV_SYMBOL_PLAY;
    }

    if (label_status) {
        lv_label_set_text(label_status, text);
        lv_obj_set_style_text_color(label_status, col, 0);
    }
    if (label_play_icon) {
        lv_label_set_text(label_play_icon, icon);
    }
}

static void btn_mpris_event_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) {
        send_cdc_cmd(cmd);
    }
}

static void btn_lyric_toggle_cb(lv_event_t *e) {
    if (current_view == VIEW_STANDARD) {
        start_view_morph(VIEW_LYRICS, true);
        send_cdc_cmd("lyrics:open\n");
    } else {
        start_view_morph(VIEW_STANDARD, true);
        send_cdc_cmd("lyrics:close\n");
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
        switch_page(PAGE_LYRICS);
    } else if (item_id == 2) {
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
    lv_obj_set_style_bg_color(menu_backdrop, theme.bg_overlay, 0);
    lv_obj_set_style_bg_opa(menu_backdrop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(menu_backdrop, 0, 0);
    lv_obj_set_style_radius(menu_backdrop, 0, 0);
    lv_obj_set_style_pad_all(menu_backdrop, 0, 0);
    lv_obj_remove_flag(menu_backdrop, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *modal = lv_obj_create(menu_backdrop);
    lv_obj_set_size(modal, 260, 235);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, theme.bg_card, 0);
    lv_obj_set_style_border_width(modal, 1, 0);
    lv_obj_set_style_border_color(modal, theme.border, 0);
    lv_obj_set_style_radius(modal, 12, 0);
    lv_obj_set_style_pad_all(modal, 12, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_hdr = lv_label_create(modal);
    lv_label_set_text(lbl_hdr, "SELECT PAGE");
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_hdr, theme.accent_secondary, 0);
    lv_obj_set_align(lbl_hdr, LV_ALIGN_TOP_MID);

    menu_group = lv_group_create();

    // Menu 1: Media Player
    lv_obj_t *btn_music = lv_button_create(modal);
    lv_obj_set_size(btn_music, 234, 36);
    lv_obj_set_pos(btn_music, 0, 28);
    lv_obj_set_style_bg_color(btn_music, theme.bg_item, 0);
    lv_obj_set_style_radius(btn_music, 8, 0);
    lv_obj_set_style_outline_width(btn_music, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_music, theme.accent_primary, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_music, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_music, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0);

    lv_obj_t *lbl_music = lv_label_create(btn_music);
    lv_label_set_text(lbl_music, LV_SYMBOL_AUDIO "  Media Player");
    lv_obj_center(lbl_music);

    // Menu 2: Lyrics View
    lv_obj_t *btn_lyrics = lv_button_create(modal);
    lv_obj_set_size(btn_lyrics, 234, 36);
    lv_obj_set_pos(btn_lyrics, 0, 70);
    lv_obj_set_style_bg_color(btn_lyrics, theme.bg_item, 0);
    lv_obj_set_style_radius(btn_lyrics, 8, 0);
    lv_obj_set_style_outline_width(btn_lyrics, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_lyrics, theme.accent_primary, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_lyrics, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_lyrics, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1);

    lv_obj_t *lbl_lyrics = lv_label_create(btn_lyrics);
    lv_label_set_text(lbl_lyrics, "♪  Lyrics View");
    lv_obj_center(lbl_lyrics);

    // Menu 3: Equalizer
    lv_obj_t *btn_eq = lv_button_create(modal);
    lv_obj_set_size(btn_eq, 234, 36);
    lv_obj_set_pos(btn_eq, 0, 112);
    lv_obj_set_style_bg_color(btn_eq, theme.bg_item, 0);
    lv_obj_set_style_radius(btn_eq, 8, 0);
    lv_obj_set_style_outline_width(btn_eq, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_eq, theme.accent_secondary, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_eq, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_eq, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)2);

    lv_obj_t *lbl_eq = lv_label_create(btn_eq);
    lv_label_set_text(lbl_eq, LV_SYMBOL_SETTINGS "  Equalizer");
    lv_obj_center(lbl_eq);

    // Menu 4: Close
    lv_obj_t *btn_close = lv_button_create(modal);
    lv_obj_set_size(btn_close, 234, 34);
    lv_obj_set_pos(btn_close, 0, 156);
    lv_obj_set_style_bg_color(btn_close, theme.bg_btn_subtle, 0);
    lv_obj_set_style_radius(btn_close, 8, 0);
    lv_obj_set_style_outline_width(btn_close, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn_close, theme.danger, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn_close, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_close, menu_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)3);

    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE "  Close");
    lv_obj_center(lbl_close);

    lv_group_add_obj(menu_group, btn_music);
    lv_group_add_obj(menu_group, btn_lyrics);
    lv_group_add_obj(menu_group, btn_eq);
    lv_group_add_obj(menu_group, btn_close);

    if (active_page == PAGE_MUSIC) {
        lv_group_focus_obj(btn_music);
    } else if (active_page == PAGE_LYRICS) {
        lv_group_focus_obj(btn_lyrics);
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

    if ((active_page == PAGE_MUSIC || active_page == PAGE_LYRICS) && music_group) {
        active_group = music_group;
    } else if (active_page == PAGE_EQ && eq_group) {
        active_group = eq_group;
    }
}

static void create_music_page(lv_obj_t *parent) {
    pages[PAGE_MUSIC] = lv_obj_create(parent);
    pages[PAGE_LYRICS] = pages[PAGE_MUSIC];
    lv_obj_t *page = pages[PAGE_MUSIC];
    lv_obj_set_size(page, 480, 320);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, theme.bg_screen, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    music_group = lv_group_create();

    // Header
    lv_obj_t *header_label = lv_label_create(page);
    lv_label_set_text(header_label, "DIODESP");
    lv_obj_set_style_text_color(header_label, theme.accent_primary, 0);
    lv_obj_set_pos(header_label, 20, 12);

    label_status = lv_label_create(page);
    lv_label_set_text(label_status, "DISCONNECTED");
    lv_obj_set_style_text_color(label_status, theme.text_muted, 0);
    lv_obj_set_pos(label_status, 110, 12);

    // Menu button
    btn_menu = lv_button_create(page);
    lv_obj_set_size(btn_menu, 90, 26);
    lv_obj_set_pos(btn_menu, 370, 8);
    lv_obj_set_style_bg_color(btn_menu, theme.bg_btn_subtle, 0);
    lv_obj_set_style_border_width(btn_menu, 1, 0);
    lv_obj_set_style_border_color(btn_menu, theme.border_subtle, 0);
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
    lv_obj_set_style_bg_color(line, theme.divider, 0);
    lv_obj_set_style_border_width(line, 0, 0);

    // Lyrics container (Middle section, animated during morph)
    lyric_box = lv_obj_create(page);
    lv_obj_set_size(lyric_box, 440, 116);
    lv_obj_set_pos(lyric_box, 20, 110);
    lv_obj_set_style_bg_opa(lyric_box, LV_OPA_0, 0);
    lv_obj_set_style_border_width(lyric_box, 0, 0);
    lv_obj_set_style_pad_all(lyric_box, 6, 0);
    lv_obj_set_style_opa(lyric_box, LV_OPA_0, 0);
    lv_obj_remove_flag(lyric_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lyric_box, LV_OBJ_FLAG_HIDDEN);

    label_lyric_prev = lv_label_create(lyric_box);
    lv_label_set_text(label_lyric_prev, "• [Previous Lyric Line] •");
    lv_obj_set_style_text_font(label_lyric_prev, &font_sub, 0);
    lv_obj_set_style_text_color(label_lyric_prev, theme.text_dim, 0);
    lv_obj_set_style_text_align(label_lyric_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label_lyric_prev, 424);
    lv_obj_set_pos(label_lyric_prev, 2, 8);
    lv_label_set_long_mode(label_lyric_prev, LV_LABEL_LONG_SCROLL_CIRCULAR);

    label_lyric_curr = lv_label_create(lyric_box);
    lv_label_set_text(label_lyric_curr, "♪ Synchronized lyrics will display here ♪");
    lv_obj_set_style_text_font(label_lyric_curr, &font_title, 0);
    lv_obj_set_style_text_color(label_lyric_curr, theme.accent_primary, 0);
    lv_obj_set_style_text_align(label_lyric_curr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label_lyric_curr, 424);
    lv_obj_set_pos(label_lyric_curr, 2, 40);
    lv_label_set_long_mode(label_lyric_curr, LV_LABEL_LONG_SCROLL_CIRCULAR);

    label_lyric_next = lv_label_create(lyric_box);
    lv_label_set_text(label_lyric_next, "• [Next Lyric Line] •");
    lv_obj_set_style_text_font(label_lyric_next, &font_sub, 0);
    lv_obj_set_style_text_color(label_lyric_next, theme.text_dim, 0);
    lv_obj_set_style_text_align(label_lyric_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label_lyric_next, 424);
    lv_obj_set_pos(label_lyric_next, 2, 82);
    lv_label_set_long_mode(label_lyric_next, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Album Art container
    art_box = lv_obj_create(page);
    lv_obj_set_size(art_box, 130, 130);
    lv_obj_set_pos(art_box, 20, 50);
    lv_obj_set_style_bg_color(art_box, theme.bg_card, 0);
    lv_obj_set_style_border_width(art_box, 1, 0);
    lv_obj_set_style_border_color(art_box, theme.border_subtle, 0);
    lv_obj_set_style_radius(art_box, 12, 0);
    lv_obj_set_style_pad_all(art_box, 0, 0);
    lv_obj_set_style_clip_corner(art_box, true, 0);
    lv_obj_remove_flag(art_box, LV_OBJ_FLAG_SCROLLABLE);

    art_lbl = lv_label_create(art_box);
    lv_label_set_text(art_lbl, LV_SYMBOL_AUDIO "\nMPRIS");
    lv_obj_set_style_text_align(art_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(art_lbl, theme.text_dim, 0);
    lv_obj_center(art_lbl);

    art_img = lv_image_create(art_box);
    lv_obj_set_size(art_img, 130, 130);
    lv_image_set_inner_align(art_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(art_img);
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);

    // Title and Artist
    label_title = lv_label_create(page);
    lv_label_set_text(label_title, "Waiting for MPRIS...");
    lv_obj_set_style_text_font(label_title, &font_title, 0);
    lv_obj_set_style_text_color(label_title, theme.text_title, 0);
    lv_obj_set_pos(label_title, 168, 50);
    lv_obj_set_width(label_title, 290);
    lv_label_set_long_mode(label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    label_artist = lv_label_create(page);
    lv_label_set_text(label_artist, "Python script not initiated");
    lv_obj_set_style_text_font(label_artist, &font_sub, 0);
    lv_obj_set_style_text_color(label_artist, theme.text_muted, 0);
    lv_obj_set_pos(label_artist, 168, 86);
    lv_obj_set_width(label_artist, 290);
    lv_label_set_long_mode(label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Progress Bar
    bar_progress = lv_bar_create(page);
    lv_obj_set_size(bar_progress, 290, 6);
    lv_obj_set_pos(bar_progress, 168, 122);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, theme.bg_item, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, theme.accent_primary, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 3, 0);

    // Elapsed / Total Time
    label_time = lv_label_create(page);
    lv_label_set_text(label_time, "00:00 / 00:00");
    lv_obj_set_style_text_font(label_time, &font_sub, 0);
    lv_obj_set_style_text_color(label_time, theme.text_muted, 0);
    lv_obj_set_pos(label_time, 168, 138);

    // Playback control buttons + Lyric button (Lower section)
    btn_prev = lv_button_create(page);
    lv_obj_set_size(btn_prev, 56, 46);
    lv_obj_set_pos(btn_prev, 96, 222);
    lv_obj_set_style_bg_color(btn_prev, theme.bg_btn, 0);
    lv_obj_set_style_radius(btn_prev, 23, 0);
    lv_obj_add_style(btn_prev, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_prev, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"previous\n");

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_center(lbl_prev);

    btn_play_obj = lv_button_create(page);
    lv_obj_set_size(btn_play_obj, 64, 64);
    lv_obj_set_pos(btn_play_obj, 168, 213);
    lv_obj_set_style_bg_color(btn_play_obj, theme.accent_primary, 0);
    lv_obj_set_style_radius(btn_play_obj, 32, 0);
    lv_obj_add_style(btn_play_obj, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_play_obj, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"pause:play\n");

    label_play_icon = lv_label_create(btn_play_obj);
    lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(label_play_icon, theme.text_inverse, 0);
    lv_obj_center(label_play_icon);

    btn_next = lv_button_create(page);
    lv_obj_set_size(btn_next, 56, 46);
    lv_obj_set_pos(btn_next, 248, 222);
    lv_obj_set_style_bg_color(btn_next, theme.bg_btn, 0);
    lv_obj_set_style_radius(btn_next, 23, 0);
    lv_obj_add_style(btn_next, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_next, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"next\n");

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_center(lbl_next);

    btn_lyric_obj = lv_button_create(page);
    lv_obj_set_size(btn_lyric_obj, 56, 46);
    lv_obj_set_pos(btn_lyric_obj, 320, 222);
    lv_obj_set_style_bg_color(btn_lyric_obj, theme.bg_btn, 0);
    lv_obj_set_style_radius(btn_lyric_obj, 23, 0);
    lv_obj_add_style(btn_lyric_obj, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_lyric_obj, btn_lyric_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_lyric = lv_label_create(btn_lyric_obj);
    lv_label_set_text(lbl_lyric, "♪");
    lv_obj_set_style_text_font(lbl_lyric, &font_sub, 0);
    lv_obj_set_style_text_color(lbl_lyric, theme.text_title, 0);
    lv_obj_center(lbl_lyric);

    // Auto-hiding navigation hint
    hint_label = lv_label_create(page);
    lv_label_set_text(hint_label, "PREV: <   OK: Select   NEXT: >");
    lv_obj_set_style_text_font(hint_label, &font_sub, 0);
    lv_obj_set_style_text_color(hint_label, theme.text_dim, 0);
    lv_obj_set_align(hint_label, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_pad_bottom(hint_label, 2, 0);

    // Add interactive elements to music group
    lv_group_add_obj(music_group, btn_prev);
    lv_group_add_obj(music_group, btn_play_obj);
    lv_group_add_obj(music_group, btn_next);
    lv_group_add_obj(music_group, btn_lyric_obj);
    lv_group_add_obj(music_group, btn_menu);

    lv_group_focus_obj(btn_play_obj);
    active_group = music_group;

    apply_morph(0);
}

static void create_eq_page(lv_obj_t *parent) {
    pages[PAGE_EQ] = lv_obj_create(parent);
    lv_obj_t *page = pages[PAGE_EQ];
    lv_obj_set_size(page, 480, 320);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, theme.bg_screen, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    eq_group = lv_group_create();

    // Header
    lv_obj_t *header_label = lv_label_create(page);
    lv_label_set_text(header_label, "DIODESP • EQUALIZER");
    lv_obj_set_style_text_color(header_label, theme.accent_secondary, 0);
    lv_obj_set_pos(header_label, 20, 12);

    // Menu button on EQ page
    lv_obj_t *btn_menu_eq = lv_button_create(page);
    lv_obj_set_size(btn_menu_eq, 90, 26);
    lv_obj_set_pos(btn_menu_eq, 370, 8);
    lv_obj_set_style_bg_color(btn_menu_eq, theme.bg_btn_subtle, 0);
    lv_obj_set_style_border_width(btn_menu_eq, 1, 0);
    lv_obj_set_style_border_color(btn_menu_eq, theme.border_subtle, 0);
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
    lv_obj_set_style_text_color(eq_center_lbl, theme.text_muted, 0);
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
    else if (strncmp(line, "LYRIC:", 6) == 0) {
        char *payload = line + 6;
        char *sep1 = strchr(payload, '|');
        if (sep1) {
            *sep1 = '\0';
            char *curr = sep1 + 1;
            char *sep2 = strchr(curr, '|');
            char *next = "";
            if (sep2) {
                *sep2 = '\0';
                next = sep2 + 1;
            }
            if (lvgl_lock(50)) {
                ui_music_set_lyrics(payload, curr, next);
                lvgl_unlock();
            }
        } else {
            if (lvgl_lock(50)) {
                ui_music_set_lyrics("", payload, "");
                lvgl_unlock();
            }
        }
    }
    else if (strncmp(line, "IMG:", 4) == 0) {
        if (strncmp(line + 4, "CLEAR", 5) == 0) {
            ESP_LOGI(DTAG, "Clear album art");
            if (lvgl_lock(50)) {
                ui_music_clear_image();
                lvgl_unlock();
            }
        } else {
            int img_len = atoi(line + 4);
            if (img_len > 0 && img_len <= ART_MAX_BUFFER_SIZE) {
                expected_img_bytes = (size_t)img_len;
                img_bytes_received = 0;
                rx_state = RX_STATE_IMAGE;
                ESP_LOGI(DTAG, "Receiving image: %d bytes", img_len);
            } else {
                ESP_LOGW(DTAG, "Invalid image length: %d", img_len);
            }
        }
    }
}

static void rx_callback(int itf, cdcacm_event_t *event) {
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(itf, cdc_buffer, CDC_BUF_SIZE, &rx_size);
    if (ret != ESP_OK || rx_size == 0) return;

    last_rx_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    host_alive = true;
    query_sent = false;

    size_t i = 0;
    while (i < rx_size) {
        if (rx_state == RX_STATE_IMAGE) {
            size_t needed = expected_img_bytes - img_bytes_received;
            size_t available = rx_size - i;
            size_t chunk = (available < needed) ? available : needed;

            if (art_img_buf && (img_bytes_received + chunk <= ART_MAX_BUFFER_SIZE)) {
                memcpy(&art_img_buf[img_bytes_received], &cdc_buffer[i], chunk);
            }
            img_bytes_received += chunk;
            i += chunk;

            if (img_bytes_received >= expected_img_bytes) {
                rx_state = RX_STATE_TEXT;
                ESP_LOGI(DTAG, "Image transfer complete (%u bytes)", (unsigned int)img_bytes_received);
                if (lvgl_lock(50)) {
                    int w = ART_IMG_W;
                    int h = ART_IMG_H;
                    if (expected_img_bytes == 130 * 130 * 2) {
                        w = 130;
                        h = 130;
                    }
                    ui_music_set_image(art_img_buf, w, h);
                    lvgl_unlock();
                }
            }
        } else {
            char c = (char)cdc_buffer[i++];
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
}

static void line_state_changed_callback(int itf, cdcacm_event_t *event) {
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    ESP_LOGI(DTAG, "CDC line state: DTR=%d, RTS=%d", dtr, rts);

    // Query host status instead of immediately wiping UI on transient DTR drops
    send_cdc_cmd("QUERY\n");
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

        // Query the host if no serial data has arrived for 4 seconds
        if (host_alive && !query_sent && (now - last_rx_time >= 4000)) {
            query_sent = true;
            send_cdc_cmd("QUERY\n");
        }

        // If host still has not answered after 8 seconds, reset UI to default
        if (host_alive && (now - last_rx_time >= 8000)) {
            host_alive = false;
            query_sent = false;
            ESP_LOGW(DTAG, "Host query timed out, resetting UI to default");
            if (lvgl_lock(50)) {
                ui_music_reset_default();
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
    font_title = lv_font_montserrat_24;
    font_sub = lv_font_montserrat_14;

#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    font_cjk = lv_font_source_han_sans_sc_14_cjk;
    font_cjk.fallback = &font_cjk_kr_14;
    font_title.fallback = &font_cjk;
    font_sub.fallback = &font_cjk;
#else
    font_title.fallback = &font_cjk_kr_14;
    font_sub.fallback = &font_cjk_kr_14;
#endif

    lv_style_init(&style_focus);
    lv_style_set_outline_width(&style_focus, 2);
    lv_style_set_outline_color(&style_focus, theme.accent_primary);
    lv_style_set_outline_pad(&style_focus, 3);
    lv_style_set_outline_opa(&style_focus, LV_OPA_COVER);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, theme.bg_screen, 0);
    create_music_page(screen);
    create_eq_page(screen);
    switch_page(PAGE_MUSIC);

    while (1) {
        uint32_t delay_ms = 10;
        if (lvgl_lock(20)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay_ms == LV_NO_TIMER_READY || delay_ms > 20) {
            delay_ms = 20;
        } else if (delay_ms < 1) {
            delay_ms = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void app_main(void) {
    theme_init_base();

    if (!art_img_buf) {
        art_img_buf = (uint8_t *)heap_caps_malloc(ART_MAX_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (!art_img_buf) {
            art_img_buf = (uint8_t *)heap_caps_malloc(ART_MAX_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!art_img_buf) {
            art_img_buf = (uint8_t *)malloc(ART_MAX_BUFFER_SIZE);
        }
    }

    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);
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

    xTaskCreatePinnedToCore(&render, "render_disp", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&button_task, "button_task", 4096, NULL, 4, NULL, 0);
}
