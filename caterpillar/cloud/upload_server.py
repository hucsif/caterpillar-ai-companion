#!/usr/bin/env python3
"""
毛毛虫智感 · 课本知识点录入（Web界面）

两种录入方式:
  1. 📸 拍照提取: 拖拽课本照片 → 选年级学科 → 喂给毛毛虫 → AI提取 → 确认存入知识库
  2. 📝 手动导入: 粘贴JSON → 选学科 → 喂给毛毛虫 → 直接合并到 knowledge_g2a.json

启动: python upload_server.py --port 8002
访问: http://192.168.x.x:8002

依赖: knowledge_extract.py（调用智谱GLM-4V-Flash视觉模型）
"""

import argparse
import base64
import json
import os
import sys
import tempfile
import shutil
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

BASE = os.path.dirname(os.path.abspath(__file__))
CONTENT_DIR = os.path.join(BASE, "..", "content")
EXTRACT_SCRIPT = os.path.join(BASE, "knowledge_extract.py")

# 内容服务器地址（由命令行 --server-url 传入，默认本地）
def _get_lan_ip():
    try:
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"

g_server_url = f"http://{_get_lan_ip()}:8001"

# 科目中文名
SUBJECT_CN = {"chinese": "语文", "math": "数学", "english": "英语"}
WEEKDAY_CN = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]


def announce_import(subject: str, start_week: int, unit_name: str = ""):
    """知识库更新后，通过 MQTT 通知设备播报"""
    import subprocess as sp
    from datetime import datetime

    # 1. 生成播报文本
    subj = SUBJECT_CN.get(subject, subject)
    unit_info = f"《{unit_name}》" if unit_name else ""
    unit_part = f"《{unit_name}》的" if unit_name else ""
    texts = [
        f"啊呜一口！毛毛虫把第{start_week}周{subj}{unit_part}知识点吃进肚子里啦，明天讲给你听！",
        f"咕噜咕噜~ 第{start_week}周{subj}{unit_part}新知识吞下去咯，毛毛虫又长大了一点点！",
        f"叮咚！第{start_week}周{subj}{unit_part}知识点已收到，明天就可以学新东西啦！",
    ]
    import random
    text = random.choice(texts)

    # 2. TTS 合成
    out_dir = os.path.join(BASE, "out", "announcements")
    os.makedirs(out_dir, exist_ok=True)
    ts = datetime.now().strftime("%H%M%S")
    txt_path = os.path.join(out_dir, f"ann_{ts}.txt")
    opus_path = os.path.join(out_dir, f"ann_{ts}.opus")
    with open(txt_path, "w", encoding="utf-8") as f:
        f.write(text)

    try:
        # 用 edge-tts 合成 MP3
        sp.run([sys.executable, "-m", "edge_tts", "--voice", "zh-CN-XiaoyiNeural",
                "--text", text, "--write-media", opus_path.replace(".opus", ".mp3")],
               capture_output=True, timeout=30)
        # 转 Opus
        mp3_path = opus_path.replace(".opus", ".mp3")
        if os.path.exists(mp3_path):
            convert_script = os.path.join(BASE, "convert_mp3_to_opus.py")
            if os.path.exists(convert_script):
                sp.run([sys.executable, convert_script, mp3_path, opus_path],
                       capture_output=True, timeout=30)
    except Exception as e:
        print(f"[announce] TTS failed: {e}")
        return

    if not os.path.exists(opus_path):
        print("[announce] Opus not generated, skip")
        return

    # 3. 通过 MQTT 下发播放指令
    try:
        import paho.mqtt.client as mqtt
        try:
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except (AttributeError, TypeError):
            c = mqtt.Client()
        c.connect("127.0.0.1", 1883, 5)
        c.publish("dev/caterpillar/cmd",
                  json.dumps({"cmd": "play_announcement",
                              "text": text,
                              "url": f"{g_server_url}/announcements/{os.path.basename(opus_path)}"}))
        c.disconnect()
        print(f"[announce] MQTT sent: {text}")
    except Exception as e:
        print(f"[announce] MQTT failed: {e}")

HTML_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>毛毛虫智感 · 课本上传</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Nunito:wght@600;800&display=swap');
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Nunito', -apple-system, sans-serif;
  background: linear-gradient(135deg, #fdf6e3 0%, #e8f5e9 30%, #fff3e0 60%, #fce4ec 100%);
  background-attachment: fixed;
  min-height: 100vh;
  padding: 20px;
  position: relative;
  overflow-x: hidden;
}
/* 背景装饰 */
body::before {
  content: '🐛 🌿 ⭐ 🌸 🍃 🦋 🌱 💚';
  position: fixed; top: 0; left: 0; right: 0; bottom: 0;
  font-size: 60px; opacity: .06; pointer-events: none; z-index: 0;
  display: flex; flex-wrap: wrap; justify-content: space-around; align-content: space-around;
  letter-spacing: 40px; line-height: 2;
}
body > * { position: relative; z-index: 1; }
h1 {
  color: #33691e; font-size: 28px; font-weight: 800;
  text-shadow: 2px 2px 0 rgba(139,195,74,.3);
  margin-bottom: 6px;
}
h1 + p { color: #689f38; font-size: 14px; margin-bottom: 20px; }
.card {
  background: rgba(255,255,255,.92);
  backdrop-filter: blur(10px);
  border-radius: 20px; padding: 24px; margin-bottom: 16px;
  box-shadow: 0 4px 16px rgba(139,195,74,.12), 0 1px 4px rgba(0,0,0,.06);
  border: 2px solid rgba(200,230,201,.5);
  transition: transform .15s, box-shadow .15s;
}
.card:hover { transform: translateY(-1px); box-shadow: 0 6px 20px rgba(139,195,74,.18); }
h3 { color: #558b2f; font-size: 17px; margin-bottom: 12px; }
.drop-zone {
  border: 3px dashed #a5d6a7; border-radius: 20px; padding: 48px; text-align: center;
  transition: .3s; cursor: pointer; color: #81c784;
  background: rgba(232,245,233,.5);
}
.drop-zone:hover { border-color: #66bb6a; background: rgba(200,230,201,.4); }
.drop-zone.drag-over { border-color: #43a047; background: #e8f5e9; color: #2e7d32; transform: scale(1.02); }
.preview { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 12px; }
.preview .thumb { width: 100px; height: 100px; object-fit: cover; border-radius: 12px; border: 3px solid #c8e6c9; }
select, input, button, textarea {
  font-family: 'Nunito', sans-serif;
  padding: 10px 14px; border: 2px solid #c8e6c9; border-radius: 12px; font-size: 14px;
  background: #fff; transition: .2s;
}
select:focus, input:focus, textarea:focus { border-color: #66bb6a; outline: none; box-shadow: 0 0 0 3px rgba(102,187,106,.15); }
button {
  background: linear-gradient(135deg, #66bb6a, #43a047);
  color: #fff; border: none; cursor: pointer; font-weight: 700; font-size: 14px;
  letter-spacing: .3px; box-shadow: 0 2px 8px rgba(67,160,71,.3);
  transition: .2s;
}
button:hover { background: linear-gradient(135deg, #4caf50, #2e7d32); transform: translateY(-1px); box-shadow: 0 4px 12px rgba(46,125,50,.4); }
button:disabled { background: #bdbdbd; box-shadow: none; cursor: not-allowed; transform: none; }
.form-row { display: flex; gap: 12px; margin-top: 16px; flex-wrap: wrap; }
.form-row label { display: flex; flex-direction: column; font-size: 13px; color: #689f38; gap: 4px; font-weight: 600; }
.result { background: #263238; color: #a5d6a7; padding: 16px; border-radius: 12px; font-family: monospace;
  font-size: 13px; white-space: pre-wrap; max-height: 500px; overflow-y: auto; }
.status { padding: 10px 14px; border-radius: 12px; margin-top: 12px; font-size: 14px; font-weight: 600; }
.status.ok { background: #e8f5e9; color: #2e7d32; border: 1px solid #a5d6a7; }
.status.err { background: #ffebee; color: #c62828; border: 1px solid #ef9a9a; }
.hidden { display: none; }
details { border: 2px solid #e8f5e9; border-radius: 12px; padding: 8px 12px; }
details[open] { background: #f1f8e9; }
summary { color: #689f38; font-weight: 700; }
</style>
</head>
<body>
<h1>🐛 毛毛虫智感</h1>
<p>拍照或拖拽课本页面，AI 自动提取知识点 🌱</p>

<div class="card">
  <div class="drop-zone" id="dropZone">
    <div style="font-size:64px;margin-bottom:8px">📸</div>
    <div style="font-size:18px;font-weight:700;color:#81c784">拖拽课本照片到此处</div>
    <div style="font-size:13px;color:#a5d6a7;margin-top:4px">或点击选择文件</div>
    <input type="file" id="fileInput" accept="image/*" multiple hidden>
  </div>
  <div class="preview" id="preview"></div>
  <div class="form-row">
    <label>年级
      <select id="grade"><option value="一年级上册">一年级上册</option>
        <option value="一年级下册">一年级下册</option>
        <option value="二年级上册" selected>二年级上册</option>
        <option value="二年级下册">二年级下册</option>
        <option value="三年级上册">三年级上册</option>
        <option value="三年级下册">三年级下册</option>
        <option value="四年级上册">四年级上册</option>
        <option value="四年级下册">四年级下册</option>
        <option value="五年级上册">五年级上册</option>
        <option value="五年级下册">五年级下册</option>
        <option value="六年级上册">六年级上册</option>
        <option value="六年级下册">六年级下册</option>
      </select>
    </label>
    <label>学科
      <select id="subject">
        <option value="chinese">语文</option>
        <option value="math">数学</option>
        <option value="english" selected>英语</option>
      </select>
    </label>
    <label>起始周
      <input type="number" id="startWeek" value="" placeholder="自动" min="1" max="20"
        style="width:70px;padding:8px 10px">
    </label>
    <button id="extractBtn" onclick="doExtract()">🍽️ 喂给毛毛虫</button>
  </div>
  <div id="status"></div>
</div>

<div class="card hidden" id="resultCard">
  <h3>提取结果</h3>
  <div class="result" id="result"></div>
  <div style="margin-top:12px;display:flex;gap:8px">
    <button onclick="doMerge()" id="mergeBtn" disabled>✅ 确认存入知识库</button>
    <button onclick="document.getElementById('resultCard').classList.add('hidden')">取消</button>
  </div>
</div>

<div class="card">
  <h3>📝 手动导入知识点</h3>
  <details style="margin-bottom:12px">
    <summary style="cursor:pointer;color:#666;font-size:13px">📋 格式参考（展开查看）</summary>
    <div style="background:#f8f8f8;padding:12px;border-radius:8px;margin-top:8px;font-size:12px;overflow-x:auto">
      <b>英语</b>（words + pattern）：<pre style="margin:4px 0 12px">{"grade":"二年级上册","subject":"english","extracted":[
  {"unit":"Module 1","pattern":"How are you? Im fine.",
   "words":[{"w":"song","cn":"歌曲","req":true,"phonetic":"/sɒŋ/","example":"This is my favourite song."},
            {"w":"library","cn":"图书馆","req":true,"phonetic":"/ˈlaɪbrəri/","example":"I read books in the library."}]}
]}</pre>
      <b>语文</b>（kps + lessons）：<pre style="margin:4px 0 12px">{"grade":"二年级上册","subject":"chinese","extracted":[
  {"unit":"第一单元·阅读","lessons":["小蝌蚪找妈妈","我是什么"],
   "knowledge_points":[
     {"type":"concept","content":"《小蝌蚪找妈妈》：先长后腿，再长前腿，尾巴变短","importance":3,"broadcast_script":"小蝌蚪怎么变成青蛙的？先长后腿再长前腿"},
     {"type":"text_recite","content":"《望庐山瀑布》李白：日照香炉生紫烟...","importance":3,"broadcast_script":"来一起背一首李白的诗吧"}
   ]}
]}</pre>
      <b>数学</b>（kps）：<pre style="margin:4px 0 0">{"grade":"二年级上册","subject":"math","extracted":[
  {"unit":"第一单元·长度单位","knowledge_points":[
     {"type":"concept","content":"1米=100厘米，用尺子量物体的长度","importance":3,"broadcast_script":"一米有多长？就是一百个厘米排排队"},
     {"type":"formula","content":"线段长度 = 终点刻度 - 起点刻度","importance":3,"broadcast_script":"量东西很简单，终点减起点就行啦"}
   ]}
]}</pre>
    </div>
  </details>
  <textarea id="manualJson" rows="6" style="width:100%;font-family:monospace;font-size:13px;border:1px solid #ddd;border-radius:8px;padding:12px"
    placeholder='粘贴 JSON → 选择学科 → 点击导入'></textarea>
  <div style="margin-top:8px;display:flex;gap:8px;align-items:center">
    <select id="manualSubject" style="padding:8px 12px;border:1px solid #ddd;border-radius:8px">
      <option value="english">英语</option><option value="chinese">语文</option><option value="math">数学</option>
    </select>
    <button onclick="doManualImport()">📥 喂给毛毛虫</button>
  </div>
</div>

<script>
const dz = document.getElementById('dropZone');
const fi = document.getElementById('fileInput');
const pv = document.getElementById('preview');
let files = [];

dz.onclick = () => fi.click();
dz.ondragover = e => { e.preventDefault(); dz.classList.add('drag-over'); };
dz.ondragleave = () => dz.classList.remove('drag-over');
dz.ondrop = e => {
  e.preventDefault(); dz.classList.remove('drag-over');
  addFiles(e.dataTransfer.files);
};
fi.onchange = () => addFiles(fi.files);

function addFiles(fl) {
  files = [...files, ...fl].slice(0, 10);
  pv.innerHTML = files.map(f => {
    const url = URL.createObjectURL(f);
    return `<img class="thumb" src="${url}" title="${f.name}">`;
  }).join('');
}

async function doExtract() {
  if (!files.length) return alert('请先选择课本照片');
  const btn = document.getElementById('extractBtn');
  const status = document.getElementById('status');
  const rc = document.getElementById('resultCard');
  const res = document.getElementById('result');
  const mb = document.getElementById('mergeBtn');

  btn.disabled = true; btn.textContent = '毛毛虫正在啃书...';
  status.innerHTML = '';
  rc.classList.add('hidden');

  // 把图片转成 base64 发送 JSON（避免跨域和 FormData 兼容问题）
  const images = [];
  for (const f of files) {
    const b64 = await new Promise(resolve => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result.split(',')[1]);
      reader.readAsDataURL(f);
    });
    images.push(b64);
  }

  try {
    const r = await fetch('/extract', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        images: images,
        grade: document.getElementById('grade').value,
        subject: document.getElementById('subject').value,
        start_week: document.getElementById('startWeek').value || null
      })
    });
    const data = await r.json();
    // 记住当前选择的学科和起始周，合并时一起发送
    const _subject = document.getElementById('subject').value;
    const _startWeek = document.getElementById('startWeek').value || null;
    if (data.ok) {
      status.innerHTML = '<div class="status ok">🤤 毛毛虫吃得好饱，知识点收到啦！</div>';
      res.textContent = JSON.stringify(data.result, null, 2);
      rc.classList.remove('hidden');
      mb.disabled = false;
      mb.onclick = async () => {
        mb.disabled = true; mb.textContent = '存进肚子里...';
        const mr = await fetch('/merge', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            subject: _subject,
            start_week: _startWeek,
            extracted: data.result.extracted || [data.result]
          })
        });
        const mdata = await mr.json();
        if (mdata.ok) {
          status.innerHTML = '<div class="status ok">🤤 吃进去了！知识点已存好~</div>';
          rc.classList.add('hidden');
          // 清空已上传的图片
          files = []; pv.innerHTML = '';
          document.getElementById('fileInput').value = '';
        } else {
          status.innerHTML = '<div class="status err">😢 ' + mdata.error + '</div>';
          mb.disabled = false; mb.textContent = '✅ 确认存入知识库';
        }
      };
    } else {
      status.innerHTML = '<div class="status err">❌ ' + data.error + '</div>';
    }
  } catch(e) {
    status.innerHTML = '<div class="status err">❌ 请求失败: ' + e.message + '</div>';
  }
  btn.disabled = false; btn.textContent = '🍽️ 喂给毛毛虫';
}

async function doManualImport() {
  const jsonStr = document.getElementById('manualJson').value.trim();
  if (!jsonStr) return alert('请粘贴JSON知识点');
  try {
    const data = JSON.parse(jsonStr);
    const subject = document.getElementById('manualSubject').value;
    const startWeek = parseInt(document.getElementById('startWeek').value) || 0;
    const status = document.getElementById('status');
    const r = await fetch('/merge', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        subject: subject,
        extracted: data.extracted || [data],
        start_week: startWeek
      })
    });
    const mdata = await r.json();
    if (mdata.ok) {
      status.innerHTML = '<div class="status ok">🤤 吃进去了！知识点已存好~</div>';
      document.getElementById('manualJson').value = '';
    } else {
      status.innerHTML = '<div class="status err">😢 ' + mdata.error + '</div>';
    }
  } catch(e) {
    document.getElementById('status').innerHTML = '<div class="status err">😢 这个JSON好像不太对，检查一下格式？</div>';
  }
}
</script>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        if self.path == "/" or self.path == "/upload":
            self._send(200, HTML_PAGE.encode("utf-8"), "text/html; charset=utf-8")
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/extract":
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or "{}")

            grade = body.get("grade", "二年级上册")
            subject = body.get("subject", "english")
            images_b64 = body.get("images", [])
            start_week = body.get("start_week")

            if not images_b64:
                return self._send(400, {"ok": False, "error": "没有上传图片"})

            # 直接 import 调用（避免 subprocess 开销）
            tmp_dir = tempfile.mkdtemp()
            try:
                tmp_paths = []
                for i, b64 in enumerate(images_b64):
                    tmp_path = os.path.join(tmp_dir, f"page_{i:03d}.jpg")
                    with open(tmp_path, "wb") as f:
                        f.write(base64.b64decode(b64))
                    tmp_paths.append(tmp_path)

                from knowledge_extract import extract_from_images
                results = []
                for path in tmp_paths:
                    result = extract_from_images([path], grade, subject,
                                                 os.path.basename(path))
                    if result:
                        results.append(result)

                if results:
                    save_data = {"grade": grade, "subject": subject,
                                 "extracted": results,
                                 "unit": results[0].get("unit", "unknown") if len(results) == 1 else "multi"}
                    self._send(200, {"ok": True, "result": save_data})
                else:
                    self._send(500, {"ok": False, "error": "提取失败，检查 API Key 和网络"})
            except Exception as e:
                self._send(500, {"ok": False, "error": str(e)})
            finally:
                shutil.rmtree(tmp_dir, ignore_errors=True)

        elif self.path == "/merge":
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or "{}")
            try:
                from knowledge_extract import merge_into_knowledge_base
                extracted = body.get("extracted", [body])
                subject = body.get("subject", "english")
                try:    start_week = int(body.get("start_week", 0) or 0)
                except: start_week = 0
                merge_into_knowledge_base(extracted, subject, start_week)
                unit_name = (extracted[0].get("unit", "") if extracted else "")
                announce_import(subject, start_week, unit_name)
                self._send(200, {"ok": True, "msg": "已合并到知识库"})
            except Exception as e:
                self._send(500, {"ok": False, "error": str(e)})
        else:
            self._send(404, {"error": "not found"})


def main():
    global g_server_url
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8002)
    ap.add_argument("--server-url", default="http://127.0.0.1:8001",
                    help="内容服务器地址（用于生成播报音频下载URL）")
    args = ap.parse_args()
    g_server_url = args.server_url.rstrip("/")

    lan_ip = _get_lan_ip()

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"🐛 毛毛虫智感 · 课本上传")
    print(f"   http://{lan_ip}:{args.port}")
    print(f"   1. 浏览器打开上述地址")
    print(f"   2. 拖拽课本照片 → 选年级学科 → 喂给毛毛虫")
    print(f"   3. 确认结果后存入知识库")
    srv.serve_forever()


if __name__ == "__main__":
    main()
