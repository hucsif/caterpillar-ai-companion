/*
 * ld2410d.h — 海凌科 LD2410D-B 24GHz 毫米波雷达 UART 协议解析器
 *
 * 纯 C、无平台依赖。帧格式（小端字节序，来源：HLK-LD2410D-B通信协议 V1.01）：
 *
 *   SOF(4B) 0xFD 0xFC 0xFB 0xFA | len(2B, LE) | cmd(2B, LE) | data(N) | EOF(4B) 0x04 0x03 0x02 0x01
 *
 * 配置模式下需要先发使能命令(0x00FF)，结束后发结束配置命令(0x00FE)。
 * 上报模式下持续输出探测数据，无需命令。
 *
 * UART: 115200 8N1, TTL 3.3V
 *
 * 与 R60ABD1 的关键差异：
 *   1. 帧头/帧尾不同（4字节标记 vs 2字节）
 *   2. 字节序不同（LE小端 vs BE大端）
 *   3. 无校验和（协议层不做校验）
 *   4. 有配置/上报两种模式
 */
#ifndef LD2410D_H
#define LD2410D_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 帧标记 ---- */
#define LD2410D_SOF1  0xFD
#define LD2410D_SOF2  0xFC
#define LD2410D_SOF3  0xFB
#define LD2410D_SOF4  0xFA
#define LD2410D_EOF1  0x04
#define LD2410D_EOF2  0x03
#define LD2410D_EOF3  0x02
#define LD2410D_EOF4  0x01

/* ---- 关注的上报帧类型 ---- */
#define LD2410D_CMD_REPORT_DATA   0x0043  /* 探测数据上报（上报模式持续输出） */
#define LD2410D_CMD_REPORT_ACK    0x0143  /* 探测上报数据的 ACK 帧 */

/* ---- 配置命令（配置模式下使用） ---- */
#define LD2410D_CMD_ENABLE_CFG    0x00FF  /* 使能配置 */
#define LD2410D_CMD_DISABLE_CFG   0x00FE  /* 结束配置 */
#define LD2410D_CMD_READ_VERSION  0x00A0  /* 读固件版本 */
#define LD2410D_CMD_READ_PARAM    0x0008  /* 读传感器参数 */
#define LD2410D_CMD_WRITE_PARAM   0x0007  /* 写传感器参数 */
#define LD2410D_CMD_SET_BAUD      0x00A2  /* 设置波特率 */

/* ---- 探测目标状态 ---- */
#define LD2410D_TARGET_NONE       0x00  /* 无目标 */
#define LD2410D_TARGET_MOVING     0x01  /* 运动目标 */
#define LD2410D_TARGET_STATIONARY 0x02  /* 静止目标 */
#define LD2410D_TARGET_BOTH       0x03  /* 运动+静止 */

/* ---- 解析结果：雷达实时探测快照 ---- */
typedef struct {
    /* 探测状态 */
    uint8_t  target_state;     /* 0=无 1=运动 2=静止 3=运动+静止 */
    uint16_t moving_distance;  /* 运动目标距离(cm) */
    uint8_t  moving_energy;    /* 运动目标能量值 0-100 */
    uint16_t static_distance;  /* 静止目标距离(cm) */
    uint8_t  static_energy;    /* 静止目标能量值 0-100 */
    uint16_t detect_distance;  /* 探测范围距离(cm) */
    /* 帧统计 */
    uint32_t last_update_ms;
} ld2410d_status_t;

/* 帧回调：cmd/数据区/长度。返回前 status 已更新。 */
typedef void (*ld2410d_frame_cb)(uint16_t cmd,
                                  const uint8_t *data, uint16_t len, void *user);

/* 解析器实例 */
typedef struct {
    uint8_t  buf[64];
    uint16_t pos;
    uint16_t expect_len;
    int      stage;           /* 0:等SOF1 1:SOF2 2:SOF3 3:SOF4 4:头部 5:数据+尾 */
    ld2410d_status_t status;
    ld2410d_frame_cb cb;
    void    *cb_user;
    uint32_t frames_ok;
    uint32_t frames_bad;
} ld2410d_parser_t;

/* 初始化 */
void ld2410d_init(ld2410d_parser_t *p, ld2410d_frame_cb cb, void *user);

/* 逐字节喂入（UART ISR 中按块循环调用） */
void ld2410d_feed(ld2410d_parser_t *p, uint8_t byte, uint32_t now_ms);

/* 获取解析状态快照 */
static inline const ld2410d_status_t *ld2410d_status(const ld2410d_parser_t *p) {
    return &p->status;
}

#ifdef __cplusplus
}
#endif
#endif /* LD2410D_H */
