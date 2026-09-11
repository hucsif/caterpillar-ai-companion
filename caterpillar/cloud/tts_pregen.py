#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TTS 预合成（原型版）。按可用性依次尝试三个后端：

  1. edge-tts（pip install edge-tts，免费，中文儿童向音色 zh-CN-XiaoyiNeural）
  2. Windows 内置 SAPI（无需安装，音质一般，离线可用——原型演示够用）
  3. 都不可用则跳过并提示（量产走腾讯精品音色/CosyVoice 批量接口，见《技术方案调研》）

被 scheduler.py --tts 调用，也可单独使用：
  python tts_pregen.py out/2026-09-07/morning.txt out/2026-09-07/morning.mp3
"""
import os
import subprocess
import sys

VOICE_EDGE = "zh-CN-XiaoyiNeural"   # 微软晓伊，偏儿童向


def _try_edge_tts(txt_path, out_path):
    try:
        import edge_tts  # noqa: F401
    except ImportError:
        return False
    text = open(txt_path, encoding="utf-8").read()
    cmd = [sys.executable, "-m", "edge_tts", "--voice", VOICE_EDGE,
           "--text", text, "--write-media", out_path]
    return subprocess.call(cmd) == 0 and os.path.exists(out_path)


def _try_sapi(txt_path, out_path):
    """Windows SAPI -> wav（扩展名按 wav 落盘）。"""
    if os.name != "nt":
        return False
    wav = os.path.splitext(out_path)[0] + ".wav"
    ps = (
        "Add-Type -AssemblyName System.Speech; "
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        "$zh = $s.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -like 'zh*' } | Select-Object -First 1; "
        "if ($zh) { $s.SelectVoice($zh.VoiceInfo.Name) }; "
        "$s.Rate = -1; "
        f"$s.SetOutputToWaveFile('{wav}'); "
        f"$s.Speak([IO.File]::ReadAllText('{txt_path}', [Text.Encoding]::UTF8)); "
        "$s.Dispose()"
    )
    r = subprocess.call(["powershell", "-NoProfile", "-Command", ps])
    return r == 0 and os.path.exists(wav)


def synthesize(txt_path, out_path):
    if _try_edge_tts(txt_path, out_path):
        print(f"[tts] edge-tts -> {out_path}")
        return out_path
    if _try_sapi(txt_path, out_path):
        wav = os.path.splitext(out_path)[0] + ".wav"
        print(f"[tts] Windows SAPI -> {wav}")
        return wav
    print(f"[tts] 无可用 TTS 后端，已跳过 {txt_path}（安装 edge-tts 或配置云端 TTS）")
    return None


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    synthesize(sys.argv[1], sys.argv[2])
