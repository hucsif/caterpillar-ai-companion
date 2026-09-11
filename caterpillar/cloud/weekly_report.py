#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
家长周报生成器（原型版）——把一周的设备数据写成"故事"，而不是报表。

数据源：state/events.log（设备事件）+ state/weak_points.json（学情）+ state/growth.json（成长积分）
输出：  out/weekly_report_<日期>.txt

两级生成：
  1. 模板叙事（默认，零依赖可跑）——聚合统计后套入叙事模板
  2. LLM 润色（可选）——设置环境变量 ARK_API_KEY 后，将模板稿交给
     火山方舟豆包改写成更生动的 200 字家庭故事（stdlib urllib 调用，无第三方依赖）

用法：python weekly_report.py [--child Leon] [--days 7]
"""
import argparse
import json
import os
import urllib.request
from collections import Counter
from datetime import date, datetime, timedelta

BASE = os.path.dirname(os.path.abspath(__file__))
STATE_DIR = os.path.join(BASE, "..", "state")
OUT_DIR = os.path.join(BASE, "out")

ARK_URL = "https://ark.cn-beijing.volces.com/api/v3/chat/completions"
ARK_MODEL = os.environ.get("ARK_MODEL", "doubao-seed-1-6-flash-250615")


def load_events(days):
    p = os.path.join(STATE_DIR, "events.log")
    if not os.path.exists(p):
        return []
    since = (datetime.now() - timedelta(days=days)).isoformat()
    evs = []
    for line in open(p, encoding="utf-8"):
        try:
            ev = json.loads(line)
            if ev.get("_ts", "") >= since:
                evs.append(ev)
        except json.JSONDecodeError:
            continue
    return evs


def gather_stats(days):
    evs = load_events(days)
    c = Counter(ev.get("ev") for ev in evs)
    quiz = [ev for ev in evs if ev.get("ev") == "quiz_result"]
    correct = sum(1 for q in quiz if q.get("correct"))
    weak_p = os.path.join(STATE_DIR, "weak_points.json")
    weak = json.load(open(weak_p, encoding="utf-8")) if os.path.exists(weak_p) else {}
    hardest = sorted(weak.items(), key=lambda kv: -kv[1].get("wrong_count", 0))[:3]
    growth_p = os.path.join(STATE_DIR, "growth.json")
    growth = json.load(open(growth_p, encoding="utf-8")) if os.path.exists(growth_p) else {"points": 0}
    return {
        "wake_confirmed": c.get("wake_confirmed", 0),
        "broadcast_done": c.get("broadcast_done", 0),
        "quiz_total": len(quiz), "quiz_correct": correct,
        "hardest": [(k.split("-")[-1], v.get("wrong_count", 0)) for k, v in hardest if v.get("wrong_count", 0) > 0],
        "growth_points": growth.get("points", 0),
    }


def template_story(child, s, days):
    lines = [f"【毛毛虫周报 · {date.today().isoformat()}】"]
    if s["wake_confirmed"]:
        lines.append(f"这{days}天里，{child}有 {s['wake_confirmed']} 个早晨自己坐起来关掉了闹钟——毛毛虫看到的每一次都算数。")
    if s["broadcast_done"]:
        lines.append(f"晨间课程预告和晚间复习一共完成了 {s['broadcast_done']} 次，知识一口一口喂给毛毛虫，它已经攒了 {s['growth_points']} 个成长点。")
    if s["quiz_total"]:
        rate = round(100 * s["quiz_correct"] / s["quiz_total"])
        lines.append(f"单词小课堂上，{child}当了 {s['quiz_total']} 次小老师，教对了 {s['quiz_correct']} 次（{rate}%）。")
    if s["hardest"]:
        words = "、".join(f"{w}（错{n}次）" for w, n in s["hardest"])
        lines.append(f"最难啃的骨头是：{words}——毛毛虫接下来几晚会多陪{child}练练这几个。")
    if len(lines) == 1:
        lines.append(f"本周设备数据较少，可能是假期或设备未联网。期待下周和{child}多见面！")
    lines.append("——毛毛虫敬上 🐛")
    return "\n".join(lines)


def llm_polish(draft, child):
    key = os.environ.get("ARK_API_KEY", "")  # 火山方舟 API 密钥，请通过环境变量 ARK_API_KEY 配置，勿硬编码
    if not key:
        return None
    body = json.dumps({
        "model": ARK_MODEL,
        "messages": [
            {"role": "system", "content": "你是儿童学习伴侣'毛毛虫'。把给你的数据周报改写成一段温暖、生动、约200字的家庭小故事，写给孩子的父母看。保留所有数字事实，不得编造，第一人称是毛毛虫。"},
            {"role": "user", "content": f"孩子叫{child}。数据稿：\n{draft}"},
        ],
    }).encode("utf-8")
    req = urllib.request.Request(ARK_URL, data=body, headers={
        "Content-Type": "application/json", "Authorization": f"Bearer {key}"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.loads(r.read())
        return data["choices"][0]["message"]["content"]
    except Exception as e:  # 网络/配额问题一律回退模板稿
        print(f"[warn] LLM 润色失败（{e}），使用模板稿")
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--child", default="Leon")
    ap.add_argument("--days", type=int, default=7)
    args = ap.parse_args()

    stats = gather_stats(args.days)
    draft = template_story(args.child, stats, args.days)
    final = llm_polish(draft, args.child) or draft

    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, f"weekly_report_{date.today().isoformat()}.txt")
    with open(out, "w", encoding="utf-8") as f:
        f.write(final)
    print(f"[ok] -> {out}\n")
    # Windows GBK 控制台可能打不出 emoji，降级替换，文件内容不受影响
    try:
        print(final)
    except UnicodeEncodeError:
        import sys
        print(final.encode(sys.stdout.encoding or "gbk", errors="replace")
                   .decode(sys.stdout.encoding or "gbk"))


if __name__ == "__main__":
    main()
