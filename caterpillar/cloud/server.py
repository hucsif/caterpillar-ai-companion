#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
毛毛虫智感 · 原型内容服务器（纯标准库，单文件，局域网用）

给 StackChan 固件提供两件事：
  GET  /manifest?date=YYYY-MM-DD        -> 当日 manifest.json（缺省今天）
  GET  /audio/<date>/<file>             -> 音频/文本文件下载
  POST /event                           -> 设备事件上报（雷达事件、播报完成、追问结果）
        body: {"ev":"quiz_result","kp_id":"en-m1-song","correct":true}
        body: {"ev":"wake_confirmed","ts":...} 等 —— 全部落到 state/events.log
        quiz_result 会更新艾宾浩斯状态（答对 level+1 间隔拉长，答错清零加权）

运行： python server.py --port 8000
固件侧配置服务器地址即可（同一局域网）。量产版见《软件开发方案》的 FastAPI+MQTT 架构。
"""
import argparse
import json
import os
from datetime import date, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

BASE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(BASE, "out")
STATE_DIR = os.path.join(BASE, "..", "state")
EBBINGHAUS_DAYS = [1, 2, 4, 7, 15]

# ---- 毛毛虫成长系统（吃知识长大 → 结茧 → 变蝴蝶）----
# 积分：完成一次播报 +2；追问答对 +3；答错也 +1（参与分，保护积极性）
GROWTH_POINTS = {"broadcast_done": 2, "quiz_correct": 3, "quiz_wrong": 1}
GROWTH_STAGES = [   # (阈值, 阶段名) —— 阈值按学期节奏标定：每天约4分，一学期走完
    (0,   "caterpillar_egg",   "虫卵"),
    (30,  "small_caterpillar", "小毛毛虫"),
    (90,  "big_caterpillar",   "大毛毛虫"),
    (180, "cocoon",            "茧"),
    (300, "butterfly",         "蝴蝶"),
]


def load_growth():
    p = os.path.join(STATE_DIR, "growth.json")
    return json.load(open(p, encoding="utf-8")) if os.path.exists(p) else {"points": 0}


def save_growth(g):
    os.makedirs(STATE_DIR, exist_ok=True)
    json.dump(g, open(os.path.join(STATE_DIR, "growth.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)


def growth_status():
    g = load_growth()
    pts = g.get("points", 0)
    stage = GROWTH_STAGES[0]
    nxt = None
    for th, key, cn in GROWTH_STAGES:
        if pts >= th:
            stage = (th, key, cn)
        elif nxt is None:
            nxt = (th, key, cn)
    return {"points": pts, "stage": stage[1], "stage_cn": stage[2],
            "next_stage_at": nxt[0] if nxt else None,
            "next_stage_cn": nxt[2] if nxt else "已是蝴蝶"}


def add_growth(kind):
    g = load_growth()
    g["points"] = g.get("points", 0) + GROWTH_POINTS.get(kind, 0)
    save_growth(g)
    return growth_status()


def load_weak():
    p = os.path.join(STATE_DIR, "weak_points.json")
    return json.load(open(p, encoding="utf-8")) if os.path.exists(p) else {}


def save_weak(w):
    os.makedirs(STATE_DIR, exist_ok=True)
    json.dump(w, open(os.path.join(STATE_DIR, "weak_points.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)


def apply_quiz_result(kp_id, correct):
    weak = load_weak()
    item = weak.get(kp_id)
    if not item:
        return False
    if correct:
        item["level"] = min(item["level"] + 1, len(EBBINGHAUS_DAYS) - 1)
    else:
        item["level"] = 0
        item["wrong_count"] = item.get("wrong_count", 0) + 1
    days = EBBINGHAUS_DAYS[item["level"]]
    item["next_review"] = (date.today() + timedelta(days=days)).isoformat()
    save_weak(weak)
    return True


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/manifest":
            q = parse_qs(u.query)
            d = q.get("date", [date.today().isoformat()])[0]
            p = os.path.join(OUT_DIR, d, "manifest.json")
            if os.path.exists(p):
                self._send(200, open(p, "rb").read())
            else:
                self._send(404, {"error": f"no manifest for {d}, run scheduler.py first"})
        elif u.path == "/growth":
            # 固件轮询此接口渲染成长阶段（灯板动画/Avatar 皮肤）
            self._send(200, growth_status())
        elif u.path.startswith("/announcements/"):
            rel = u.path[len("/announcements/"):].replace("..", "")
            ann_dir = os.path.join(BASE, "out", "announcements")
            p = os.path.normpath(os.path.join(ann_dir, rel))
            if os.path.isfile(p) and p.startswith(os.path.normpath(ann_dir)):
                ext = os.path.splitext(p)[1].lower()
                # [毛毛虫改动] 合并原重复块中的 .wav/.json Content-Type 支持
                ctype = {".mp3": "audio/mpeg", ".wav": "audio/wav",
                         ".opus": "audio/ogg", ".txt": "text/plain; charset=utf-8",
                         ".json": "application/json"}.get(ext, "application/octet-stream")
                self._send(200, open(p, "rb").read(), ctype)
            else:
                self._send(404, {"error": "not found"})
            # [毛毛虫改动] 删除原重复响应块：原代码在此处有第二个 if 块再次调用 self._send，
            #             会导致 HTTP 响应重复发送（客户端收到乱码），已移除
        # [毛毛虫改动] 新增 /audio/ 路径处理
        # 固件 caterpillar_http_play() 和 cache_mgr_sync() 通过此路径下载播报音频
        # URL: /audio/{date}/{filename}  ->  文件: out/{date}/{filename}
        elif u.path.startswith("/audio/"):
            rel = u.path[len("/audio/"):].replace("..", "")
            p = os.path.normpath(os.path.join(OUT_DIR, rel))
            if os.path.isfile(p) and p.startswith(os.path.normpath(OUT_DIR)):
                ext = os.path.splitext(p)[1].lower()
                ctype = {".mp3": "audio/mpeg", ".opus": "audio/ogg",
                         ".txt": "text/plain; charset=utf-8",
                         ".json": "application/json"}.get(ext, "application/octet-stream")
                self._send(200, open(p, "rb").read(), ctype)
            else:
                self._send(404, {"error": "not found"})
        else:
            self._send(404, {"error": "unknown path"})

    def do_POST(self):
        if urlparse(self.path).path != "/event":
            return self._send(404, {"error": "unknown path"})
        n = int(self.headers.get("Content-Length", 0))
        try:
            ev = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return self._send(400, {"error": "bad json"})
        ev["_ts"] = datetime.now().isoformat(timespec="seconds")
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(os.path.join(STATE_DIR, "events.log"), "a", encoding="utf-8") as f:
            f.write(json.dumps(ev, ensure_ascii=False) + "\n")
        resp = {"ok": True}
        if ev.get("ev") == "quiz_result" and "kp_id" in ev:
            correct = bool(ev.get("correct"))
            apply_quiz_result(ev["kp_id"], correct)
            resp["growth"] = add_growth("quiz_correct" if correct else "quiz_wrong")
        elif ev.get("ev") == "broadcast_done":
            resp["growth"] = add_growth("broadcast_done")
        self._send(200, resp)

    def log_message(self, fmt, *args):
        print(f"[{datetime.now():%H:%M:%S}] {self.address_string()} {fmt % args}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"毛毛虫内容服务器 http://0.0.0.0:{args.port}  (manifest/audio/event)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
