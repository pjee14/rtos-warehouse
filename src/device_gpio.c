/* device_gpio.c — 真實 GPIO 實作 (Raspberry Pi 4, libgpiod v2) */
#include "device.h"
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHIP_PATH  "/dev/gpiochip0"   /* Pi 3/4 主排針 */
#define LED_PIN    17                 /* 實體 pin 11 */
#define BUZZER_PIN 27                 /* 實體 pin 13 */

static struct gpiod_chip         *chip = NULL;
static struct gpiod_line_request *req  = NULL;

void dev_init(void) {
    chip = gpiod_chip_open(CHIP_PATH);
    if (!chip) { perror("gpiod_chip_open"); exit(1); }

    /* 設定:輸出方向、初始低電位 */
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    /* 把 LED 與蜂鳴器兩支腳一起納入同一份請求 */
    unsigned int offsets[] = { LED_PIN, BUZZER_PIN };
    struct gpiod_line_config *lcfg = gpiod_line_config_new();
    gpiod_line_config_add_line_settings(lcfg, offsets, 2, settings);

    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rcfg, "warehouse");

    req = gpiod_chip_request_lines(chip, rcfg, lcfg);
    if (!req) { perror("gpiod_chip_request_lines"); exit(1); }

    gpiod_request_config_free(rcfg);
    gpiod_line_config_free(lcfg);
    gpiod_line_settings_free(settings);

    printf("[GPIO] 初始化完成 (LED=GPIO%d, Buzzer=GPIO%d)\n", LED_PIN, BUZZER_PIN);
}

void dev_set_led(int on) {
    if (!req) return;
    gpiod_line_request_set_value(req, LED_PIN,
        on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

void dev_set_buzzer(int on) {
    if (!req) return;
    gpiod_line_request_set_value(req, BUZZER_PIN,
        on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

void dev_show_number(int n) {
    /* 七段尚未接線:暫時印終端機,Phase 3 再換成真實七段驅動 */
    printf("[七段] 全倉總數 %d\n", n);
}
