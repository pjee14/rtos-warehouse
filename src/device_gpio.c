#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define GPIO_BASE 0  // 使用 /dev/gpiomem 時，offset 從 0 開始

static volatile uint32_t *gpio_map;

// 樹莓派 GPIO 暫存器偏移量 (以 32-bit word 為單位)
#define GPFSEL(pin)   ((pin) / 10)
#define GPSET0        7   // Offset 0x1C
#define GPCLR0        10  // Offset 0x28

// 定義硬體腳位 (BCM GPIO 編號)
#define LED_PIN       17
#define BUZZER_PIN    27
// 七段顯示器 A~G 段對應的 GPIO 腳位
static const int SEG_PINS[7] = {5, 6, 13, 19, 26, 12, 16};

// 七段顯示器數字編碼表 (0-9)
// 順序: A, B, C, D, E, F, G (共陰極：1 為亮，0 為暗)
static const uint8_t NUM_MAP[10][7] = {
    {1, 1, 1, 1, 1, 1, 0}, // 0
    {0, 1, 1, 0, 0, 0, 0}, // 1
    {1, 1, 0, 1, 1, 0, 1}, // 2
    {1, 1, 1, 1, 0, 0, 1}, // 3
    {0, 1, 1, 0, 0, 1, 1}, // 4
    {1, 0, 1, 1, 0, 1, 1}, // 5
    {1, 0, 1, 1, 1, 1, 1}, // 6
    {1, 1, 1, 0, 0, 0, 0}, // 7
    {1, 1, 1, 1, 1, 1, 1}, // 8
    {1, 1, 1, 1, 0, 1, 1}  // 9
};

// 設定腳位為輸出模式
static void set_pin_output(int pin) {
    int reg = GPFSEL(pin);
    int shift = (pin % 10) * 3;
    gpio_map[reg] &= ~(7 << shift); // 先清空 3 bits
    gpio_map[reg] |=  (1 << shift); // 設為 Output (001)
}

// 設定腳位高低電位
static void set_pin_val(int pin, int val) {
    if (val) gpio_map[GPSET0] = (1 << pin);
    else     gpio_map[GPCLR0] = (1 << pin);
}

// 實作 device.h 的介面
int dev_init(void) {
    int mem_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("無法開啟 /dev/gpiomem (請加 sudo 執行)");
        return -1;
    }
    
    // MMIO 映射
    gpio_map = (uint32_t *)mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, GPIO_BASE);
    close(mem_fd);
    
    if (gpio_map == MAP_FAILED) {
        perror("MMAP 失敗");
        return -1;
    }

    // 初始化所有腳位為輸出，並預設關閉
    set_pin_output(LED_PIN);
    set_pin_output(BUZZER_PIN);
    set_pin_val(LED_PIN, 0);
    set_pin_val(BUZZER_PIN, 0);

    for (int i = 0; i < 7; i++) {
        set_pin_output(SEG_PINS[i]);
        set_pin_val(SEG_PINS[i], 0);
    }
    
    printf("[硬體層] GPIO Memory-Mapped I/O 初始化成功！\n");
    return 0;
}

void dev_show_number(int num) {
    if (num < 0 || num > 9) return;
    
    // 輸出七段顯示器信號
    for(int i = 0; i < 7; i++) {
        set_pin_val(SEG_PINS[i], NUM_MAP[num][i]);
    }
}

void dev_set_led(int state) {
    set_pin_val(LED_PIN, state ? 1 : 0);
}

void dev_set_buzzer(int state) {
    // 如果沒有實體蜂鳴器，可以先把這行註解掉，或接在 GPIO 27 測試
    set_pin_val(BUZZER_PIN, state ? 1 : 0);
}
