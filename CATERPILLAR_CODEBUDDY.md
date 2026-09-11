# 毛毛虫智感 · 项目代码说明书

## 一、产品一句话

一台放在孩子书桌/床头的"毫米波雷达 × 播报引擎"学习伴侣。雷达负责"看"（在离位/体动/坐姿/睡眠），伴侣负责"说"（晨间课程预告/晚间单词回顾/坐姿提醒/睡眠闹钟）。

核心闭环：**雷达感知 → FSM判断时机 → 播预生成TTS音频 → MQTT上报 → 成长系统**

---

## 二、目录结构

```
esp32/
├── stackchan-mcp/                          ← 主项目（固件 + 云端）
│   │
│   ├── firmware/                           ← ESP32-S3 固件
│   │   ├── components/
│   │   │   ├── caterpillar_radar/          ← 雷达帧解析 + 闹钟状态机
│   │   │   │   ├── r60abd1.c/h             R60ABD1 UART协议解析（115200 8N1）
│   │   │   │   ├── radar_fsm.c/h           五状态闹钟FSM（判停/回睡/起床确认）
│   │   │   │   └── radar_data_export.h     算法团队接口合约
│   │   │   └── caterpillar_broadcast/      ← 播报调度 + 缓存 + MQTT
│   │   │       ├── broadcast_sched.c/h     晨间/晚间播报触发 + 去重 + 兜底
│   │   │       ├── cache_mgr.c/h           LittleFS内容缓存 + LRU淘汰
│   │   │       └── caterpillar_mqtt.c/h    EMQX MQTT事件上报 + 心跳
│   │   │
│   │   ├── main/boards/
│   │   │   ├── stackchan/                  ← 原有标准板型
│   │   │   └── stackchan-k151-caterpillar/ ← 毛毛虫专属板型（防OTA覆盖）
│   │   │       ├── stackchan.cc            板级初始化（雷达/Avatar/舵机/WiFi/蓝牙）
│   │   │       ├── caterpillar_radar_glue.cc/h 核心胶水层（串起六大模块）
│   │   │       ├── caterpillar_http.cc/h       播报Opus下载→播放
│   │   │       ├── caterpillar_blufi.cc/h      BLE蓝牙配网
│   │   │       ├── config.h / config.json      雷达UART引脚 + 板名 + 编译选项
│   │   │       └── ...（avatar/servo/audio 从 stackchan 复制）
│   │   │
│   │   ├── main/Kconfig.projbuild          新增板型 STACKCHAN_K151_CATERPILLAR
│   │   ├── main/CMakeLists.txt             板型映射 + 组件条件链接
│   │   └── partitions/v2/16m.csv           新增 content 512KB SPIFFS分区
│   │
│   ├── caterpillar/                        ← 云端 Python 脚本
│   │   ├── cloud/
│   │   │   ├── scheduler.py                每日播报队列生成（晨间包+晚间包+艾宾浩斯）
│   │   │   ├── server.py                   HTTP内容服务（manifest/audio/event/growth）
│   │   │   ├── tts_pregen.py               TTS预合成（edge-tts → SAPI → 跳过）
│   │   │   ├── weekly_report.py            家长叙事周报（模板 + 可选LLM润色）
│   │   │   ├── knowledge_extract.py        课本照片 → 智谱GLM-4V → 结构化知识点JSON
│   │   │   ├── upload_server.py            Web上传页面（拖拽拍照 + 手动粘贴JSON）
│   │   │   ├── mqtt_bridge.py              MQTT→HTTP桥接（驱动成长系统闭环）
│   │   │   └── convert_mp3_to_opus.py      MP3→Opus音频格式转换
│   │   ├── content/
│   │   │   ├── knowledge_g2a.json          语数英知识点库（units/weeks/kps/words）
│   │   │   └── schedule.json               课程表模板（周一~周五）
│   │   └── state/
│   │       ├── events.log                  设备事件日志
│   │       ├── growth.json                 成长积分（虫卵→蝴蝶5阶段）
│   │       └── weak_points.json            艾宾浩斯复习池（5级间隔）
│   │
│   └── gateway/                            原有网关（未改动）
│
├── xiaozhi-esp32-server/                   ← 语音问答服务（ASR→LLM→TTS）
│   └── main/xiaozhi-server/
│       ├── data/.config.yaml              毛毛虫配置（prompt/唤醒词/单轮/去娱乐）
│       ├── core/handle/intentHandler.py    单轮对话（每轮清空dialogue）
│       └── plugins_func/functions/
│           └── caterpillar_knowledge.py    学习知识库MCP工具
│
└── 毛毛虫智感-交接包-2026-07-15/           ← 需求文档 + prototype（原始参考）
```

---

## 三、固件数据流

```
R60ABD1雷达 ──UART2(115200,G18/G17)──→ r60abd1.c
                                          帧解析：byte → struct
                                          {presence, body_move, breath_rate,
                                           heart_rate, in_bed, sleep_stage}
                                              │
                                              ▼
                                        radar_fsm.c
                                          五状态FSM：数据 → 语义事件
                                          EV_SEATED / EV_AWAY
                                          EV_ALARM_STOP / EV_ALARM_RESUME
                                          EV_WAKE_CONFIRMED
                                              │
                    ┌─────────────────────────┼─────────────────────────┐
                    ▼                         ▼                         ▼
          broadcast_sched.c          caterpillar_mqtt.c         caterpillar_http.c
          决定播不播                   MQTT上报事件              下载Opus→播放
          ·晨间deadline兜底           ·EV_SEATED→mosquitto     ·cache_mgr优先
          ·晚间窗口触发               ·30s心跳                  ·AudioService推送
          ·每天每包只播一次           ·dev/caterpillar/event
                    │                         │
                    ▼                         ▼
              cache_mgr.c               mqtt_bridge.py
              LittleFS缓存              MQTT→HTTP转发
              /spiffs/bc/               → server.py /event
```

---

## 四、FreeRTOS 任务

| 任务 | 核心 | 优先级 | 栈 | 功能 |
|------|------|--------|-----|------|
| audio_input | 核0 | 8 | 6KB | AFE音频前端 |
| radar_task | 任意 | 8 | 4KB | 雷达UART→FSM→播报(200ms) |
| servo_motion | 任意 | 5 | 4KB | 舵机控制 |
| audio_output | 任意 | 4 | 4KB | 音频播放 |
| cache_sync | 任意 | 3 | 3KB | 缓存同步(04-06点) |
| opus_codec | 任意 | 2 | 24KB | Opus编解码 |

---

## 五、全部脚本与服务速查

### 目录：`stackchan-mcp/caterpillar/cloud/`

| 脚本 | 功能 | 启动方式 | 端口 |
|------|------|----------|------|
| **server.py** | HTTP内容服务：固件下载播报包、上传问答结果、成长积分 | `python3 server.py --port 8001`（常驻） | 8001 |
| **scheduler.py** | 每日播报队列生成：根据课程表+知识库生成晨间/晚间脚本，维护艾宾浩斯复习池 | `python3 scheduler.py --date 日期 --tts`（cron自动） | — |
| **tts_pregen.py** | TTS预合成：文本→MP3语音（edge-tts/SAPI降级） | 被 scheduler.py --tts 内部调用 | — |
| **weekly_report.py** | 家长叙事周报：读事件日志→模板故事（可选豆包润色） | `python3 weekly_report.py --days 7`（cron自动） | — |
| **knowledge_extract.py** | 课本照片→智谱GLM-4V→结构化知识点JSON | `python3 knowledge_extract.py --image 照片.jpg --subject english --merge` | — |
| **upload_server.py** | Web上传页面：拖拽拍照提取 + 手动粘贴JSON导入 | `python3 upload_server.py --port 8002`（按需） | 8002 |
| **mqtt_bridge.py** | MQTT→HTTP桥接：订阅mosquitto雷达事件→转发到server.py→驱动成长系统 | `python3 mqtt_bridge.py`（常驻） | — |
| **convert_mp3_to_opus.py** | MP3→Opus音频格式转换 | 被其他脚本调用 | — |
| **auto_daily.sh** | cron包装脚本 | 被crontab调用 | — |

### 目录：`xiaozhi-esp32-server/main/xiaozhi-server/`

| 脚本 | 功能 | 启动方式 | 端口 |
|------|------|----------|------|
| **app.py** | 语音问答主服务：ASR→LLM→TTS全链路，WebSocket通信 | `./venv/bin/python3 app.py`（常驻） | 8000(ws) / 8003(http) |
| **plugins_func/functions/caterpillar_knowledge.py** | 学习知识库MCP工具：查询本地knowledge_g2a.json | 自动加载，无需单独启动 | — |

### 外部服务（系统级）

| 服务 | 功能 | 启动方式 | 端口 |
|------|------|----------|------|
| **mosquitto** | MQTT消息中间件 | `sudo systemctl start mosquitto`（开机自启） | 1883 |
| **crontab** | 定时任务调度 | 系统自带 | — |

### 全部启动命令（四个终端）

```bash
# 终端1: 语音问答
cd ~/esp32/xiaozhi-esp32-server/main/xiaozhi-server
./venv/bin/python3 app.py

# 终端2: 内容服务（播报包 + 成长系统）
cd ~/esp32/stackchan-mcp/caterpillar/cloud
python3 server.py --port 8001

# 终端3: MQTT事件桥接（驱动成长积分）
cd ~/esp32/stackchan-mcp/caterpillar/cloud
python3 mqtt_bridge.py

# 终端4: 课本上传（按需启动）
cd ~/esp32/stackchan-mcp/caterpillar/cloud
python3 upload_server.py --port 8002
```

### 端口总览

```
 8000  → xiaozhi-server WebSocket     （固件语音问答）
 8001  → server.py HTTP               （固件下载播报包 + 事件上报）
 8002  → upload_server.py HTTP        （家长浏览器上传课本）
 8003  → xiaozhi-server OTA           （固件OTA升级）
 1883  → mosquitto MQTT               （设备事件 + 云端指令）
18083  → EMQX Dashboard               （仅EMQX，当前用mosquitto故不涉及）
```

---

## 六、xiaozhi-esp32-server 改动记录

相较于原始仓库，以下文件做了修改：

### 修改文件

| 文件 | 改动内容 |
|------|----------|
| `main/xiaozhi-server/data/.config.yaml` | 见下方详细说明 |
| `main/xiaozhi-server/core/handle/intentHandler.py` | +3行：每轮唤醒后清空 dialogue，实现单轮对话 |

### 新增文件

| 文件 | 功能 |
|------|------|
| `main/xiaozhi-server/plugins_func/functions/caterpillar_knowledge.py` | 学习知识库MCP工具：查询本地 knowledge_g2a.json |

### .config.yaml 详细改动

| 配置项 | 原始值 | 改动后 | 原因 |
|--------|--------|--------|------|
| `wakeup_words` | 仅"你好小智"等 | 新增"毛毛虫""嗨毛毛虫" | 产品唤醒词 |
| `prompt` | 台湾女生"小智"人设 | 毛毛虫学习伴侣人设（温暖软萌、30字内、不打骂） | 产品定位 |
| `end_prompt.prompt` | "时间过得真快，依依不舍" | "毛毛虫的口吻，温柔地说晚安" | 匹配人设 |
| `system_error_response` | "主人，小智现在有点忙" | "毛毛虫的信号不太好……等下再试试好不好？" | 匹配人设 |
| `Intent.function_call.functions` | change_role, web_search, get_weather, get_news, play_music | 仅 caterpillar_knowledge | 学习伴侣不应有娱乐功能 |
| `selected_module.LLM` | ChatGLMLLM | DoubaoLLM | 豆包1.5-pro支持function call更稳定 |
| `selected_module.Memory` | — | nomem | 默认单轮不需要记忆 |

---

## 七、关键概念速查

| 概念 | 在哪 | 说明 |
|------|------|------|
| FSM五状态 | radar_fsm.c | SLEEP_MONITOR → ALARM_RINGING → GUARD_PERIOD → DAY_AWAY ⇄ DAY_SEATED |
| 判停 | FSM + 参数 | 持续≥3s体动≥30 或 离床 → 闹钟停 |
| 回睡 | FSM + 参数 | 躺回+呼吸回落+静止≥3min → 再响 |
| 起床确认 | FSM + 参数 | 离床≥60s → 触发晨间播报 |
| 入座检测 | radar_fsm.c | body_move>5 或 presence=1 持续5s |
| 知识库 | knowledge_g2a.json | 语数英，units[n].{weeks, kps/words, lessons} |
| 课程表 | schedule.json | 周一~周五，每节 {subject, name, items} |
| 艾宾浩斯 | weak_points.json | 5级 1/2/4/7/15天，quiz答对升级答错归零 |
| 成长系统 | growth.json | 5阶段: 虫卵(0)→小毛毛虫(30)→大毛毛虫(90)→茧(180)→蝴蝶(300) |
| 播报触发 | broadcast_sched.c | 晨间=起床确认/deadline兜底，晚间=入座+窗口 |
| MQTT主题 | caterpillar_mqtt.c | dev/caterpillar/event + /status |

---

## 八、新增代码一览

以下是在交接包 prototype 基础上**本次新增**的文件和修改：

**固件新增** (8个文件)
- `cache_mgr.c/h` — LittleFS内容缓存 + LRU淘汰
- `caterpillar_mqtt.c/h` — EMQX MQTT事件上报
- `radar_data_export.h` — 算法团队接口合约
- `caterpillar_radar_glue.cc/h` — 核心胶水层（串起雷达→FSM→播报→MQTT→缓存）
- `caterpillar_blufi.cc/h` — BLE蓝牙配网

**板级新增** (1个整目录)
- `boards/stackchan-k151-caterpillar/` — 独立板型，包含 config.json/h + caterpillar_http.cc/h

**构建系统修改** (3个文件)
- `Kconfig.projbuild` — 新板型注册
- `CMakeLists.txt` — 板型映射 + 组件条件链接
- `partitions/v2/16m.csv` — content 512KB分区

**云端新增** (3个文件)
- `knowledge_extract.py` — 拍照→多模态LLM提取知识点
- `upload_server.py` — Web拖拽上传页面
- `mqtt_bridge.py` — MQTT→HTTP桥接（驱动成长系统）

**xiaozhi-server 修改** (3个文件)
- `data/.config.yaml` — 毛毛虫prompt + 去娱乐插件 + 单轮化
- `intentHandler.py` — 每轮清空dialogue
- `caterpillar_knowledge.py` — 学习知识库MCP工具

**原型同步** (1个文件)
- `prototype/radar_fsm.c` — 入座判断逻辑同步（body_move>5兜底）

---

## 九、待办

- [ ] FSM 7参数真机标定（判停/翻身/回睡/离床阈值）
- [ ] 渐进式唤醒播放（微光→轻音乐→人声三级递进）
- [ ] 碎片插播 F4.5（入座+非专注→插复习条）
- [ ] 免打扰规则 F4.6（总量上限/专注静默/睡眠静默）
- [ ] 成长系统端侧渲染（灯板动画/升级仪式）
- [ ] 知识点提取全自动流水线（课本PDF→多模态LLM→入库→播报）
- [ ] 唤醒词定制（送乐鑫WakeNet）
- [ ] 家长小程序
