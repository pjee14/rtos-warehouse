#include <stdio.h>
#include "device.h"
#include <stdio.h>
void dev_wait_button(void) {
    /* 虛擬版:在 server 終端機按 Enter 模擬按鈕 */
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
}
void dev_init(void) { /* 虛擬裝置:免初始化 */ }

void dev_show_number(int value) { printf("  [七段] 庫存總數 %d\n", value); }

/* 實體版才控制 GPIO;虛擬版靜默,避免洗版 */
void dev_set_led(int on)    { (void)on; }
void dev_set_buzzer(int on) { (void)on; }



//void dev_init(void) {
//    printf("[裝置] 虛擬裝置已就緒(七段/LED/蜂鳴器以文字模擬)\n");
//}
//void dev_show_number(int value) { printf("    [七段顯示器] => %d\n", value); }
//void dev_set_led(int on)        { printf("    [警報LED]    => %s\n", on ? "亮" : "滅"); }
//void dev_set_buzzer(int on)     { printf("    [蜂鳴器]     => %s\n", on ? "響" : "停"); }
