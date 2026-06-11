#!/usr/bin/env python3
# 智慧倉庫即時監控（2-worker 打卡版）
#   上下班打卡 + 備貨倒數進度條 + 立體貨物 + 動畫（無鎖頭）
# 用法：python3 warehouse_view.py [伺服器IP]   (預設 127.0.0.1)
import socket, sys, time, math
import tkinter as tk

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 9000
W, H = 900, 720
FLOOR = 430

ITEM_ORDER = [("med", "醫療用品"), ("food", "生鮮食品"), ("goods", "一般貨物")]
NAME_OF = {"med": "醫療用品", "food": "生鮮食品", "goods": "一般貨物"}
RED   = "#d9352c"
GREEN = "#3a9a4a"
AMBER = "#d98a2b"
BLUE  = "#3a78c9"
BG_TOP, BG_BOT = "#2a2a40", "#181826"


class WarehouseView:
    def __init__(self, root):
        self.root = root
        root.title("智慧倉庫即時監控")
        self.canvas = tk.Canvas(root, width=W, height=H, bg=BG_BOT, highlightthickness=0)
        self.canvas.pack()
        self.sock = None
        self.connected = False
        self.data = {"items": {}, "workers": []}
        self.data_time = time.time()
        self.prev = {}             # code -> 上次數量
        self.floats = []           # 浮動 +N/-N
        self.disp_total = 0.0
        self.wprev = {}            # worker id -> 上次 state（偵測打卡瞬間）
        self.wflash = {}           # worker id -> 打卡閃光起始時間
        self.connect()
        self.poll()
        self.animate()

    # ---------- 連線 / 解析 ----------
    def connect(self):
        try:
            self.sock = socket.create_connection((HOST, PORT), timeout=2)
            self.sock.settimeout(2)
            self.sock.recv(2048)
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
        d = {"items": {}, "workers": []}
        for line in text.splitlines():
            p = line.split()
            if not p:
                continue
            if p[0] == "ITEM" and len(p) >= 5:
                d["items"][p[1]] = (int(p[2]), int(p[3]), int(p[4]))
            elif p[0] == "WORKER" and len(p) >= 7:
                d["workers"].append({
                    "id": int(p[1]), "state": p[2],          # off|idle|busy
                    "item": p[3], "action": p[4],
                    "rem": float(p[5]), "prep": int(p[6]),
                })
        return d

    def poll(self):
        text = self.fetch()
        if text and ("ITEM" in text or "WORKER" in text):
            new = self.parse(text)
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
            # 偵測打卡瞬間（state 改變）→ 閃光
            for w in new["workers"]:
                wid = w["id"]
                if wid in self.wprev and self.wprev[wid] != w["state"]:
                    self.wflash[wid] = time.time()
                self.wprev[wid] = w["state"]
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
        else:
            c.create_line(cx - 9 * s, cy, cx + 9 * s, cy, fill="#7a5a30")
            c.create_line(cx, cy - 9 * s, cx, cy + 9 * s, fill="#7a5a30")

    def beacon(self, cx, cy, on, pulse):
        c = self.canvas
        if on:
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

    def person(self, cx, cy, color, bob=0.0):
        c = self.canvas
        y = cy + bob
        c.create_oval(cx - 8, y - 34, cx + 8, y - 18, fill=color, outline="")        # 頭
        c.create_polygon(cx - 12, y, cx - 10, y - 17, cx + 10, y - 17, cx + 12, y,
                         fill=color, outline="")                                      # 身體

    def punch_badge(self, x, y, on):
        c = self.canvas
        col = GREEN if on else "#7a3030"
        self.round_rect(x, y, x + 64, y + 26, 7, fill=col, outline="")
        c.create_oval(x + 7, y + 6, x + 21, y + 20, outline="white", width=2)         # 時鐘
        c.create_line(x + 14, y + 13, x + 14, y + 8, fill="white", width=2)
        c.create_line(x + 14, y + 13, x + 18, y + 15, fill="white", width=2)
        c.create_text(x + 44, y + 13, text="上班" if on else "下班",
                      fill="white", font=("Sans", 11, "bold"))

    def headcount(self, cx, y, on_count, pulse):
        c = self.canvas
        c.create_text(cx - 70, y, text="上班人力", fill="#cfcfcf", font=("Sans", 12), anchor="e")
        for i in range(2):
            x = cx - 40 + i * 40
            lit = i < on_count
            col = self._lerp("#3a3a4a", GREEN, (pulse * 0.4 + 0.6) if lit else 0)
            c.create_oval(x - 13, y - 14, x + 13, y + 14, fill=col, outline="#15151c")
            self.person(x, y + 9, "#f0f4ff" if lit else "#555")
        c.create_text(cx + 60, y, text="%d / 2" % on_count, anchor="w",
                      fill=GREEN if on_count else "#888", font=("Sans", 14, "bold"))

    def worker_bay(self, cx, cy, w, now):
        c = self.canvas
        state = w["state"]
        on = state in ("idle", "busy")
        bw, bh = 300, 170
        x0, y0 = cx - bw / 2, cy - bh / 2

        # 打卡瞬間閃光
        flash = 0.0
        born = self.wflash.get(w["id"])
        if born is not None:
            age = now - born
            if age < 1.0:
                flash = (1.0 - age) * (0.5 + 0.5 * math.sin(now * 20))

        base = {"busy": "#2a2440", "idle": "#23283a", "off": "#1c1c28"}[state]
        edgec = {"busy": AMBER, "idle": BLUE, "off": "#33334a"}[state]
        edgec = self._lerp(edgec, "#ffffff", flash)
        self.round_rect(x0, y0, x0 + bw, y0 + bh, 14, fill=base, outline=edgec)

        c.create_text(x0 + 18, y0 + 20, text="員工 %d" % w["id"], anchor="w",
                      fill="#e8e8f0", font=("Sans", 14, "bold"))
        self.punch_badge(x0 + bw - 80, y0 + 8, on)

        px = cx - 95
        if state == "busy":
            self.person(px, y0 + bh - 28, "#f0c060", math.sin(now * 6 + w["id"]) * 2.5)
            self.icon(cx - 20, y0 + 66, w["item"], 1.1)
            act = "進貨 ▲" if w["action"] == "in" else "出貨 ▼"
            col = GREEN if w["action"] == "in" else AMBER
            c.create_text(cx + 50, y0 + 56, text=NAME_OF.get(w["item"], w["item"]),
                          fill="#e8e8f0", font=("Sans", 12))
            c.create_text(cx + 50, y0 + 76, text=act, fill=col, font=("Sans", 13, "bold"))
            live_rem = max(0.0, w["rem"] - (now - self.data_time))
            prog = 1.0 - (live_rem / w["prep"]) if w["prep"] > 0 else 1.0
            prog = max(0.0, min(1.0, prog))
            bx0, bx1, by = cx - 110, cx + 110, y0 + bh - 30
            self.round_rect(bx0, by, bx1, by + 15, 7, fill="#12121c", outline="#444")
            if prog > 0.02:
                self.round_rect(bx0, by, bx0 + (bx1 - bx0) * prog, by + 15, 7, fill=col, outline="")
            c.create_text(cx, by + 30, text="備貨中… 剩 %.1f 秒" % live_rem,
                          fill="#dcdce4", font=("Sans", 11))
        elif state == "idle":
            self.person(px, y0 + bh - 28, "#cdd6e8")
            c.create_text(cx + 25, cy + 6, text="上班中 · 待命", fill=BLUE, font=("Sans", 14, "bold"))
        else:  # off
            self.person(px, y0 + bh - 28, "#555")
            c.create_text(px + 26, y0 + bh - 56, text="z", fill="#6a6a7a", font=("Sans", 12, "italic"))
            c.create_text(cx + 25, cy + 6, text="已下班", fill="#7a7a8a", font=("Sans", 14, "bold"))

    # ---------- 主繪製 ----------
    def animate(self):
        now = time.time()
        c = self.canvas
        c.delete("all")

        self.vgrad(0, 0, W, FLOOR, BG_TOP, BG_BOT, 22)
        for gx in range(0, W, 90):
            c.create_line(gx, 130, gx, FLOOR, fill=self._lerp(BG_TOP, BG_BOT, 0.4))
        self.vgrad(0, FLOOR, W, FLOOR + 60, "#3a3a52", "#262636", 8)
        c.create_line(0, FLOOR, W, FLOOR, fill="#5a5a78")

        if not self.connected:
            c.create_text(W / 2, H / 2, justify="center", fill="white", font=("Sans", 15),
                          text="未連線到倉庫系統\n請先在 Pi 執行  sudo ./warehouse\n(或確認 IP: %s)" % HOST)
            self.root.after(200, self.animate)
            return

        items = self.data["items"]
        workers = self.data["workers"] or [{"id": i + 1, "state": "off", "item": "-",
                                            "action": "-", "rem": 0, "prep": 0} for i in range(2)]
        on_count = sum(1 for w in workers if w["state"] in ("idle", "busy"))
        any_low = any(v[0] < v[1] for v in items.values()) if items else False
        pulse = (math.sin(now * 5) + 1) / 2

        # 標題 + 平滑總數
        c.create_text(W / 2, 30, text="智慧倉庫即時監控", fill="white", font=("Sans", 21, "bold"))
        total = sum(v[0] for v in items.values()) if items else 0
        self.disp_total += (total - self.disp_total) * 0.2
        c.create_text(W / 2, 60, text="全倉總數：%d" % round(self.disp_total),
                      fill="#f4cf3a", font=("Sans", 14, "bold"))

        # 警報器
        self.beacon(58, 44, any_low, pulse)
        self.beacon(W - 58, 44, any_low, pulse)

        # 上班人力指示（取代鎖頭）
        self.headcount(W / 2, 92, on_count, pulse)

        # 橫幅：無人上班 / 低庫存
        banner = None
        if on_count == 0:
            banner = ("目前無人上班 — 請刷卡打卡才能進出貨", "#3a2a14", AMBER)
        elif any_low:
            banner = ("⚠ 警報：有貨物庫存低於安全量！", "#3a1414", RED)
        if banner:
            txt, bgc, hot = banner
            self.round_rect(W / 2 - 230, 112, W / 2 + 230, 138, 8,
                            fill=self._lerp(bgc, hot, pulse), outline="")
            c.create_text(W / 2, 125, text=txt, fill="white", font=("Sans", 13, "bold"))

        # 貨物堆疊
        xs = [W * 0.26, W * 0.5, W * 0.74]
        for (code, label), cx in zip(ITEM_ORDER, xs):
            cur, th, resv = items.get(code, (0, 3, 0))
            low = cur < th
            c.create_oval(cx - 52, FLOOR - 6, cx + 52, FLOOR + 10, fill="#141420", outline="")
            vis = min(cur, 7)
            for i in range(vis):
                self.carton(cx, FLOOR - 8 - i * 28, code, low, i == vis - 1)
            if cur > 7:
                c.create_text(cx, FLOOR - 8 - 7 * 28 - 14, text="+%d" % (cur - 7),
                              fill="white", font=("Sans", 11, "bold"))
            c.create_text(cx, FLOOR + 30, text=label, fill="white", font=("Sans", 14, "bold"))
            c.create_text(cx, FLOOR + 52, text="庫存 %d（門檻 %d）" % (cur, th),
                          fill=(RED if low else "#dcdce4"), font=("Sans", 11))

        # 浮動數量變化
        alive = []
        for f in self.floats:
            age = now - f["born"]
            if age < 1.2:
                y = f["y"] - age * 40
                col = self._lerp(f["color"], BG_BOT, age / 1.2)
                c.create_text(f["x"], y, text=f["text"], fill=col, font=("Sans", 16, "bold"))
                alive.append(f)
        self.floats = alive

        # 備貨區（2 員工）
        c.create_text(W / 2, FLOOR + 78, text="── 備貨區（打卡上班才會處理；2 人可平行）──",
                      fill="#9a9ab0", font=("Sans", 12))
        for w, cx in zip(workers[:2], [W * 0.3, W * 0.7]):
            self.worker_bay(cx, FLOOR + 178, w, now)

        self.root.after(40, self.animate)


if __name__ == "__main__":
    root = tk.Tk()
    WarehouseView(root)
    root.mainloop()
