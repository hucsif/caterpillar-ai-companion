#!/usr/bin/env python3
"""
毛毛虫智感 · MQTT → HTTP 事件桥接

补齐成长系统闭环的关键一环:
  ESP32 → MQTT(mosquitto) → 本脚本 → POST /event → server.py
                                                         ├─ events.log 写入
                                                         ├─ growth.json 积分+1
                                                         └─ weak_points.json 更新
                                                              ↓
                                                         scheduler.py 次日读取
                                                              ↓
                                                         晚间播报包含艾宾浩斯复习

运行: python3 mqtt_bridge.py --mqtt-host 127.0.0.1 --server http://127.0.0.1:8001

依赖: pip install paho-mqtt
"""

import argparse
import json
import urllib.request

MQTT_TOPIC = "dev/caterpillar/#"  # 订阅所有毛毛虫设备消息


def main():
    # 连接 mosquitto → 订阅雷达事件 → 转发到 server.py /event → 驱动成长系统
    ap = argparse.ArgumentParser()
    ap.add_argument("--mqtt-host", default="127.0.0.1")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--server", default="http://127.0.0.1:8001")
    args = ap.parse_args()

    try:
        import paho.mqtt.client as mqtt
        import paho.mqtt.enums as mqtt_enums
        MQTT_V2 = True
    except ImportError:
        import paho.mqtt.client as mqtt
        mqtt_enums = None
        MQTT_V2 = False

    def on_connect(client, userdata, flags, reason_code, props=None):
        print(f"MQTT connected (rc={reason_code}), subscribe {MQTT_TOPIC}")
        client.subscribe(MQTT_TOPIC)

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
        except json.JSONDecodeError:
            payload = {"raw": msg.payload.decode()}

        body = payload if isinstance(payload, dict) else {"ev": "unknown"}
        body["_topic"] = msg.topic

        try:
            data = json.dumps(body).encode("utf-8")
            req = urllib.request.Request(
                f"{args.server}/event",
                data=data,
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=5) as r:
                resp = json.loads(r.read())
                g = resp.get("growth", {})
                if g:
                    print(f"📈 {g.get('stage_cn','?')} {g.get('points',0)}分")
        except Exception as e:
            print(f"⚠️ {e}")

    if MQTT_V2:
        client = mqtt.Client(mqtt_enums.CallbackAPIVersion.VERSION2, client_id="caterpillar-bridge")
    else:
        client = mqtt.Client(client_id="caterpillar-bridge")
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.mqtt_host, args.mqtt_port, 60)
    print(f"MQTT->HTTP bridge: {args.mqtt_host}:{args.mqtt_port} -> {args.server}")
    client.loop_forever()


if __name__ == "__main__":
    main()
