/*
 * ld2410d.c — 海凌科 LD2410D-B 24GHz 毫米波雷达 UART 帧解析实现
 *
 * 帧格式（小端字节序，来源：HLK-LD2410D-B通信协议 V1.01）：
 *
 *   SOF(4B) 0xFD 0xFC 0xFB 0xFA | len(2B,LE) | cmd(2B,LE) | data(len-2) | EOF(4B) 0x04 0x03 0x02 0x01
 *
 * 注意：len 包含 cmd 字段（即 len = 2 + data实际长度）。
 * 上报模式下持续输出探测数据帧(cmd=0x0043)，无需进入配置模式。
 */
#include "ld2410d.h"
#include <string.h>

/* 帧内偏移（相对 buf[0]=SOF1） */
#define OFF_LENL    4   /* 长度低字节（LE） */
#define OFF_LENH    5   /* 长度高字节 */
#define OFF_CMDL    6   /* 命令低字节 */
#define OFF_CMDH    7   /* 命令高字节 */
#define OFF_DATA    8   /* 数据区起始 */
#define LEN_SOF     4   /* 帧头长度 */
#define LEN_EOF     4   /* 帧尾长度 */

void ld2410d_init(ld2410d_parser_t *p, ld2410d_frame_cb cb, void *user) {
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->cb_user = user;
}

/* 将 data[] 按 LE 解析为探测数据，更新 status */
static void parse_report_frame(ld2410d_parser_t *p, const uint8_t *d, uint16_t len,
                                uint32_t now_ms) {
    ld2410d_status_t *s = &p->status;
    /*
     * 上报数据帧(data area, LE byte order):
     *   [0]     target_state   0=无 1=运动 2=静止 3=运动+静止
     *   [1-2]   moving_distance_cm   uint16 LE
     *   [3]     moving_energy        0-100
     *   [4-5]   stationary_distance_cm  uint16 LE
     *   [6]     stationary_energy    0-100
     *   [7-8]   detection_distance_cm  uint16 LE
     *
     * 注：不同固件版本字节偏移可能不同，上真机后需对标定。
     */
    if (len < 9) return;

    s->target_state     = d[0];
    s->moving_distance  = ((uint16_t)d[2] << 8) | d[1];
    s->moving_energy    = d[3];
    s->static_distance  = ((uint16_t)d[5] << 8) | d[4];
    s->static_energy    = d[6];
    s->detect_distance  = ((uint16_t)d[8] << 8) | d[7];
    s->last_update_ms   = now_ms;
}

/* 读 LE uint16 */
static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void ld2410d_feed(ld2410d_parser_t *p, uint8_t b, uint32_t now_ms) {
    switch (p->stage) {
    case 0: /* 等 SOF1 */
        if (b == LD2410D_SOF1) { p->buf[0] = b; p->stage = 1; }
        break;
    case 1: /* 等 SOF2 */
        if (b == LD2410D_SOF2) { p->buf[1] = b; p->stage = 2; }
        else { p->stage = (b == LD2410D_SOF1) ? 1 : 0; }
        break;
    case 2: /* 等 SOF3 */
        if (b == LD2410D_SOF3) { p->buf[2] = b; p->stage = 3; }
        else { p->stage = (b == LD2410D_SOF1) ? 1 : 0; }
        break;
    case 3: /* 等 SOF4 */
        if (b == LD2410D_SOF4) { p->buf[3] = b; p->stage = 4; p->pos = LEN_SOF; }
        else { p->stage = (b == LD2410D_SOF1) ? 1 : 0; }
        break;
    case 4: /* len(2B LE) + cmd(2B LE) + data */
        p->buf[p->pos++] = b;
        if (p->pos == LEN_SOF + 2) {
            /* 解析长度：len 包含 cmd 字段 */
            uint16_t total = read_u16_le(&p->buf[OFF_LENL]);
            if (total < 2 || total > sizeof(p->buf) - LEN_SOF - LEN_EOF) {
                p->frames_bad++;
                p->stage = 0;
            } else {
                p->expect_len = total;
                p->stage = 5;
            }
        }
        break;
    case 5: /* data + EOF(4B) */
        p->buf[p->pos++] = b;
        uint16_t frame_end = LEN_SOF + p->expect_len + LEN_EOF;
        if (p->pos == frame_end) {
            /* 校验帧尾 */
            bool ok = (p->buf[frame_end - 4] == LD2410D_EOF1) &&
                      (p->buf[frame_end - 3] == LD2410D_EOF2) &&
                      (p->buf[frame_end - 2] == LD2410D_EOF3) &&
                      (p->buf[frame_end - 1] == LD2410D_EOF4);
            if (ok) {
                p->frames_ok++;
                uint16_t cmd = read_u16_le(&p->buf[OFF_CMDL]);
                uint16_t data_len = p->expect_len - 2;  /* 减去cmd */
                const uint8_t *data = &p->buf[OFF_DATA];
                /* 解析上报数据帧 */
                if (cmd == LD2410D_CMD_REPORT_DATA) {
                    parse_report_frame(p, data, data_len, now_ms);
                }
                if (p->cb) {
                    p->cb(cmd, data, data_len, p->cb_user);
                }
            } else {
                p->frames_bad++;
            }
            p->stage = 0;
            p->pos = 0;
        }
        break;
    default:
        p->stage = 0;
        p->pos = 0;
        break;
    }
}
