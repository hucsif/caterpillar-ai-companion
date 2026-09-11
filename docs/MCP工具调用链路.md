# 固件内置 MCP 工具调用完整链路

## 概述

ESP32 固件启动后通过持久 WebSocket 连接小智云端，LLM 可直接下发 MCP 工具调用指令控制设备（舵机、LED、表情等）。整个过程不需要本地 Python gateway。

---

## 第 1 步：小智云端 LLM 下发 MCP 调用

网关通过已有 WebSocket 发送 JSON 文本帧：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
      "name": "self.display.set_avatar",
      "arguments": {"face": "happy"}
    }
  }
}
```

---

## 第 2 步：WebSocket 接收 JSON 文本帧

**文件：`protocols/websocket_protocol.cc`**

| 行号 | 说明 |
|------|------|
| 429 | `OnData()` 回调注册，区分 binary 和 text 帧 |
| 471-473 | text 帧：`cJSON_ParseWithLength()` 解析 JSON |
| 474 | 提取 `"type"` 字段 |
| 494-497 | type 不是 hello/tts/listen → 直接调 `on_incoming_json_()` |
| 502 | `cJSON_Delete()` 释放 |

```cpp
// websocket_protocol.cc:471-497
} else {
    auto root = cJSON_ParseWithLength(data, len);
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root, notify_disconnect, arm_audio_channel);
        } else if (strcmp(type->valuestring, "tts") == 0 ||
                   strcmp(type->valuestring, "listen") == 0) {
            // session_id 校验 ...
        } else {
            if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);  // ← MCP 消息走这里
            }
        }
    }
    cJSON_Delete(root);
}
```

---

## 第 3 步：Application 分发 JSON 消息

**文件：`application.cc`**

| 行号 | 说明 |
|------|------|
| 535 | `OnIncomingJson` 回调注册（在 `InitializeProtocol()` 中） |
| 646 | 匹配 `type == "mcp"` |
| 647-649 | 提取 `"payload"` 对象，调 `McpServer::GetInstance().ParseMessage(payload)` |

```cpp
// application.cc:646-650
} else if (strcmp(type->valuestring, "mcp") == 0) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    if (cJSON_IsObject(payload)) {
        McpServer::GetInstance().ParseMessage(payload);
    }
}
```

---

## 第 4 步：McpServer JSON-RPC 解析

**文件：`mcp_server.cc`**

| 行号 | 说明 |
|------|------|
| 353 | `ParseMessage(const cJSON* json)` 入口 |
| 355-359 | 校验 `"jsonrpc":"2.0"` |
| 362-366 | 提取 `"method"` 字段 |
| 368 | 转 std::string |
| 380-385 | 提取 `"id"`（用于回复） |
| 387-398 | `method == "initialize"` → 返回 `serverInfo` |
| 399-412 | `method == "tools/list"` → 返回所有注册的工具列表 |
| 413 | `method == "tools/call"` → 执行工具 |
| 419-423 | 提取 `params.name` → 工具名 |
| 425-429 | 提取 `params.arguments` |
| 431 | `DoToolCall(id_int, tool_name, tool_arguments)` |

```cpp
// mcp_server.cc:353-431
void McpServer::ParseMessage(const cJSON* json) {
    // 校验 jsonrpc: "2.0"
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || strcmp(version->valuestring, "2.0") != 0) return;

    // 提取 method
    auto method = cJSON_GetObjectItem(json, "method");
    auto method_str = std::string(method->valuestring);

    // 提取 id
    auto id = cJSON_GetObjectItem(json, "id");
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        // 返回 serverInfo
    } else if (method_str == "tools/list") {
        // 返回工具列表
    } else if (method_str == "tools/call") {
        auto tool_name = cJSON_GetObjectItem(params, "name");
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        DoToolCall(id_int, tool_name->valuestring, tool_arguments);
    }
}
```

---

## 第 5 步：DoToolCall — 参数填充 + 调度回调

**文件：`mcp_server.cc`**

| 行号 | 说明 |
|------|------|
| 511-514 | `std::find_if` 在 `tools_` 列表中按名称查找工具 |
| 517-520 | 找不到 → `ReplyError` |
| 523 | 获取工具声明的 `PropertyList` |
| 524-567 | 遍历 properties，从 JSON arguments 提取值填充（bool/int/string/array） |
| 570-574 | 缺少必填参数 → `ReplyError` |
| 582-584 | `Application::Schedule()` → 切换到主线程执行 |
| 585-586 | `(*tool_iter)->Call(arguments)` → 触发回调函数 |

```cpp
// mcp_server.cc:511-592
void McpServer::DoToolCall(int id, const std::string& tool_name, 
                           const cJSON* tool_arguments) {
    // 1. 查找工具
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
        [&tool_name](const McpTool* tool) { return tool->name() == tool_name; });
    if (tool_iter == tools_.end()) {
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    // 2. 填充参数
    PropertyList arguments = (*tool_iter)->properties();
    for (auto& argument : arguments) {
        if (cJSON_IsObject(tool_arguments)) {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value))
                argument.set_value<bool>(value->valueint == 1);
            else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value))
                argument.set_value<int>(value->valueint);
            else if (argument.type() == kPropertyTypeString && cJSON_IsString(value))
                argument.set_value<std::string>(value->valuestring);
            // ... array types ...
        }
    }

    // 3. 切换到主线程执行回调
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        ReplyResult(id, (*tool_iter)->Call(arguments));
    });
}
```

---

## 第 6 步：McpTool::Call → 回调函数执行

**文件：`mcp_server.h`**

| 行号 | 说明 |
|------|------|
| 326 | `callback_` 成员：`std::function<ReturnValue(const PropertyList&)>` |
| 337 | 构造函数存储回调 |
| 385-386 | `Call()`: `ReturnValue return_value = callback_(properties)` |

```cpp
// mcp_server.h:385-386
std::string Call(const PropertyList& properties) {
    ReturnValue return_value = callback_(properties);
    // ... 序列化返回 ...
}
```

---

## 第 7 步：工具注册（启动时）

### 7a. 通用工具注册

**文件：`application.cc`**

| 行号 | 说明 |
|------|------|
| 117-118 | `AddCommonTools()` — 注册通用工具 |
| 119 | `AddUserOnlyTools()` — 注册用户工具 |

**文件：`mcp_server.cc`**

| 行号 | 说明 |
|------|------|
| 33 | `AddCommonTools()` 入口 |
| 45-55 | `self.get_device_status` — 设备状态 |
| 55-68 | `self.audio_speaker.set_volume` — 音量 |
| 68-83 | `self.screen.set_brightness` — 亮度 |
| 83-102 | `self.screen.set_theme` — 主题 |
| 128 | `AddUserOnlyTools()` 入口 — 系统信息、重启、固件升级等 |

### 7b. 板级工具注册

**文件：`boards/stackchan-k151-caterpillar/stackchan.cc`**

| 行号 | 说明 |
|------|------|
| 5476 | `RegisterMcpTools()` 入口 |
| 7311 | 构造函数末尾调用 |

注册的工具包括：

| 工具名 | 功能 | 大致行号 |
|--------|------|----------|
| `self.robot.set_head_angles` | 控制舵机角度 | ~5550 |
| `self.robot.get_head_angles` | 读取舵机角度 | ~5600 |
| `self.robot.set_servo_torque` | 舵机力矩开关 | ~5650 |
| `self.robot.set_auto_torque_release` | 自动释放力矩 | ~5700 |
| `self.robot.gpio_test` | GPIO 测试 | ~5750 |
| `self.robot.uart_diag` | 串口诊断 | ~5800 |
| `self.robot.check_vm_en` | 舵机电源检查 | ~5830 |
| `self.robot.get_touch_sensor_enabled` | 触摸传感器状态 | ~5860 |
| `self.robot.set_touch_sensor_enabled` | 开关触摸传感器 | ~5900 |
| `self.display.set_avatar` | 设置表情 | ~5940 |
| `self.display.set_mouth` | 设置嘴巴 | ~5990 |
| `self.display.set_mouth_sequence` | 嘴巴序列 | ~6040 |
| `self.display.set_blink` | 眨眼开关 | ~6090 |
| `self.display.switch_avatar_mode` | 切换矢量/emoji 模式 | ~6125 |
| `self.led.set_color` | 单 LED | ~6200 |
| `self.led.set_all` | 全 LED | ~6230 |
| `self.led.set_many` | 批量 LED | ~6260 |
| `self.led.clear` | 清除 LED | ~6300 |
| `self.touch.get_touch_state` | 头部触摸 | ~6320 |
| `self.gateway_config.get` | 网关配置读 | ~6360 |
| `self.gateway_config.set` | 网关配置写 | ~6400 |
| `self.i2c.scan` | I2C 扫描 | ~6440 |
| `self.i2c.read/write/write_read` | I2C 读写 | ~6480-6600 |
| `self.port_b.ws2812.*` | Port B 灯带 | ~6620-6720 |
| `self.port_c.ws2812.*` | Port C 灯带 | ~6740-6840 |

---

## 第 8 步：结果返回

### 8a. 序列化结果

**文件：`mcp_server.cc`**

| 行号 | 说明 |
|------|------|
| 438-443 | `ReplyResult(id, result)` → 包装为 `{"jsonrpc":"2.0","id":...,"result":...}` |
| 446-452 | `ReplyError(id, message)` → 包装为 `{"jsonrpc":"2.0","id":...,"error":...}` |

```cpp
// mcp_server.cc:438-443
void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}
```

### 8b. 通过 WebSocket 发送

**文件：`application.cc`**

| 行号 | 说明 |
|------|------|
| 1270-1274 | `SendMcpMessage(payload)` → `Schedule` 到主线程 → `protocol_->SendMcpMessage()` |

**文件：`protocols/protocol.cc`**

| 行号 | 说明 |
|------|------|
| 76-78 | `SendMcpMessage(payload)` → 包装为 `{"type":"mcp","payload":...}` → `SendText()` |

```cpp
// protocol.cc:76-78
void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message = "{\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}
```

**文件：`protocols/websocket_protocol.cc`**

| 行号 | 说明 |
|------|------|
| 152-157 | `SendText(text)` → `websocket_->Send(text)` 发回云端 |

---

## 完整流程图

```
云端 LLM 决定调用工具
      │
      ▼
WebSocket JSON: {"type":"mcp","payload":{"jsonrpc":"2.0","id":1,
                "method":"tools/call","params":{...}}}
      │
      ▼ websocket_protocol.cc:471-497
OnData() → cJSON_ParseWithLength() → on_incoming_json_()
      │
      ▼ application.cc:646-649
type == "mcp" → McpServer::GetInstance().ParseMessage(payload)
      │
      ▼ mcp_server.cc:353-431
JSON-RPC 2.0 解析 → method="tools/call" → DoToolCall()
      │
      ▼ mcp_server.cc:511-586
查找工具 → 遍历填充参数 → Schedule 到主线程 → callback_()
      │
      ▼ mcp_server.h:385 + stackchan.cc:5476+
回调 lambda 执行（控制舵机/LED/表情/...）
      │
      ▼ mcp_server.cc:438-443
ReplyResult(id, result_json)
      │
      ▼ application.cc:1270-1274
SendMcpMessage() → protocol_->SendMcpMessage()
      │
      ▼ protocol.cc:76-78
包装为 {"type":"mcp","payload":...}
      │
      ▼ websocket_protocol.cc:152-157
SendText() → websocket_->Send()
      │
      ▼
WebSocket JSON: {"type":"mcp","payload":{"jsonrpc":"2.0","id":1,"result":{...}}}
      │
      ▼
云端 LLM 收到工具执行结果
```

---

## 工具注册初始化时序

```
ESP32 启动
  │
  ▼
application.cc:117  →  McpServer::AddCommonTools()     // 通用工具
application.cc:119  →  McpServer::AddUserOnlyTools()   // 用户工具
  │
  ▼
StackChanBoard 构造函数
  │
  ▼
stackchan.cc:7311  →  RegisterMcpTools()              // 板级工具 (40+ 个)
  │
  ▼
所有工具存储在 McpServer::tools_ (vector<McpTool*>)
  │
  ▼
云端 tools/list 时序列化为 JSON Schema 返回给 LLM
LLM 根据 Schema 决定调用哪个工具、传什么参数
```
