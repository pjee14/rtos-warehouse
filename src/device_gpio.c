/* device_gpio.c — 真實 GPIO (Raspberry Pi, 透過 sysfs,無需任何函式庫) */
#include "device.h"
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

/* ---- GPIO 腳位 (BCM 編號) ---- */
#define LED_PIN     17
#define BUZZER_PIN  27
static const int seg_pins[7] = {5, 6, 13, 19, 26, 12, 16};  /* a,b,c,d,e,f,g */
#define DP_PIN      20      /* 小數點(選配):總數 >=10 時點亮當「十位以上」指示 */
static const int seg2_pins[7] = {22, 23, 24, 25, 8, 7, 18};  /* 第二顆 a,b,c,d,e,f,g */
#define DP2_PIN 21

/* 共陰極:1=亮。若你的七段是共陽極,把這兩個對調即可 */
#define SEG_ON  1
#define SEG_OFF 0

#define BUTTON_PIN 4          /* 實體 pin 7 */
static int btn_fd = -1;

static void gpio_dir_in(int pin) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", pin);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "in"); fclose(f); }
}
static void gpio_edge(int pin, const char *e) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/edge", pin);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%s", e); fclose(f); }
}

void dev_wait_button(void) {
    if (btn_fd < 0) { sleep(1); return; }
    char buf[8];
    struct pollfd pfd = { .fd = btn_fd, .events = POLLPRI | POLLERR };
    lseek(btn_fd, 0, SEEK_SET); if (read(btn_fd, buf, sizeof(buf)) < 0) {}
    poll(&pfd, 1, -1);                 /* 阻塞等下降緣 → 等同中斷 */
    lseek(btn_fd, 0, SEEK_SET); if (read(btn_fd, buf, sizeof(buf)) < 0) {}
    usleep(30000);                     /* 去彈跳 */
}

static const int seg_code[10][7] = {   /* 沿用你 HW1 的 7 段碼 */
    {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1},
    {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
};

static void gpio_export(int pin) {
    FILE *f = fopen("/sys/class/gpio/export", "w");
    if (f) { fprintf(f, "%d", pin); fclose(f); }
}
static void gpio_dir_out(int pin) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", pin);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "out"); fclose(f); }
}
static void gpio_write(int pin, int v) {
    char p[64]; snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", pin);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d", v); fclose(f); }
}

void dev_init(void) {
    gpio_export(LED_PIN); gpio_export(BUZZER_PIN); gpio_export(DP_PIN);
    for (int i = 0; i < 7; i++) gpio_export(seg_pins[i]);
    gpio_export(DP2_PIN);
    for (int i = 0; i < 7; i++) gpio_export(seg2_pins[i]);
    usleep(200000);     /* 等 sysfs 建立檔案 */
        gpio_dir_out(DP2_PIN);
    for (int i = 0; i < 7; i++) gpio_dir_out(seg2_pins[i]);
    gpio_write(DP2_PIN, 0);
    for (int i = 0; i < 7; i++) gpio_write(seg2_pins[i], SEG_OFF);
    gpio_dir_out(LED_PIN); gpio_dir_out(BUZZER_PIN); gpio_dir_out(DP_PIN);
    for (int i = 0; i < 7; i++) gpio_dir_out(seg_pins[i]);
    gpio_write(LED_PIN, 0); gpio_write(BUZZER_PIN, 0); gpio_write(DP_PIN, 0);
    for (int i = 0; i < 7; i++) gpio_write(seg_pins[i], SEG_OFF);
    gpio_export(BUTTON_PIN);
    usleep(200000);
    gpio_dir_in(BUTTON_PIN);
    gpio_edge(BUTTON_PIN, "falling");  /* 按下接地 → 下降緣 */
    {
        char vp[64]; snprintf(vp, sizeof(vp), "/sys/class/gpio/gpio%d/value", BUTTON_PIN);
        btn_fd = open(vp, O_RDONLY);
    }
    printf("[GPIO] sysfs 初始化完成\n");
}

void dev_set_led(int on)    { gpio_write(LED_PIN,    on ? 1 : 0); }
void dev_set_buzzer(int on) { gpio_write(BUZZER_PIN, on ? 1 : 0); }

void dev_show_number(int n) {
    printf("[七段] 最近異動分類數量 %d(十位+個位)\n", n);
    if (n < 0)  n = 0;
    if (n > 99) n = 99;                 /* 兩顆只能到兩位數 */
    int tens  = n / 10;
    int units = n % 10;

    /* 第一顆 = 個位 */
    for (int i = 0; i < 7; i++)
        gpio_write(seg_pins[i],  seg_code[units][i] ? SEG_ON : SEG_OFF);

    /* 第二顆 = 十位(十位是 0 就不顯示,消除前導零,例如 5 顯示「 5」而非「05」) */
    for (int i = 0; i < 7; i++) {
        int on = (tens > 0) && seg_code[tens][i];
        gpio_write(seg2_pins[i], on ? SEG_ON : SEG_OFF);
    }
}
