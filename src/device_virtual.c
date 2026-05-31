#include <stdio.h>
#include "device.h"

void dev_init(void) {
    printf("[裝置] 虛擬裝置已就緒(七段/LED/蜂鳴器以文字模擬)\n");
}
void dev_show_number(int value) { printf("    [七段顯示器] => %d\n", value); }
void dev_set_led(int on)        { printf("    [警報LED]    => %s\n", on ? "亮" : "滅"); }
void dev_set_buzzer(int on)     { printf("    [蜂鳴器]     => %s\n", on ? "響" : "停"); }
