# 智慧倉庫即時管理系統

NYCU RTOS 期末專題 — Group 4

> 一套跑在 Raspberry Pi 4 上的多執行緒即時倉庫管理系統，結合 RFID 打卡上下班、GPIO 硬體控制、TCP 多人連線、優先權排程與即時視覺化監控。

---

## 系統概述

本系統模擬一座智慧倉庫的完整運作流程：

- **員工打卡上班**後系統才開始處理進出貨；無人上班則拒絕所有操作
- **2 位員工**可同時上班，各自從優先權佇列中搶工作**平行處理**
- 每種貨物有不同的**備貨時間**與**優先權**，佇列自動排序
- 庫存低於門檻時觸發 **LED + 蜂鳴器警報**，需實體按鈕解除
- 雙七段顯示器即時顯示最近異動的分類數量
- 遠端 Client 透過 TCP 連線操作；VM 上的 Python 視覺化即時呈現倉庫狀態

---

## 架構圖

```
┌─────────────────────────────────────────────────────┐
│                   Raspberry Pi 4                     │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ Scanner  │  │ Scanner  │  │   Socket Task    │   │
│  │ (PN532)  │  │ (RC522)  │  │  select() 多工   │   │
│  │ reader=0 │  │ punch 2  │  │  port 9000       │   │
│  └────┬─────┘  └────┬─────┘  └────────┬─────────┘   │
│       │              │                 │              │
│       ▼              ▼                 ▼              │
│  ┌─────────────────────────────────────────────┐     │
│  │          worker_punch(i) 打卡閘門            │     │
│  │   shift_lock + condition variable            │     │
│  │   on_duty[] / clockin_sec[] / clockout_req[] │     │
│  └──────────────┬──────────────┬────────────────┘     │
│                 │              │                       │
│          ┌──────▼──────┐ ┌────▼────────┐              │
│          │ Inventory-1 │ │ Inventory-2 │              │
│          │ (員工1)     │ │ (員工2)     │              │
│          └──────┬──────┘ └──────┬──────┘              │
│                 │               │                      │
│          ┌──────▼───────────────▼──────┐               │
│          │  Priority Queue (scan_q)    │               │
│          │  inv_lock (PRIO_INHERIT)    │               │
│          │  inventory[] + reserved[]   │               │
│          └──────┬──────────────────────┘               │
│                 │                                      │
│     ┌───────────┼───────────┬────────────┐             │
│     ▼           ▼           ▼            ▼             │
│  ┌──────┐  ┌────────┐  ┌────────┐  ┌─────────┐       │
│  │Alert │  │Display │  │Button  │  │GPIO     │       │
│  │Task  │  │Task    │  │Task    │  │LED/Buzz │       │
│  │HIGH  │  │LOW     │  │HIGH    │  │7-seg x2 │       │
│  └──────┘  └────────┘  └────────┘  └─────────┘       │
└─────────────────────────────────────────────────────┘

         ┌──────────────────┐
         │ VM (Ubuntu 22.04)│
         │                  │
         │  warehouse_view  │◄── TCP status 輪詢
         │  (tkinter 視覺化) │
         │                  │
         │  nc / Client     │◄── TCP 操作
         └──────────────────┘
```

---

## 任務表

| 任務 | 優先權 | 排程 | 功能 |
|------|--------|------|------|
| Alert | HIGH (80) | SCHED_FIFO | 接收警報事件 → 亮 LED + 響蜂鳴器 |
| Button | HIGH (80) | SCHED_FIFO | 偵測 GPIO4 按鈕 → 解除警報 |
| Scanner-0 | MEDIUM (50) | SCHED_FIFO | PN532 偵測刷卡 → 員工1 打卡 |
| Inventory-1 | MEDIUM (50) | SCHED_FIFO | 員工1 從佇列取工作 → 備貨 → 提交 |
| Inventory-2 | MEDIUM (50) | SCHED_FIFO | 員工2 從佇列取工作 → 備貨 → 提交 |
| Socket | MEDIUM (50) | SCHED_FIFO | select() 多工 TCP 伺服器 |
| Display | LOW (20) | SCHED_FIFO | 驅動雙七段顯示器 |

---

## 貨物資訊

| 貨物 | 代碼 | 優先權 | 門檻 | 備貨時間 | 初始庫存 |
|------|------|--------|------|----------|----------|
| 醫療物資 | med | 3（最高） | 3 | 3 秒 | 5 |
| 生鮮食品 | food | 2 | 3 | 5 秒 | 5 |
| 一般貨物 | goods | 1（最低） | 3 | 8 秒 | 5 |

---

## 打卡機制

- **上班**：刷卡第一次 → 員工開始從佇列取工作處理
- **下班**：再刷一次 → 需上班滿 **15 秒**才接受；正在備貨會**等該筆做完**才下班
- **0 人上班**：所有進出貨指令被拒絕
- **1 人上班**：單人序列處理
- **2 人上班**：兩位員工平行搶佇列處理

| 讀卡機 | 晶片 | 通訊 | 對應員工 |
|--------|------|------|----------|
| PN532 | NXP PN532 | UART (/dev/serial0) | 員工1（C 內建 libnfc） |
| RC522 | NXP MFRC522 | SPI (/dev/spidev0.0) | 員工2（外部 Python → punch 2） |

> 沒接到讀卡機時可用 socket 指令 `punch 1` / `punch 2` 模擬打卡

---

## 同步機制

| 機制 | 用途 |
|------|------|
| `inv_lock`（mutex + PRIO_INHERIT） | 保護庫存陣列臨界區，防優先權反轉 |
| `shift_lock` + `shift_cv`（cond var） | 員工上班閘門：沒打卡 → 阻塞等待 |
| `alert_not_empty`（cond var） | 警報通道：Alert Task 等待事件 |
| `pq_pop_priority_timed`（cond var + timeout） | 佇列取工作，0.3 秒逾時回頭檢查下班 |
| `wstate_lock`（mutex） | 保護員工備貨狀態（給 status/視覺化） |
| 計數號誌（mempool） | 訊息記憶體池管理 |
| `reserved_out[]` 保留機制 | 鎖內保留 → 鎖外備貨 → 鎖內提交，防超賣 |

---

## 硬體接線（GPIO BCM）

### 基本元件

| 元件 | GPIO | 實體腳 | 備註 |
|------|------|--------|------|
| LED | 17 | pin 11 | 高電位亮 |
| 蜂鳴器 | 27 | pin 13 | 高電位響 |
| 按鈕 | 4 | pin 7 | 10kΩ 上拉至 3.3V，按下為 0 |

### 七段顯示器 #1（個位）— 共陰

| 段 | a | b | c | d | e | f | g |
|----|---|---|---|---|---|---|---|
| GPIO | 5 | 6 | 13 | 19 | 26 | 12 | 16 |

### 七段顯示器 #2（十位）— 共陰

| 段 | a | b | c | d | e | f | g |
|----|---|---|---|---|---|---|---|
| GPIO | 22 | 23 | 24 | 21 | 20 | 7 | 18 |

> 原本 GPIO25/GPIO8 讓給 RC522 SPI，改用 GPIO21/GPIO20

### PN532（UART → 員工1）

| PN532 | Pi | 備註 |
|-------|-----|------|
| 5V | pin 2 (5V) | PN532 需 5V 供電 |
| GND | pin 6 | |
| TX | pin 10 (GPIO15 RXD) | 交叉接 |
| RX | pin 8 (GPIO14 TXD) | 交叉接 |

### RC522（SPI → 員工2）

| RC522 | Pi | 備註 |
|-------|-----|------|
| 3.3V | pin 1 (3V3) | RC522 只能 3.3V |
| GND | pin 6 | |
| NSS | pin 24 (GPIO8 CE0) | 片選 |
| SCK | pin 23 (GPIO11) | |
| MOSI | pin 19 (GPIO10) | |
| MISO | pin 21 (GPIO9) | |
| RST | pin 22 (GPIO25) | |
| IRQ | 不接 | |

---

## 檔案結構

```
warehouse/
├── src/
│   ├── main.c              # 主程式（2-worker 打卡版）
│   ├── pqueue.h             # 優先權佇列宣告
│   ├── pqueue.c             # 優先權佇列實作（含 timed pop）
│   ├── message.h            # 訊息結構定義
│   ├── mempool.h            # 記憶體池宣告
│   ├── mempool.c            # 記憶體池實作（計數號誌）
│   ├── device.h             # 裝置介面宣告
│   ├── device_virtual.c     # VM 空殼版（無硬體）
│   └── device_gpio.c        # Pi 實體版（sysfs + libnfc）
├── tools/
│   ├── warehouse_view.py    # tkinter 即時視覺化（VM 上跑）
│   └── rc522_punch.py       # RC522 讀卡 → punch 2（Pi 上跑）
├── Makefile                 # DEVICE=virtual / DEVICE=gpio
├── DEMO_SCRIPT.md           # Demo 腳本
└── README.md                # 本文件
```

---

## 編譯與執行

### VM（開發 / 測試邏輯）

```bash
make                     # 預設 DEVICE=virtual
sudo ./warehouse
```

### Pi（實體部署）

```bash
# 事前（僅首次）
sudo raspi-config        # 開啟 Serial Port（停用 console、啟用硬體）
# /boot/firmware/config.txt 加：
#   dtparam=spi=on
#   dtoverlay=spi0-1cs
sudo reboot
sudo apt install libnfc-bin libnfc-dev
sudo pip3 install mfrc522 --break-system-packages

# 編譯
make clean && make DEVICE=gpio

# 啟動（需 root 取得 SCHED_FIFO 權限 + GPIO）
sudo ./warehouse

# 另開終端跑 RC522 讀卡程式
python3 rc522_punch.py
```

### 視覺化（VM 上跑，指向 Pi IP）

```bash
python3 tools/warehouse_view.py 192.168.222.222
```

### 遠端操作

```bash
nc 192.168.222.222 9000
```

---

## Socket 指令

| 指令 | 說明 |
|------|------|
| `query` | 查詢庫存（人類可讀格式） |
| `status` | 機器可讀狀態（視覺化用） |
| `in med/food/goods N` | 進貨 N 個 |
| `out med/food/goods N` | 出貨 N 個 |
| `punch 1` | 模擬員工1 刷卡打卡 |
| `punch 2` | 模擬員工2 刷卡打卡 |
| `quit` | 斷線 |

---

## status 輸出格式

```
LOCK 1                          # 1=無人上班(等同鎖定) / 0=有人上班
ITEM med 5 3 0                  # 代碼 現有 門檻 保留出貨
ITEM food 5 3 0
ITEM goods 5 3 0
WORKER 1 off - - 0 0            # off=下班 / idle=待命 / busy=備貨中
WORKER 2 busy med out 2.3 3     # 代碼 進出 剩餘秒 總秒
```

---

## OS 概念對應

| 課程概念 | 實作位置 |
|----------|----------|
| pthread + SCHED_FIFO | 所有 Task（7 條執行緒） |
| Mutex + PTHREAD_PRIO_INHERIT | inv_lock（庫存臨界區） |
| Condition Variable | alert_not_empty, shift_cv, pq not_empty |
| 計數號誌 (Semaphore) | mempool（訊息池管理） |
| SIGINT / SIGPIPE | 優雅關閉 / 忽略斷線訊號 |
| select() I/O 多工 | socket_task（多 client 同時連線） |
| Priority Queue | pq_pop_priority（貨物優先權排序） |
| 優先權反轉防護 | PTHREAD_PRIO_INHERIT |
| GPIO sysfs | LED、蜂鳴器、按鈕、雙七段顯示器 |
| libnfc (UART) | PN532 讀卡（員工1 打卡） |
| SPI + 外部程式 | RC522 讀卡 → punch 2（員工2 打卡） |

---

## 組員

| 姓名 | 學號 |
|------|------|
| 李澤言 | |
| 林彥兆 | |
| 洪珮珈 | |

---

## 授權

本專題為 NYCU RTOS 課程作業，僅供學術用途。
