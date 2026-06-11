/* device_gpio.c — Raspberry Pi 實體版(sysfs GPIO + PN532 libnfc)
 * 介面對應 device.h:
 *   dev_init, dev_set_led, dev_set_buzzer, dev_show_number,
 *   dev_wait_button, dev_card_count, dev_wait_card(reader)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <nfc/nfc.h>
#include "device.h"

/* ===== 腳位(BCM) ===== */
#define LED_PIN    17
#define BUZZER_PIN 27
#define BUTTON_PIN 4

/* 七段#1(個位)a–g */
static const int seg_pins[7]  = { 5, 6, 13, 19, 26, 12, 16 };
/* 七段#2(十位)a–g:25→21、8→20,把 SPI 的 CE0/RST 讓給 RC522 */
static const int seg2_pins[7] = { 22, 23, 24, 21, 20, 7, 18 };

/* 共陰七段:段亮 = 輸出 1 */
#define SEG_ON  1
#define SEG_OFF 0
static const int seg_code[10][7] = {
    {1,1,1,1,1,1,0}, /* 0 */
    {0,1,1,0,0,0,0}, /* 1 */
    {1,1,0,1,1,0,1}, /* 2 */
    {1,1,1,1,0,0,1}, /* 3 */
    {0,1,1,0,0,1,1}, /* 4 */
    {1,0,1,1,0,1,1}, /* 5 */
    {1,0,1,1,1,1,1}, /* 6 */
    {1,1,1,0,0,0,0}, /* 7 */
    {1,1,1,1,1,1,1}, /* 8 */
    {1,1,1,1,0,1,1}, /* 9 */
};

/* ===== sysfs GPIO 小工具(fopen/fprintf,免外部函式庫) ===== */
static void gpio_export(int pin) {
    FILE *f = fopen("/sys/class/gpio/export", "w");
    if (f) { fprintf(f, "%d", pin); fclose(f); }
    usleep(100000);                       /* 等 udev 建好節點 */
}
static void gpio_dir(int pin, const char *dir) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", pin);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s", dir); fclose(f); }
}
static void gpio_write(int pin, int v) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", pin);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d", v ? 1 : 0); fclose(f); }
}
static int gpio_read(int pin) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", pin);
    FILE *f = fopen(p, "r");
    if (!f) return 1;
    int v = 1; if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v;
}

/* ===== PN532(libnfc,只有一台 → reader0 = 員工1) ===== */
static nfc_context *nfc_ctx    = NULL;
static nfc_device  *nfc_dev[1] = { NULL };
static int          nfc_n      = 0;
void dev_init(void) {
    /* LED / 蜂鳴器 / 按鈕 */
    gpio_export(LED_PIN);    gpio_dir(LED_PIN,    "out"); gpio_write(LED_PIN,    0);
    gpio_export(BUZZER_PIN); gpio_dir(BUZZER_PIN, "out"); gpio_write(BUZZER_PIN, 0);
    gpio_export(BUTTON_PIN); gpio_dir(BUTTON_PIN, "in");

    /* 雙七段 */
    for (int i = 0; i < 7; i++) { gpio_export(seg_pins[i]);  gpio_dir(seg_pins[i],  "out"); gpio_write(seg_pins[i],  SEG_OFF); }
    for (int i = 0; i < 7; i++) { gpio_export(seg2_pins[i]); gpio_dir(seg2_pins[i], "out"); gpio_write(seg2_pins[i], SEG_OFF); }

    /* PN532 */
    nfc_init(&nfc_ctx);
    nfc_n = 0;
    if (nfc_ctx) {
        nfc_connstring cs;
        snprintf(cs, sizeof(cs), "pn532_uart:/dev/serial0");
        nfc_device *d = nfc_open(nfc_ctx, cs);
        if (d && nfc_initiator_init(d) >= 0) {
            nfc_dev[0] = d; nfc_n = 1;
            printf("[NFC] PN532 就緒 (%s)\n", cs);
        } else {
            if (d) nfc_close(d);
            printf("[NFC] 找不到 PN532(員工1 可用 punch 1 模擬)\n");
        }
    }
}

void dev_set_led(int on)    { gpio_write(LED_PIN,    on ? 1 : 0); }
void dev_set_buzzer(int on) { gpio_write(BUZZER_PIN, on ? 1 : 0); }

/* 顯示 0–99:個位在七段#1,十位在七段#2(十位為 0 則熄滅) */
void dev_show_number(int n) {
    if (n < 0)  n = 0;
    if (n > 99) n = 99;
    int units = n % 10, tens = n / 10;
    for (int i = 0; i < 7; i++) gpio_write(seg_pins[i],  seg_code[units][i] ? SEG_ON : SEG_OFF);
    if (tens == 0)
        for (int i = 0; i < 7; i++) gpio_write(seg2_pins[i], SEG_OFF);          /* 前導零熄滅 */
    else
        for (int i = 0; i < 7; i++) gpio_write(seg2_pins[i], seg_code[tens][i] ? SEG_ON : SEG_OFF);
}

/* 按鈕:上拉,平時讀 1,按下為 0;偵測 1→0 並 30ms 去彈跳 */
void dev_wait_button(void) {
    int prev = 1;
    while (1) {
        int v = gpio_read(BUTTON_PIN);
        if (prev == 1 && v == 0) {
            usleep(30000);
            if (gpio_read(BUTTON_PIN) == 0) return;   /* 確認真的按下 */
        }
        prev = v;
        usleep(5000);
    }
}

/* ===== 讀卡機 ===== */
int dev_card_count(void) { return nfc_n; }

int dev_wait_card(int reader) {
    if (reader != 0 || !nfc_dev[0]) { sleep(1); return 0; }   /* 只有 reader0 = PN532 */
    const nfc_modulation nm = { .nmt = NMT_ISO14443A, .nbr = NBR_106 };
    nfc_target nt;
    while (1) {
        if (nfc_initiator_select_passive_target(nfc_dev[0], nm, NULL, 0, &nt) > 0)
            return 1;                       /* 偵測到任何卡 = 員工1 打卡 */
        usleep(200000);
    }
}
