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

// play-pause, next, previous
// NOTE: GPIO 19 & 20 are USB D-/D+ lines. Swapped to GPIO 4 & 5 so USB is untouched.
#define PSPL GPIO_NUM_4
#define NXT  GPIO_NUM_5
#define PVS  GPIO_NUM_8

#define CDC_BUF_SIZE 2048
uint8_t cdc_buffer[CDC_BUF_SIZE];

static const char *DTAG = "CDC-DIODESP";

// change this to change da framerates. 16 = ~60 FPS, 33 = ~30 FPS
const uint32_t renderCool = 16;

// thread safety mutex for lvgl
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

// ----------------- Page Management -----------------
typedef enum {
    PAGE_MUSIC = 0,
    PAGE_EQ,
    PAGE_COUNT
} app_page_t;

static lv_obj_t *pages[PAGE_COUNT] = {NULL};
static app_page_t active_page = PAGE_MUSIC;

// UI elements on music page
static lv_obj_t *label_title = NULL;
static lv_obj_t *label_artist = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *label_play_icon = NULL;
static lv_obj_t *bar_progress = NULL;
static lv_obj_t *label_time = NULL;

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

static void btn_page_event_cb(lv_event_t *e) {
    app_page_t next_p = (active_page == PAGE_MUSIC) ? PAGE_EQ : PAGE_MUSIC;
    switch_page(next_p);
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

    // 1. Top Header
    lv_obj_t *header_label = lv_label_create(page);
    lv_label_set_text(header_label, "DIODESP");
    lv_obj_set_style_text_color(header_label, lv_color_hex(0x1DB954), 0);
    lv_obj_set_pos(header_label, 20, 12);

    label_status = lv_label_create(page);
    lv_label_set_text(label_status, "IDLE");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(label_status, 110, 12);

    // Page switch button (top right)
    lv_obj_t *page_btn = lv_button_create(page);
    lv_obj_set_size(page_btn, 110, 26);
    lv_obj_set_pos(page_btn, 350, 8);
    lv_obj_set_style_bg_color(page_btn, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(page_btn, 1, 0);
    lv_obj_set_style_border_color(page_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(page_btn, 13, 0);
    lv_obj_add_event_cb(page_btn, btn_page_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *page_btn_lbl = lv_label_create(page_btn);
    lv_label_set_text(page_btn_lbl, "PAGE [1/2] >");
    lv_obj_center(page_btn_lbl);

    // Subtle divider
    lv_obj_t *line = lv_obj_create(page);
    lv_obj_set_size(line, 440, 1);
    lv_obj_set_pos(line, 20, 38);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x262626), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    // 2. Album Art box
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

    // 3. Track Details (Right of Album Art)
    label_title = lv_label_create(page);
    lv_label_set_text(label_title, "No Song Playing");
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(label_title, 168, 50);
    lv_obj_set_width(label_title, 290);
    lv_label_set_long_mode(label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    label_artist = lv_label_create(page);
    lv_label_set_text(label_artist, "Waiting for MPRIS...");
    lv_obj_set_style_text_font(label_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_artist, lv_color_hex(0x1DB954), 0);
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

    // Time Label: 00:00 / 00:00
    label_time = lv_label_create(page);
    lv_label_set_text(label_time, "00:00 / 00:00");
    lv_obj_set_style_text_font(label_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_time, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(label_time, 168, 138);

    // 4. Control Buttons (Centered bottom)
    lv_obj_t *btn_prev = lv_button_create(page);
    lv_obj_set_size(btn_prev, 64, 46);
    lv_obj_set_pos(btn_prev, 120, 195);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x242424), 0);
    lv_obj_set_style_radius(btn_prev, 23, 0);
    lv_obj_add_event_cb(btn_prev, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"previous\n");

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_center(lbl_prev);

    lv_obj_t *btn_play = lv_button_create(page);
    lv_obj_set_size(btn_play, 64, 64);
    lv_obj_set_pos(btn_play, 208, 186);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_radius(btn_play, 32, 0);
    lv_obj_add_event_cb(btn_play, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"pause:play\n");

    label_play_icon = lv_label_create(btn_play);
    lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(label_play_icon, lv_color_hex(0x000000), 0);
    lv_obj_center(label_play_icon);

    lv_obj_t *btn_next = lv_button_create(page);
    lv_obj_set_size(btn_next, 64, 46);
    lv_obj_set_pos(btn_next, 296, 195);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x242424), 0);
    lv_obj_set_style_radius(btn_next, 23, 0);
    lv_obj_add_event_cb(btn_next, btn_mpris_event_cb, LV_EVENT_CLICKED, (void *)"next\n");

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_center(lbl_next);

    // Bottom hint label
    lv_obj_t *hint_lbl = lv_label_create(page);
    lv_label_set_text(hint_lbl, "HW Buttons: GPIO 4 (P/P)  GPIO 5 (NXT)  GPIO 8 (PVS)");
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(0x444444), 0);
    lv_obj_set_pos(hint_lbl, 55, 290);
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

    // Top Header
    lv_obj_t *header_label = lv_label_create(page);
    lv_label_set_text(header_label, "DIODESP • EQUALIZER");
    lv_obj_set_style_text_color(header_label, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_pos(header_label, 20, 12);

    // Page switch button (top right)
    lv_obj_t *page_btn = lv_button_create(page);
    lv_obj_set_size(page_btn, 110, 26);
    lv_obj_set_pos(page_btn, 350, 8);
    lv_obj_set_style_bg_color(page_btn, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(page_btn, 1, 0);
    lv_obj_set_style_border_color(page_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(page_btn, 13, 0);
    lv_obj_add_event_cb(page_btn, btn_page_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *page_btn_lbl = lv_label_create(page_btn);
    lv_label_set_text(page_btn_lbl, "< PAGE [2/2]");
    lv_obj_center(page_btn_lbl);

    // Center info
    lv_obj_t *eq_center_lbl = lv_label_create(page);
    lv_label_set_text(eq_center_lbl, "EQUALIZER DSP PAGE\n(DSP Biquads & Spectrum ready for next step)");
    lv_obj_set_style_text_align(eq_center_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(eq_center_lbl, lv_color_hex(0x888888), 0);
    lv_obj_center(eq_center_lbl);

    // Hidden by default
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

// Line buffer for incoming serial stream
static char rx_line_buf[256];
static size_t rx_line_pos = 0;

static void process_serial_line(char *line) {
    if (strncmp(line, "TITLE:", 6) == 0) {
        char *title = line + 6;
        ESP_LOGI(DTAG, "Got Title: %s", title);
        if (lvgl_lock(50)) {
            ui_music_set_title(title);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "ARTIST:", 7) == 0) {
        char *artist = line + 7;
        ESP_LOGI(DTAG, "Got Artist: %s", artist);
        if (lvgl_lock(50)) {
            ui_music_set_artist(artist);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "TXT:", 4) == 0) {
        char *payload = line + 4;
        ESP_LOGI(DTAG, "Got TXT: %s", payload);
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
        ESP_LOGI(DTAG, "Got Status: %s", status);
        if (lvgl_lock(50)) {
            ui_music_set_status(status);
            lvgl_unlock();
        }
    }
    else if (strncmp(line, "IMG:", 4) == 0) {
        ESP_LOGI(DTAG, "I can sense an Image Byte from mpris bro.");
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

// Physical button polling
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

    while (1) {
        int r_pspl = gpio_get_level(PSPL);
        int r_nxt  = gpio_get_level(NXT);
        int r_pvs  = gpio_get_level(PVS);

        // Active LOW (pressed = 0)
        if (last_pspl == 1 && r_pspl == 0) {
            ESP_LOGI(DTAG, "Physical Button: Play/Pause");
            send_cdc_cmd("pause:play\n");
        }
        if (last_nxt == 1 && r_nxt == 0) {
            ESP_LOGI(DTAG, "Physical Button: Next");
            send_cdc_cmd("next\n");
        }
        if (last_pvs == 1 && r_pvs == 0) {
            ESP_LOGI(DTAG, "Physical Button: Previous");
            send_cdc_cmd("previous\n");
        }

        last_pspl = r_pspl;
        last_nxt = r_nxt;
        last_pvs = r_pvs;

        vTaskDelay(pdMS_TO_TICKS(40)); // debounce delay
    }
}

void render(void *pvParameters) {
    lv_obj_t *screen = lv_screen_active();
    create_music_page(screen);
    create_eq_page(screen);

    TickType_t remember_toCig = xTaskGetTickCount();
    const TickType_t coolFreq = pdMS_TO_TICKS(renderCool);

    // to keep things running or render frame. :P
    while (1) {
        lv_tick_inc(renderCool);
        if (lvgl_lock(20)) {
            lv_timer_handler();
            lvgl_unlock();
        }
        vTaskDelayUntil(&remember_toCig, coolFreq);
    }
}

void app_main(void) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    GF_to_LV();

    // tinyusb driver needs to be installed FIRST before cdcacm init.
    // default config sets bus-powered mode (self_powered = false, vbus_monitor_io = -1)
    const tinyusb_config_t cdc_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&cdc_cfg));

    tinyusb_config_cdcacm_t amc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&amc_cfg));

    xTaskCreatePinnedToCore(&render, "render_disp", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&button_task, "button_task", 4096, NULL, 4, NULL, 1);
}
