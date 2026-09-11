#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
毛毛虫智感 · 知识点提取流水线（原型版）

家长拍摄课本页面或上传 PDF → 多模态大模型提取 → 结构化 JSON → 写入知识库。

用法:
  # 默认用智谱GLM-4V（免费），先去 https://bigmodel.cn 注册获取API Key
  export ZHIPU_API_KEY=你的智谱API密钥

  # 单张图片
  python knowledge_extract.py --image textbook_page.jpg --grade 二年级上册 --subject english

  # 整个目录的图片
  python knowledge_extract.py --dir ./textbook_pages/ --grade 二年级上册 --subject chinese --merge

  # 用通义千问（需百炼API Key）
  VISION_PROVIDER=qwen QWEN_API_KEY=xxx python knowledge_extract.py --image page.jpg ...

输出: content/knowledge_extracted.json → 确认后 merge 到 knowledge_g2a.json

依赖: pip install requests pillow
"""

import argparse
import base64
import json
import os
import sys
from datetime import datetime
from pathlib import Path

BASE = os.path.dirname(os.path.abspath(__file__))
CONTENT_DIR = os.path.join(BASE, "..", "content")
STATE_DIR = os.path.join(BASE, "..", "state")
OUT_DIR = os.path.join(BASE, "out")

# ── 多模态 LLM 配置（三个供应商可选） ─────────────────────────
PROVIDER = os.environ.get("VISION_PROVIDER", "zhipu")

if PROVIDER == "doubao":
    # 火山引擎豆包（需要创建视觉接入点，当前豆包vision-pro已下架）
    VISION_API_URL = "https://ark.cn-beijing.volces.com/api/v3/chat/completions"
    VISION_API_KEY = os.environ.get("DOUBAO_API_KEY", "")
    VISION_MODEL = os.environ.get("DOUBAO_VISION_MODEL", "doubao-1-5-vision-pro-32k-250115")
elif PROVIDER == "qwen":
    # 阿里通义千问 VL（需百炼 API Key）
    VISION_API_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
    VISION_API_KEY = os.environ.get("QWEN_API_KEY", "")
    VISION_MODEL = "qwen-vl-max"
else:
    # 智谱 GLM-4V-Flash（免费，需注册获取 API Key）
    # https://bigmodel.cn/usercenter/proj-mgmt/apikeys
    VISION_API_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"
    VISION_API_KEY = os.environ.get("ZHIPU_API_KEY", "")  # 智谱 API 密钥，请通过环境变量 ZHIPU_API_KEY 配置，勿硬编码
    VISION_MODEL = "glm-4v-flash"

if not VISION_API_KEY:
    _env_hints = {"doubao": "DOUBAO_API_KEY", "qwen": "QWEN_API_KEY", "zhipu": "ZHIPU_API_KEY"}
    print(f"[warn] {_env_hints.get(PROVIDER, 'VISION_API_KEY')} 环境变量未设置")
    print(f"[warn] 请运行: export {_env_hints.get(PROVIDER, 'VISION_API_KEY')}=你的API密钥")

# ── 提取 Prompt（对应《软件开发方案》§1 Step 2） ─────────────
# 英语和语文/数学的知识点格式不同，按学科区分

EXTRACT_PROMPT_EN = """你是小学英语教材分析专家。这是{grade}年级英语（{press}版）教材第{unit_name}单元的页面。
请提取本页的核心知识点，输出 JSON（严格遵循 schema，不要输出其他内容）：

{{
  "unit": "单元名（如 Module 1 / Unit 1）",
  "pattern": "本单元的句型（如 There is/are...）",
  "words": [
    {{
      "w": "library",
      "cn": "图书馆",
      "req": true,
      "phonetic": "/ˈlaɪbrəri/",
      "example": "I read books in the library."
    }}
  ]
}}

规则：
- 只提取需要"记忆或掌握"的单词和句型，跳过练习题和插图说明
- 每个单词必须包含：w(英文)、cn(中文释义)、phonetic(音标)、example(一个简单例句)
- req=true 表示必须掌握的单词（考点），req=false 表示了解即可
- pattern 是该单元的语法句型，一句话描述"""

EXTRACT_PROMPT_CN = """你是小学{subject_name}教材分析专家。这是{grade}年级{subject_name}（{press}版）教材第{unit_name}单元的页面。
请提取本页的核心知识点，输出 JSON（严格遵循 schema，不要输出其他内容）：

{{
  "unit": "单元名（如 第一单元·阅读）",
  "lessons": ["本页包含的课文标题"],
  "knowledge_points": [
    {{
      "type": "concept|text_recite|formula|word_write",
      "content": "知识点本体（公式含文字表述；古诗含全文；会写字列出全部字）",
      "importance": 1-3,
      "broadcast_script": "适合8-10岁儿童收听的一句话播报文案，口语化，15字内引入"
    }}
  ]
}}

规则：
- 只提取需要"记忆或掌握"的内容，跳过练习题和插图说明
- type=text_recite：语文背诵课文，必须给出全文
- type=formula：数学公式，给出完整文字表述
- type=concept：重要的概念、寓意、理解要点
- type=word_write：语文会写字，列出全部字，如"会写字：两、就、哪、宽、顶、眼、睛"
- importance: 3=考点/课标要求必须掌握, 2=重点理解, 1=了解即可
- broadcast_script 是知识点的"声音形态"，口语化15字内，在提取时一并生成"""


def image_to_base64(path: str) -> str:
    """图片文件 → base64 字符串（智谱不要 data: 前缀，豆包/通义需要）"""
    with open(path, "rb") as f:
        data = base64.b64encode(f.read()).decode("utf-8")
    if PROVIDER in ("doubao", "qwen"):
        import mimetypes
        mime, _ = mimetypes.guess_type(path)
        return f"data:{mime or 'image/jpeg'};base64,{data}"
    # 智谱：只要纯 base64
    return data


def call_vision_api(image_paths: list[str], grade: str, subject: str,
                    press: str = "", unit_name: str = "当前") -> dict | None:
    """调用多模态LLM提取知识点
    按学科选择不同prompt：英语提取words+pattern，语文数学提取kps+lessons
    返回解析后的JSON dict，失败返回None"""
    import requests

    # 根据学科确定默认出版社和 prompt
    if not press:
        press_map = {"chinese": "人教版（2022课标修订）", "math": "人教版（2022课标修订）",
                     "english": "外研社（一年级起点）"}
        press = press_map.get(subject, "人教版")

    subject_name = {"chinese": "语文", "math": "数学", "english": "英语"}.get(subject, subject)
    if subject == "english":
        prompt = EXTRACT_PROMPT_EN.format(grade=grade, press=press, unit_name=unit_name)
    else:
        prompt = EXTRACT_PROMPT_CN.format(grade=grade, subject_name=subject_name,
                                          press=press, unit_name=unit_name)

    # 构建消息：system prompt + 用户图片
    content = [{"type": "text", "text": prompt}]
    for path in image_paths:
        content.append({
            "type": "image_url",
            "image_url": {"url": image_to_base64(path)}
        })

    body = {
        "model": VISION_MODEL,
        "messages": [
            {"role": "user", "content": content}
        ],
        "max_tokens": 1024 if PROVIDER == "zhipu" else 4096,
        "temperature": 0.1,
    }

    # 认证头：三个供应商均为 Bearer token，统一构造
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {VISION_API_KEY}"
    }

    print(f"[extract] 调用视觉模型({PROVIDER}): {len(image_paths)} 张图片 → {grade} {subject} {unit_name}")
    try:
        r = requests.post(VISION_API_URL, json=body, headers=headers, timeout=120)
        r.raise_for_status()
        result = r.json()
        text = result["choices"][0]["message"]["content"]

        # 提取 JSON（模型可能包裹在 ```json ... ``` 里）
        if "```json" in text:
            text = text.split("```json")[1].split("```")[0]
        elif "```" in text:
            text = text.split("```")[1].split("```")[0]

        return json.loads(text.strip())
    except Exception as e:
        print(f"[error] 视觉模型调用失败: {e}")
        return None


def extract_from_images(image_paths: list[str], grade: str, subject: str,
                       press: str = "", unit_name: str = "当前") -> dict | None:
    """从一张或多张图片提取知识点"""
    return call_vision_api(image_paths, grade, subject, press, unit_name)


def extract_from_pdf(pdf_path: str, grade: str, subject: str) -> list[dict]:
    """从 PDF 提取知识点：先转图片再逐页提取"""
    try:
        from pdf2image import convert_from_path
    except ImportError:
        print("[error] 需要 pip install pdf2image 和 poppler-utils")
        return []

    print(f"[pdf] 转换 PDF: {pdf_path}")
    images = convert_from_path(pdf_path, dpi=200)
    print(f"[pdf] 共 {len(images)} 页")

    results = []
    tmp_dir = os.path.join(OUT_DIR, "_pdf_pages")
    os.makedirs(tmp_dir, exist_ok=True)

    for i, img in enumerate(images):
        tmp_path = os.path.join(tmp_dir, f"page_{i+1:03d}.jpg")
        img.save(tmp_path, "JPEG", quality=85)
        unit_name = f"第{i+1}页"
        result = extract_from_images([tmp_path], grade, subject, unit_name)
        if result:
            results.append(result)
        print(f"  第{i+1}页: {'OK' if result else 'FAIL'}")

    # 清理临时图片
    import shutil
    shutil.rmtree(tmp_dir, ignore_errors=True)

    return results


def merge_into_knowledge_base(extracted: list[dict], subject: str, start_week: int = 0):
    """将提取结果合并到 knowledge_g2a.json，遵循《软件开发方案》§1 的入库规范
    start_week: 新单元的起始周（>0时使用指定值，否则自动推算）"""
    kb_path = os.path.join(CONTENT_DIR, "knowledge_g2a.json")
    kb = json.load(open(kb_path, "r", encoding="utf-8")) if os.path.exists(kb_path) else {}

    if subject not in kb:
        kb[subject] = {"units": []}

    for item in extracted:
        unit_name = item.get("unit", "未知单元")
        if not unit_name:
            print("[merge] 跳过空单元名")
            continue

        # 查找是否已有该单元
        existing = next((u for u in kb[subject]["units"]
                        if u.get("name") == unit_name), None)
        # 根据学科构建新单元内容
        new_unit = False
        if not existing:
            new_unit = True
            existing = {"no": len(kb[subject]["units"]) + 1, "name": unit_name}
            kb[subject]["units"].append(existing)

        if subject == "english":
            # 英语：words[] + pattern，无 kps
            existing.setdefault("words", [])
            existing.setdefault("pattern", "")
            existing_words = {w.get("w", "") for w in existing["words"]}
            for w in item.get("words", []):
                if w.get("w", "") not in existing_words:
                    existing["words"].append({
                        "w": w.get("w", ""),
                        "cn": w.get("cn", ""),
                        "req": w.get("req", True),
                        "phonetic": w.get("phonetic", ""),
                        "example": w.get("example", ""),
                    })
            if item.get("pattern"):
                existing["pattern"] = item["pattern"]
        else:
            # 语文/数学：kps[] + lessons[]
            existing.setdefault("kps", [])
            existing.setdefault("lessons", [])
            existing_contents = {kp.get("content", "") for kp in existing["kps"]}
            for kp in item.get("knowledge_points", []):
                content = kp.get("content", "")
                if content and content not in existing_contents:
                    existing["kps"].append({
                        "type": kp.get("type", "concept"),
                        "imp": kp.get("importance", kp.get("imp", 1)),
                        "content": content,
                        "broadcast_script": kp.get("broadcast_script", ""),
                    })
            existing_lessons = set(existing["lessons"])
            for l in item.get("lessons", []):
                if l and l not in existing_lessons:
                    existing["lessons"].append(l)

        if new_unit:
            # 设置 weeks：优先用指定的 start_week，否则接在上一单元之后
            all_units = kb[subject]["units"]
            if start_week > 0:
                existing["weeks"] = [start_week, start_week + 1]
            else:
                prev_week = 0
                if len(all_units) > 1:
                    prev = all_units[-2]
                    prev_weeks = prev.get("weeks", [0, 0])
                    prev_week = prev_weeks[-1] if prev_weeks else 0
                existing["weeks"] = [prev_week + 1, prev_week + 2]
            print(f"[merge] 新增单元: {unit_name} (weeks={existing['weeks']})")
        else:
            print(f"[merge] 合并到已有单元: {unit_name}")

    # 备份后再写
    backup_path = kb_path + ".backup." + datetime.now().strftime("%Y%m%d_%H%M%S")
    if os.path.exists(kb_path):
        os.rename(kb_path, backup_path)
        print(f"[merge] 已备份 → {backup_path}")

    os.makedirs(CONTENT_DIR, exist_ok=True)
    with open(kb_path, "w", encoding="utf-8") as f:
        json.dump(kb, f, ensure_ascii=False, indent=2)
    print(f"[merge] 知识库已更新: {kb_path}, 共 {len(kb[subject]['units'])} 个单元")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", help="单张课本照片路径")
    ap.add_argument("--dir", help="课本照片目录（按页码排序）")
    ap.add_argument("--pdf", help="课本 PDF 文件路径")
    ap.add_argument("--grade", default="二年级上册", help="年级")
    ap.add_argument("--subject", required=True, help="学科: chinese/math/english")
    ap.add_argument("--unit", default="当前", help="单元名（可选）")
    ap.add_argument("--merge", action="store_true", help="自动合并到知识库")
    ap.add_argument("--start-week", type=int, help="新单元的起始周（不填则自动推算）")
    ap.add_argument("--output", help="输出文件路径（默认 out/knowledge_extracted.json）")
    args = ap.parse_args()

    results = []

    if args.image:
        result = extract_from_images([args.image], args.grade, args.subject, args.unit)
        if result:
            results.append(result)
    elif args.dir:
        paths = sorted(Path(args.dir).glob("*"))
        paths = [str(p) for p in paths if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp")]
        print(f"[scan] 找到 {len(paths)} 张图片")
        for path in paths:
            result = extract_from_images([path], args.grade, args.subject,
                                         os.path.basename(path))
            if result:
                results.append(result)
    elif args.pdf:
        results = extract_from_pdf(args.pdf, args.grade, args.subject)

    if not results:
        print("[done] 没有提取到知识点")
        return 1

    # 保存提取结果
    out_path = args.output or os.path.join(OUT_DIR, "knowledge_extracted.json")
    # 如果是单元提取，按单元名分组
    save_data = {"grade": args.grade, "subject": args.subject,
                  "extracted": results,
                  "unit": results[0].get("unit", "unknown") if len(results) == 1 else "multi"}

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    json.dump(save_data, open(out_path, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    print(f"[save] 提取结果 → {out_path}")
    print(f"       共 {len(results)} 个单元/页")

    if args.merge:
        print("[merge] 自动合并模式...")
        merge_into_knowledge_base(results, args.subject, args.start_week or 0)

    return 0


if __name__ == "__main__":
    sys.exit(main())
