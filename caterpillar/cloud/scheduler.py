#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
毛毛虫智感 · 每日播报队列生成器（原型版，纯标准库）

功能（对应《软件开发方案》功能二/三）：
  1. 按 semester_start 计算教学周，从 knowledge_g2a.json 定位各科"本周单元"
  2. 结合 schedule.json 课程表生成：
     - 晨间包脚本：今天有哪些课 + 各科本周学习重点（预习）
     - 晚间包脚本：今天课程对应的重点回顾（英语单词逐个"单词→拼读→释义→例句"）
       + 艾宾浩斯到期复习项 + 追问清单(quiz)
  3. 维护 state/weak_points.json（艾宾浩斯间隔状态，追问结果由 server.py 回写）
  4. 输出 out/<date>/morning.txt、evening.txt、manifest.json
     （--tts 时调用 tts_pregen.py 合成 mp3，需可用的 TTS 后端）

用法：
  python scheduler.py --date 2026-09-07
  python scheduler.py --date 2026-09-07 --tts
"""
import argparse
import json
import os
import sys
from datetime import date, datetime, timedelta

BASE = os.path.dirname(os.path.abspath(__file__))
CONTENT_DIR = os.path.join(BASE, "..", "content")
STATE_DIR = os.path.join(BASE, "..", "state")
OUT_DIR = os.path.join(BASE, "out")

EBBINGHAUS_DAYS = [1, 2, 4, 7, 15]          # interval_level 0..4
WEEKDAY_KEYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
SUBJ_NAME = {"chinese": "语文", "math": "数学", "english": "英语"}


def load_json(path, default=None):
    if not os.path.exists(path):
        return default
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


def teaching_week(kb, d):
    """返回教学周序号（1 起）；不在学期内返回 None。"""
    start = datetime.strptime(kb["meta"]["semester_start"], "%Y-%m-%d").date()
    # 教学周从学期第一周的周一起算
    week1_monday = start - timedelta(days=start.weekday())
    if d < start:
        return None
    week = (d - week1_monday).days // 7 + 1
    if week > kb["meta"]["teaching_weeks"]:
        return None
    return week


def unit_of_week(units, week):
    for u in units:
        if week in u["weeks"]:
            return u
    return None


def spell(word):
    """把单词拆成字母朗读串：library -> L-I-B-R-A-R-Y（词组只拆首个单词）"""
    head = word.split()[0].replace("-", "")
    return "-".join(ch.upper() for ch in head)


# ---------------------------------------------------------------- 晨间脚本
def build_morning(kb, timetable_today, week):
    lines = []
    subjects_today = [p["name"] for p in timetable_today]
    lines.append(f"早上好呀！新的一天开始啦。今天有{len(timetable_today)}节课：" + "、".join(subjects_today) + "。")

    covered = set()
    for p in timetable_today:
        s = p["subject"]
        if s in covered or s not in SUBJ_NAME:
            continue
        covered.add(s)
        unit = unit_of_week(kb[s]["units"], week)
        if not unit:
            continue
        if s == "english":
            words = [w for w in unit["words"] if w["req"]][:4]
            wtxt = "、".join(f"{w['w']}（{w['cn']}）" for w in words)
            lines.append(f"英语这周学{unit['name']}，句型是：{unit['pattern']} 重点单词有：{wtxt}。上课时注意听这几个词哦。")
        else:
            kps = [k for k in unit["kps"] if k["imp"] >= 3][:2]
            focus = "；".join(k["content"].split("：")[-1][:40] for k in kps)
            lines.append(f"{SUBJ_NAME[s]}这周学《{unit['name']}》，重点是：{focus}。")

    # 带物提醒（课程表 items 字段）——家长真实痛点，晨播顺嘴一句
    items = [f"{p['name']}课{p['items']}" for p in timetable_today if p.get("items")]
    if items:
        lines.append("对了对了，" + "；".join(items) + "，别忘了哦！")

    lines.append("好啦，去洗漱吃早饭吧，毛毛虫等你放学回来！")
    return "\n".join(lines)


# ---------------------------------------------------------------- 晚间脚本
def build_evening(kb, timetable_today, week, weak):
    """返回 (script_text, quiz_list)。quiz 时间点由 TTS 后按段落定位，原型用段落序号。"""
    lines = ["晚上好！睡觉前，我们一起把今天的重点再过一遍。"]
    quiz = []
    seg = 1

    covered = set()
    for p in timetable_today:
        s = p["subject"]
        if s in covered or s not in SUBJ_NAME:
            continue
        covered.add(s)
        unit = unit_of_week(kb[s]["units"], week)
        if not unit:
            continue

        if s == "english":
            lines.append(f"先复习英语{unit['name']}的单词。")
            seg += 1
            for idx, w in enumerate([x for x in unit["words"] if x["req"]][:6]):
                lines.append(f"{w['w']}，{spell(w['w'])}，{w['w']}，意思是{w['cn']}。造个句：{unit['pattern'].replace('...', w['w'])}")
                seg += 1
                # 两种追问模式交替：quiz=考一考；teach=毛毛虫装傻求教（以教代学，
                # protégé effect——孩子当小老师，记忆留存更好、无被考核感）
                if idx % 2 == 0:
                    mode, ask = "teach", f"哎呀，我把 {w['w']} 的意思忘了……你能教教我吗？"
                    praise = "原来是这样！谢谢你教我，你讲得真清楚！"
                else:
                    mode, ask = "quiz", f"{w['w']} 是什么意思，还记得吗？"
                    praise = "答对啦，真棒！"
                quiz.append({
                    "seg": seg,
                    "mode": mode,
                    "kp_id": f"en-m{unit['no']}-{w['w']}",
                    "ask": ask,
                    "praise": praise,
                    "answer_keywords": [w["cn"].split("，")[0].split("；")[0]]
                })
        else:
            kps = [k for k in unit["kps"] if k["imp"] >= 3][:2]
            for k in kps:
                if k["type"] == "text_recite":
                    lines.append(f"{SUBJ_NAME[s]}要背诵的是：{k['content']} 跟着我在心里再背一遍。")
                else:
                    lines.append(f"{SUBJ_NAME[s]}今天的重点：{k['content']}。")
                seg += 1

    # 艾宾浩斯到期项
    due = [w for w in weak.values() if w.get("next_review") and w["next_review"] <= datetime.now().strftime("%Y-%m-%d")]
    due.sort(key=lambda x: (-x.get("wrong_count", 0), x.get("next_review", "")))
    if due:
        lines.append("接下来是几个之前学过、该复习的内容。")
        seg += 1
        for w in due[:5]:
            lines.append(w["review_text"])
            seg += 1
    lines.append("今天的复习就到这里，晚安，做个好梦！")
    return "\n".join(lines), quiz


# ---------------------------------------------------------------- 状态
def seed_weak_points(kb, weak, week, today_str):
    """把本周英语必学词注册进艾宾浩斯池（首次出现 level=0，明天到期）。"""
    unit = unit_of_week(kb["english"]["units"], week)
    if not unit:
        return
    tomorrow = (datetime.strptime(today_str, "%Y-%m-%d") + timedelta(days=1)).strftime("%Y-%m-%d")
    for w in unit["words"]:
        if not w["req"]:
            continue
        kid = f"en-m{unit['no']}-{w['w']}"
        if kid not in weak:
            weak[kid] = {
                "level": 0, "wrong_count": 0,
                "next_review": tomorrow,
                "review_text": f"单词 {w['w']}，{spell(w['w'])}，意思是{w['cn']}。"
            }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=date.today().isoformat())
    ap.add_argument("--tts", action="store_true", help="生成脚本后调用 tts_pregen 合成音频")
    args = ap.parse_args()

    d = datetime.strptime(args.date, "%Y-%m-%d").date()
    kb = load_json(os.path.join(CONTENT_DIR, "knowledge_g2a.json"))
    sched = load_json(os.path.join(CONTENT_DIR, "schedule.json"))
    weak = load_json(os.path.join(STATE_DIR, "weak_points.json"), {})

    week = teaching_week(kb, d)
    wd = WEEKDAY_KEYS[d.weekday()]
    if week is None or wd not in sched["timetable"]:
        print(f"[i] {args.date} 不是教学日（week={week}, weekday={wd}），跳过生成。")
        return 0

    timetable_today = sched["timetable"][wd]
    morning = build_morning(kb, timetable_today, week)
    evening, quiz = build_evening(kb, timetable_today, week, weak)
    seed_weak_points(kb, weak, week, args.date)

    out = os.path.join(OUT_DIR, args.date)
    os.makedirs(out, exist_ok=True)
    for name, text in [("morning.txt", morning), ("evening.txt", evening)]:
        with open(os.path.join(out, name), "w", encoding="utf-8") as f:
            f.write(text)

    manifest = {
        "date": args.date, "child_id": sched["child_id"], "teaching_week": week,
        "packages": [
            {"slot": "morning", "text": "morning.txt", "audio": "morning.mp3",
             "trigger": "EVENT_WAKE_CONFIRMED", "deadline": sched["morning_deadline"]},
            {"slot": "evening", "text": "evening.txt", "audio": "evening.mp3",
             "trigger": "EVENT_SEATED_EVENING", "window": sched["evening_review_window"],
             "quiz": quiz}
        ]
    }
    save_json(os.path.join(out, "manifest.json"), manifest)
    save_json(os.path.join(STATE_DIR, "weak_points.json"), weak)

    print(f"[ok] 第{week}教学周 {wd} -> {out}")
    print(f"     晨间 {len(morning)}字 / 晚间 {len(evening)}字 / 追问 {len(quiz)} 条 / 复习池 {len(weak)} 项")

    if args.tts:
        import tts_pregen
        tts_pregen.synthesize(os.path.join(out, "morning.txt"), os.path.join(out, "morning.mp3"))
        tts_pregen.synthesize(os.path.join(out, "evening.txt"), os.path.join(out, "evening.mp3"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
