#!/usr/bin/env python3
# 智慧倉庫即時監控(加強版)
#   立體紙箱 + 三員工備貨進度 + 刷卡鎖定指示 + 動畫
# 用法:python3 warehouse_view.py [伺服器IP]   (預設 127.0.0.1)
import socket, sys, time, math
import tkinter as tk

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 9000
W, H = 900, 720
FLOOR = 440

# 顯示順序與中文名
ITEM_ORDER = [("med", "醫療用品"), ("food", "生鮮食品"), ("goods", "一般貨物")]
RED   = "#d9352c"
GREEN = "#3a9a4a"
AMBER = "#d98a2b"
BG_TOP, BG_BOT = "#2a2a40", "#181826"


class WarehouseView:
    def __init__(self, root):
        self.root = root
        root.title("智慧倉庫即時監控")
        self.canvas = tk.Canvas(root, width=W, height=H, bg=BG_BOT, highlightthickness=0)
        self.canvas.pack()
        self.sock = None
        self.connected = False
        self.data = {"locked": True, "items": {}, "workers": []}
        self.data_time = time.time()
        self.prev = {}            # code -> 上次數量
        self.floats = []          # 浮動 +N/-N 文字
        self.disp_total = 0.0     # 平滑變化的總數
        self.connect()
        self.poll()               # 每秒抓資料
        self.animate()            # 連續動畫

    # ---------- 連線 / 解析 ----------
    def connect(self):
        try:
            self.sock = socket.create_connection((HOST, PORT), timeout=2)
            self.sock.settimeout(2)
            self.sock.recv(2048)          # 吃掉歡迎訊息
        except Exception:
            self.sock = None

    def fetch(self):
        if not self.sock:
            self.connect()
            if not self.sock:
                return None
        try:
            self.sock.sendall(b"status\n")
            return self.sock.recv(2048).decode("utf-8", "ignore")
        except Exception:
            self.sock = None
            return None

    def parse(self, text):
        d = {"locked": True, "items": {}, "workers": []}
        for line in text.splitlines():
            p = line.split()
            if not p:
                continue
            if p[0] == "LOCK" and len(p) >= 2:
                d["locked"] = (p[1] == "1")
            elif p[0] == "ITEM" and len(p) >= 5:
                d["items"][p[1]] = (int(p[2]), int(p[3]), int(p[4]))   # cur, th, resv
            elif p[0] == "WORKER" and len(p) >= 7:
                d["workers"].append({
                    "id": int(p[1]), "busy": p[2] == "busy",
                    "item": p[3], "action": p[4],
                    "rem": float(p[5]), "prep": int(p[6]),
                })
        return d

    def poll(self):
        text = self.fetch()
        if text:
            new = self.parse(text)
            # 偵測數量變動 → 噴出浮動文字
            xs = {"med": W * 0.26, "food": W * 0.5, "goods": W * 0.74}
            for code, (cur, th, resv) in new["items"].items():
                if code in self.prev and self.prev[code] != cur:
                    dv = cur - self.prev[code]
                    self.floats.append({
                        "x": xs[code], "y": FLOOR - 200,
                        "text": ("+%d" % dv) if dv > 0 else ("%d" % dv),
                        "color": GREEN if dv > 0 else RED, "born": time.time(),
                    })
                self.prev[code] = cur
            self.data = new
            self.data_time = time.time()
            self.connected = True
        else:
            self.connected = False
        self.root.after(1000, self.poll)

    # ---------- 繪圖工具 ----------
    @staticmethod
    def _lerp(c1, c2, t):
        t = max(0.0, min(1.0, t))
        a = (int(c1[1:3], 16), int(c1[3:5], 16), int(c1[5:7], 16))
        b = (int(c2[1:3], 16), int(c2[3:5], 16), int(c2[5:7], 16))
        return "#%02x%02x%02x" % tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

    def vgrad(self, x0, y0, x1, y1, top, bot, n=20):
        step = (y1 - y0) / n
        for i in range(n):
            col = self._lerp(top, bot, i / (n - 1) if n > 1 else 0)
            self.canvas.create_rectangle(x0, y0 + i * step, x1, y0 + (i + 1) * step + 1,
                                         fill=col, outline=col)

    def round_rect(self, x0, y0, x1, y1, r, **kw):
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1,
               x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
        return self.canvas.create_polygon(pts, smooth=True, **kw)

    # ---------- 元件 ----------
    def carton(self, cx, base_y, kind, low, top_lid):
        c = self.canvas
        bw, bh, d = 78, 27, 14
        x0, x1 = cx - bw / 2, cx + bw / 2
        yt, yb = base_y - bh, base_y
        if low:
            ftop, fbot, sidec, lidc, edge = "#b06a52", "#8a4c39", "#763f31", "#c08376", "#5f3225"
        else:
            ftop, fbot, sidec, lidc, edge = "#dab074", "#ad8245", "#946d3b", "#ecc998", "#6f5230"
        c.create_polygon(x1, yt, x1 + d, yt - d, x1 + d, yb - d, x1, yb, fill=sidec, outline=edge)
        if top_lid:
            c.create_polygon(x0, yt, x0 + d, yt - d, x1 + d, yt - d, x1, yt, fill=lidc, outline=edge)
        self.vgrad(x0 + 1, yt + 1, x1 - 1, yb - 1, ftop, fbot, 8)
        c.create_rectangle(x0, yt, x1, yb, outline=edge)
        c.create_line(x0 + 2, yt + 2, x1 - 2, yt + 2, fill=self._lerp(ftop, "#ffffff", 0.35))
        self.icon(cx, (yt + yb) / 2, kind, 1.0)

    def icon(self, cx, cy, kind, s):
        c = self.canvas
        if kind == "med":
            r = 10 * s
            c.create_oval(cx - r, cy - r, cx + r, cy + r, fill="#f5f5f5", outline="#cfcfcf")
            c.create_rectangle(cx - 3 * s, cy - 7 * s, cx + 3 * s, cy + 7 * s, fill=RED, outline="")
            c.create_rectangle(cx - 7 * s, cy - 3 * s, cx + 7 * s, cy + 3 * s, fill=RED, outline="")
        elif kind == "food":
            c.create_oval(cx - 10 * s, cy - 9 * s, cx + 10 * s, cy + 11 * s, fill="#e07d12", outline="#b5640c")
            c.create_oval(cx - 9 * s, cy - 8 * s, cx + 3 * s, cy + 2 * s, fill="#f3a23a", outline="")
            c.create_oval(cx, cy - 14 * s, cx + 8 * s, cy - 9 * s, fill="#3f9b3a", outline="#2c7029")
        else:  # goods:素面箱,畫個十字膠帶
            c.create_line(cx - 9 * s, cy, cx + 9 * s, cy, fill="#7a5a30")
            c.create_line(cx, cy - 9 * s, cx, cy + 9 * s, fill="#7a5a30")

    def beacon(self, cx, cy, on, pulse):
        c = self.canvas
        if on:
            glow = self._lerp("#3a1a14", RED, pulse)
            c.create_oval(cx - 26, cy - 18, cx + 26, cy + 20, outline=glow, width=2)
            for a in range(-2, 3):
                ang = math.radians(90 + a * 28)
                c.create_line(cx + 13 * math.cos(ang), cy - 6 - 6 * abs(a),
                              cx + 24 * math.cos(ang), cy - 14 - 6 * abs(a),
                              fill=self._lerp("#5a4a10", "#ffd23f", pulse), width=2)
        c.create_rectangle(cx - 12, cy + 9, cx + 12, cy + 16, fill="#2e2e38", outline="#15151c")
        dome = self._lerp("#7a2a20", RED, pulse) if on else "#6a6a72"
        c.create_arc(cx - 13, cy - 5, cx + 13, cy + 21, start=0, extent=180,
                     style=tk.PIESLICE, fill=dome, outline="#1a1a1a")
        c.create_text(cx, cy + 28, text="警報器", fill="#cfcfcf", font=("Sans", 9))

    def lock_badge(self, cx, cy, locked):
        c = self.canvas
        col = RED if locked else GREEN
        # 鎖身
        self.round_rect(cx - 16, cy - 2, cx + 16, cy + 22, 5, fill=col, outline="")
        c.create_oval(cx - 3, cy + 7, cx + 3, cy + 13, fill="#1c1c28", outline="")  # 鑰匙孔
        # 鎖環:鎖定=置中閉合,解鎖=偏移打開
        if locked:
            c.create_arc(cx - 11, cy - 20, cx + 11, cy + 2, start=0, extent=180,
                         style=tk.ARC, outline=col, width=4)
        else:
            c.create_arc(cx - 4, cy - 22, cx + 18, cy, start=20, extent=180,
                         style=tk.ARC, outline=col, width=4)
        txt = "已鎖定 — 請刷卡解鎖" if locked else "已解鎖 — 可進出貨"
        c.create_text(cx + 30, cy + 10, text=txt, anchor="w", fill=col, font=("Sans", 14, "bold"))

    def worker_person(self, cx, cy, busy, bob):
        c = self.canvas
        col = "#f0c060" if busy else "#5a5a68"
        y = cy + (bob if busy else 0)
        c.create_oval(cx - 7, y - 30, cx + 7, y - 16, fill=col, outline="")          # 頭
        c.create_polygon(cx - 11, y, cx - 9, y - 15, cx + 9, y - 15, cx + 11, y,
                         fill=col, outline="")                                        # 身體

    def worker_bay(self, cx, cy, w, now):
        c = self.canvas
        busy = w["busy"]
        bw, bh = 250, 150
        x0, y0 = cx - bw / 2, cy - bh / 2
        base = "#26263a" if busy else "#1f1f2c"
        edge = self._lerp("#26263a", "#f0c060", 0.5) if busy else "#33334a"
        self.round_rect(x0, y0, x0 + bw, y0 + bh, 12, fill=base, outline=edge)
        c.create_text(cx, y0 + 18, text="員工 %d" % w["id"], fill="#e8e8f0", font=("Sans", 13, "bold"))

        bob = math.sin(now * 6 + w["id"]) * 2
        self.worker_person(cx - 80, y0 + bh - 30, busy, bob)

        if busy:
            name = {"med": "醫療用品", "food": "生鮮食品", "goods": "一般貨物"}.get(w["item"], w["item"])
            self.icon(cx - 10, y0 + 60, w["item"], 1.1)
            act = "進貨 ▲" if w["action"] == "in" else "出貨 ▼"
            col = GREEN if w["action"] == "in" else AMBER
            c.create_text(cx + 55, y0 + 52, text=name, fill="#e8e8f0", font=("Sans", 11))
            c.create_text(cx + 55, y0 + 70, text=act, fill=col, font=("Sans", 12, "bold"))
            # 即時倒數進度條
            live_rem = max(0.0, w["rem"] - (now - self.data_time))
            prog = 1.0 - (live_rem / w["prep"]) if w["prep"] > 0 else 1.0
            prog = max(0.0, min(1.0, prog))
            bx0, bx1, by = cx - 95, cx + 95, y0 + bh - 26
            self.round_rect(bx0, by, bx1, by + 14, 7, fill="#12121c", outline="#444")
            if prog > 0.02:
                self.round_rect(bx0, by, bx0 + (bx1 - bx0) * prog, by + 14, 7, fill=col, outline="")
            c.create_text(cx, by + 28, text="備貨中… 剩 %.1f 秒" % live_rem,
                          fill="#cfcfcf", font=("Sans", 10))
        else:
            c.create_text(cx + 20, cy, text="待命中", fill="#6a6a7a", font=("Sans", 13))

    # ---------- 主繪製 ----------
    def animate(self):
        now = time.time()
        c = self.canvas
        c.delete("all")

        # 背景 + 後牆格線 + 地板
        self.vgrad(0, 0, W, FLOOR, BG_TOP, BG_BOT, 22)
        for gx in range(0, W, 90):
            c.create_line(gx, 120, gx, FLOOR, fill=self._lerp(BG_TOP, BG_BOT, 0.4))
        self.vgrad(0, FLOOR, W, FLOOR + 60, "#3a3a52", "#262636", 8)
        c.create_line(0, FLOOR, W, FLOOR, fill="#5a5a78")

        if not self.connected:
            c.create_text(W / 2, H / 2, justify="center", fill="white", font=("Sans", 15),
                          text="未連線到倉庫系統\n請先在 Pi 執行  sudo ./warehouse\n(或確認 IP: %s)" % HOST)
            self.root.after(200, self.animate)
            return

        items = self.data["items"]
        any_low = any(v[0] < v[1] for v in items.values()) if items else False
        pulse = (math.sin(now * 5) + 1) / 2

        # 標題 + 平滑總數
        c.create_text(W / 2, 30, text="智慧倉庫即時監控", fill="white", font=("Sans", 21, "bold"))
        total = sum(v[0] for v in items.values()) if items else 0
        self.disp_total += (total - self.disp_total) * 0.2
        c.create_text(W / 2, 62, text="全倉總數：%d" % round(self.disp_total),
                      fill="#f4cf3a", font=("Sans", 14, "bold"))

        # 警報器(脈動)
        self.beacon(58, 44, any_low, pulse)
        self.beacon(W - 58, 44, any_low, pulse)

        # 刷卡鎖定指示
        self.lock_badge(W / 2 - 120, 86, self.data["locked"])

        # 低庫存橫幅
        if any_low:
            self.round_rect(W / 2 - 220, 112, W / 2 + 220, 138, 8,
                            fill=self._lerp("#3a1414", RED, pulse), outline="")
            c.create_text(W / 2, 125, text="⚠ 警報：有貨物庫存低於安全量！",
                          fill="white", font=("Sans", 13, "bold"))

        # 貨物堆疊
        xs = [W * 0.26, W * 0.5, W * 0.74]
        for (code, label), cx in zip(ITEM_ORDER, xs):
            cur, th, resv = items.get(code, (0, 3, 0))
            low = cur < th
            c.create_oval(cx - 52, FLOOR - 6, cx + 52, FLOOR + 10, fill="#141420", outline="")  # 陰影
            vis = min(cur, 7)
            for i in range(vis):
                self.carton(cx, FLOOR - 8 - i * 28, code, low, i == vis - 1)
            if cur > 7:
                c.create_text(cx, FLOOR - 8 - 7 * 28 - 14, text="+%d" % (cur - 7),
                              fill="white", font=("Sans", 11, "bold"))
            c.create_text(cx, FLOOR + 30, text=label, fill="white", font=("Sans", 14, "bold"))
            c.create_text(cx, FLOOR + 52, text="庫存 %d（門檻 %d）" % (cur, th),
                          fill=(RED if low else "#dcdce4"), font=("Sans", 11))

        # 浮動數量變化文字
        alive = []
        for f in self.floats:
            age = now - f["born"]
            if age < 1.2:
                y = f["y"] - age * 40
                col = self._lerp(f["color"], BG_BOT, age / 1.2)
                c.create_text(f["x"], y, text=f["text"], fill=col, font=("Sans", 16, "bold"))
                alive.append(f)
        self.floats = alive

        # 備貨區(三員工)
        c.create_text(W / 2, FLOOR + 78, text="── 備貨區（3 位作業員平行處理）──",
                      fill="#9a9ab0", font=("Sans", 12))
        wbx = [W * 0.2, W * 0.5, W * 0.8]
        workers = self.data["workers"] or [{"id": i + 1, "busy": False, "item": "-",
                                            "action": "-", "rem": 0, "prep": 0} for i in range(3)]
        for w, cx in zip(workers, wbx):
            self.worker_bay(cx, FLOOR + 175, w, now)

        self.root.after(40, self.animate)   # ~25fps


if __name__ == "__main__":
    root = tk.Tk()
    WarehouseView(root)
    root.mainloop()
