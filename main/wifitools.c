#include "wifitools.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "sound.h"
#include "term.h"
#include "ui_draw.h"
#include "wifi.h"

#define FB_W DISPLAY_WIDTH
#define FB_H DISPLAY_HEIGHT

#define BG        0x0A0014
#define PANEL     0x16002A
#define ACCENT    0xBF00FF
#define LABEL     0xF2F2F7
#define DIM       0x8A8594
#define DANGER    0xF2334F
#define OK        0x4ADE80
#define BAR_OK    0x4C1D95
#define BAR_HOT   0xF2334F

#define CHANNELS      13
#define HOP_MS        280
#define HISTORY       48    // seconds of the graph
#define ALERT_PER_SEC 5     // frames/s that counts as an attack in progress

// Updated from the Wi-Fi task (promiscuous callback): keep it tiny
static volatile uint32_t total_count;
static volatile uint32_t second_count;
static volatile uint8_t last_source[6];
static volatile uint8_t last_channel;

static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    const uint8_t* p = pkt->payload;
    uint8_t frame_control = p[0];
    // Management frame, subtype 0x0C = deauthentication, 0x0A = disassociation
    if (frame_control == 0xC0 || frame_control == 0xA0) {
        total_count++;
        second_count++;
        memcpy((void*)last_source, p + 10, 6); // addr2: the (spoofed) transmitter
        last_channel = pkt->rx_ctrl.channel;
    }
}

static uint16_t* fb;
static uint8_t history[HISTORY]; // deauth frames per second, newest at the end
static int history_len;

static void push_history(int value) {
    if (value > 255) value = 255;
    if (history_len < HISTORY) {
        history[history_len++] = value;
    } else {
        memmove(history, history + 1, HISTORY - 1);
        history[HISTORY - 1] = value;
    }
}

static void draw(int per_sec, bool alert) {
    Canvas c = {fb, FB_W, FB_H};
    ui_fill(&c, BG);
    ui_back(&c, 6, 2, ACCENT);
    ui_status(&c, FB_W, 2);
    ui_text(&c, 20, 22, "DETECTOR DEAUTH", &fonts[3], LABEL, false);

    // Big total
    char num[16];
    snprintf(num, sizeof(num), "%lu", (unsigned long)total_count);
    ui_text(&c, 20, 54, num, &fonts[3], alert ? DANGER : LABEL, false);
    ui_text(&c, 20, 80, "quadros deauth/disassoc", &fonts[1], DIM, false);

    // Status line: live rate + channel
    char line[40];
    snprintf(line, sizeof(line), "agora: %d/s   canal %d", per_sec, last_channel ? last_channel : 1);
    ui_text(&c, 20, 98, line, &fonts[2], alert ? DANGER : OK, false);

    // Source of the last frame
    if (total_count > 0) {
        char mac[40];
        snprintf(mac, sizeof(mac), "origem %02x:%02x:%02x:%02x:%02x:%02x",
                 last_source[0], last_source[1], last_source[2], last_source[3], last_source[4], last_source[5]);
        ui_text(&c, 20, 116, mac, &fonts[1], DIM, false);
    }

    // Graph of frames per second
    int gx = 20, gy = 140, gw = FB_W - 40, gh = 56;
    ui_round_rect(&c, gx - 4, gy - 4, gw + 8, gh + 8, 6.0f, PANEL);
    int bars = history_len < gw ? history_len : gw;
    int bw = gw / (HISTORY);
    if (bw < 1) bw = 1;
    for (int i = 0; i < bars; i++) {
        int v = history[history_len - bars + i];
        int h = v * gh / 12; // 12/s fills the graph
        if (h > gh) h = gh;
        uint32_t color = v >= ALERT_PER_SEC ? BAR_HOT : BAR_OK;
        for (int x = 0; x < bw - 1; x++) {
            for (int y = 0; y < h; y++) ui_blend(&c, gx + i * bw + x, gy + gh - 1 - y, color, 255);
        }
    }

    if (alert) {
        ui_round_rect(&c, 20, FB_H - 36, FB_W - 40, 22, 6.0f, DANGER);
        const char* msg = "ATAQUE DEAUTH EM ANDAMENTO";
        ui_text(&c, (FB_W - (int)strlen(msg) * 8) / 2, FB_H - 32, msg, &fonts[2], 0xFFFFFF, false);
    } else {
        ui_text(&c, 20, FB_H - 16, "escuta passiva - nao transmite - MENU volta", &fonts[1], DIM, false);
    }
    display_present(0, FB_H, fb);
}

void wifitools_deauth_detector(void) {
    if (fb == NULL) fb = heap_caps_malloc(FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    total_count = second_count = 0;
    last_channel = 0;
    history_len = 0;
    memset(history, 0, sizeof(history));

    // Enter monitor mode: drop the STA link and listen, hopping channels
    esp_wifi_disconnect();
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(sniffer_cb);
    esp_wifi_set_promiscuous(true);

    term_pause(true);
    term_hide_bar(true);

    int channel = 1;
    int64_t last_second = esp_timer_get_time();
    int per_sec = 0;
    draw(0, false);

    bool run = true;
    while (run) {
        InputEvent event;
        if (input_wait(&event, HOP_MS)) {
            if (event.type == EV_BACK) run = false;
            else if (event.type == EV_KEY && (event.key == 0x1b || event.key == 'q')) {
                // consume a possible arrow sequence after Esc
                char next;
                if (event.key == 0x1b && input_read(&next, 20) && (next == '[' || next == 'O')) {
                    input_read(&next, 20);
                } else {
                    run = false;
                }
            }
            if (!run) break;
        }

        // Hop to the next channel
        channel = channel % CHANNELS + 1;
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

        // Once per second: roll the graph
        int64_t now = esp_timer_get_time();
        if (now - last_second >= 1000000) {
            last_second = now;
            per_sec = second_count;
            second_count = 0;
            push_history(per_sec);
            if (per_sec >= ALERT_PER_SEC) sound_play(SOUND_ERROR);
        }
        draw(per_sec, per_sec >= ALERT_PER_SEC);
    }

    // Leave monitor mode and reconnect to the saved network
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    term_hide_bar(false);
    term_pause(false);
    wifi_reconnect();
}
