# 智慧倉庫系統(RTOS Smart Warehouse)

以 **C + POSIX threads(`SCHED_FIFO` 即時排程)** 實作的多工即時倉儲管理系統。在 Ubuntu VM 上開發,並部署到 Raspberry Pi 4 連接真實硬體(LED、蜂鳴器、按鈕、雙七段顯示器、PN532 RFID)。本專案為 NYCU 嵌入式 / RTOS 課程期末專題。

## 組員

洪珮珈、李澤言、林彥兆

## 系統概念

模擬一個多人同時操作的智慧倉庫:多個遠端使用者透過網路同時進貨 / 出貨,系統以多個即時任務(task)平行處理、依貨物優先權排程、保護共享庫存不被超賣,並透過實體裝置(七段顯示、警報燈、蜂鳴器、按鈕、RFID)與外界互動。

## 架構:任務(Task)與優先權

採用 pthreads + `SCHED_FIFO`,將系統拆解為以下並行任務:

| 任務 | 優先權 | 職責 |
|------|--------|------|
| Alert | 高 (80) | 庫存低於門檻 / 出貨被拒時觸發警報,點亮 LED + 蜂鳴器,**持續到管理員按鈕解除** |
| Button | 高 (80) | 等待實體按鈕(GPIO),按下即解除警報(非同步事件 / 中斷處理) |
| Inventory ×3 | 中 (50) | 三個平行作業員,處理進出貨;備貨時間在鎖外執行,達成真正平行 |
| Socket | 中 (50) | 多人 TCP 伺服器(port 9000),`select()` 同時服務多個 client |
| Display | 低 (20) | 驅動雙七段顯示器,顯示最近一筆異動分類的數量(十位 + 個位) |

## 平行處理與並發安全

- **三個 Inventory worker 平行備貨**:耗時的備貨動作在鎖外執行,三筆訂單(3 + 5 + 8 秒)單一 worker 需 16 秒,三個平行最慢只要 8 秒。
- **保留機制(`reserved_out`)防止超賣**:出貨在鎖內原子地「檢查 + 保留」,避免多個 worker 同時通過檢查導致庫存變負。
- **兩層優先權排程**:任務優先權由 `SCHED_FIFO` 管理;貨物優先權由優先佇列管理(醫療 > 生鮮 > 一般;同品項時,數量較接近現有庫存者先處理)。

## IPC 與同步機制(OS 核心服務)

- 優先權訊息佇列(`scan_q`)
- 優先權繼承互斥鎖(`PTHREAD_PRIO_INHERIT`)保護庫存表,防止優先權反轉
- 條件變數(佇列與警報通道的喚醒)
- 計數號誌(固定區塊記憶體池,取代 `malloc`,O(1) 配置、無碎片)
- 信號(`SIGINT` 優雅關機、忽略 `SIGPIPE`)

## 貨物分級

| 品項 | 指令字 | 優先權 | 安全門檻 | 備貨時間 | 初始庫存 |
|------|--------|--------|----------|----------|----------|
| 醫療物資 | `med` | 3 | 3 | 3 秒 | 5 |
| 生鮮食品 | `food` | 2 | 3 | 5 秒 | 5 |
| 一般貨物 | `goods` | 1 | 3 | 8 秒 | 5 |

## 裝置抽象層(device.h)

上層任務不直接碰硬體,一律透過統一介面操作:
`dev_init()`、`dev_set_led()`、`dev_set_buzzer()`、`dev_show_number()`、`dev_wait_button()`。

- **`device_virtual.c`**:終端機模擬(VM 開發用,無硬體)
- **`device_gpio.c`**:真實 GPIO(Raspberry Pi,透過 sysfs)

同一份上層程式碼,只要在編譯時切換裝置實作即可,不必更動任何任務。

## 硬體接線(Raspberry Pi 4,BCM 編號)

| 功能 | GPIO | 實體 pin |
|------|------|---------|
| LED(警報燈) | 17 | 11 |
| 蜂鳴器 | 27 | 13 |
| 按鈕(解除警報) | 4 | 7 |
| 七段 #1 個位 a~g | 5, 6, 13, 19, 26, 12, 16 | 29, 31, 33, 35, 37, 32, 36 |
| 七段 #2 十位 a~g | 22, 23, 24, 25, 8, 7, 18 | 15, 16, 18, 22, 24, 26, 12 |
| PN532 RFID(UART,進行中) | 14 (TX), 15 (RX) | 8, 10 |

接線注意:
- 每個 LED / 七段段位串接 220–330Ω 限流電阻。
- 按鈕需外接 10kΩ 上拉電阻到 3.3V(平常讀 1、按下接地讀 0)。
- 兩顆七段的共用腳依共陰 / 共陽分別接 GND / 3.3V。
- PN532 以 5V 供電(3.3V 供電不足會隨機重置),走 HSU/UART 模式,TX ↔ RX 交叉。

## 編譯與執行

需 root 權限(`SCHED_FIFO` 即時優先權 + GPIO 存取)。

VM(終端機模擬):

```bash
make
sudo ./warehouse
```

Raspberry Pi(真實硬體):

```bash
make DEVICE=gpio
sudo ./warehouse
```

遠端 client 連線:

```bash
nc <伺服器IP> 9000
```

可用指令:

- `query` — 查詢各分類庫存
- `in med/food/goods 數量` — 進貨
- `out med/food/goods 數量` — 出貨
- `quit` — 離開連線

## 即時視覺化

```bash
sudo apt install python3-tk
python3 tools/warehouse_view.py
```

以 tkinter 連到 port 9000,每秒查詢庫存並繪製倉庫場景(立體紙箱、分類圖示、警報燈)。本質上就是另一個 socket client。

## 檔案結構

```
warehouse/
├── src/
│   ├── main.c            # 任務、優先權、警報、按鈕、Socket、主程式
│   ├── message.h         # 訊息結構與品項型別
│   ├── pqueue.c / .h     # 優先權佇列
│   ├── mempool.c / .h    # 固定區塊記憶體池
│   ├── device.h          # 裝置抽象介面
│   ├── device_virtual.c  # 終端機模擬(VM)
│   └── device_gpio.c     # 真實 GPIO(Pi,sysfs)
├── tools/
│   └── warehouse_view.py # tkinter 即時視覺化
├── Makefile              # make / make DEVICE=gpio 切換虛擬與實體
└── README.md
```

## Demo 情境

1. **平行處理**:三個 client 幾乎同時送 `out goods/food/med 1`,觀察三個員工同時備貨、依秒數先後完成。
2. **優先權排程**:先讓三個員工忙碌,再丟入混合優先序的訂單,觀察醫療物資被優先挑出。
3. **並發安全**:三個 client 同時對同品項超額出貨,1 筆成功、其餘被拒,並觸發警報。
4. **警報與人為介入**:出貨使庫存低於門檻 → LED + 蜂鳴器持續警示 → 管理員按下按鈕解除。
5. **即時查詢**:任一 client 隨時 `query`,展示 Socket 任務獨立運作。

## 對應課程內容

- **多工 / 任務拆分**:五類任務各司其職、各自阻塞於不同事件而平行運作
- **排程與優先權**:`SCHED_FIFO` 兩層優先權
- **IPC**:訊息佇列、互斥鎖、條件變數、號誌
- **中斷 / 非同步事件**:GPIO 按鈕觸發警報解除
- **裝置 I/O**:GPIO 控制 LED / 蜂鳴器 / 雙七段;PN532 RFID 掃描(進行中)
- **Socket 與記憶體管理**:TCP 多人連線、固定區塊記憶體池

## 開發環境

- Ubuntu 22.04(VirtualBox VM)、gcc、pthreads
- Raspberry Pi 4、Raspberry Pi OS、sysfs GPIO、libnfc(RFID)
- Python 3 + tkinter(視覺化)
