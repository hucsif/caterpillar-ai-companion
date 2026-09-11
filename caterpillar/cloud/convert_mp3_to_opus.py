#!/usr/bin/env python3
"""
把 MP3 转成 Opus 帧二进制文件，供 ESP32 下载后直接推送播放。

用法:
  python convert_mp3_to_opus.py out/2026-09-07/morning.mp3 out/2026-09-07/morning.opus

输出格式:
  [2字节大端帧长度][Opus帧数据] ... 重复

每帧 60ms, 16kHz 单声道, 对应 960 个采样点。
"""

import struct
import sys

import miniaudio
import opuslib


def convert(mp3_path: str, opus_path: str):
    # 1. 解码 MP3 -> PCM
    print(f"[convert] 解码: {mp3_path}")
    audio = miniaudio.mp3_read_file_f32(mp3_path)

    # 2. 转成 int16
    pcm = audio.samples
    if audio.nchannels == 1:
        samples = [int(max(-1.0, min(1.0, s)) * 32767) for s in pcm]
    else:
        # 立体声 -> 单声道 (取平均)
        samples = []
        for i in range(0, len(pcm), 2):
            avg = (pcm[i] + pcm[i + 1]) / 2.0
            samples.append(int(max(-1.0, min(1.0, avg)) * 32767))

    sample_rate = audio.sample_rate

    # 3. 如果需要，重采样到 16kHz (线性插值)
    if sample_rate != 16000:
        print(f"[convert] 重采样: {sample_rate}Hz -> 16000Hz")
        ratio = sample_rate / 16000.0
        new_len = int(len(samples) / ratio)
        resampled = []
        for i in range(new_len):
            src_idx = i * ratio
            src_lo = int(src_idx)
            src_hi = min(src_lo + 1, len(samples) - 1)
            frac = src_idx - src_lo
            resampled.append(int(samples[src_lo] * (1 - frac) + samples[src_hi] * frac))
        samples = resampled
        sample_rate = 16000

    # 4. 编码 Opus (60ms 帧 = 960 samples at 16kHz)
    print(f"[convert] 编码 Opus: {len(samples)} samples, {sample_rate}Hz")
    encoder = opuslib.Encoder(16000, 1, opuslib.APPLICATION_AUDIO)

    FRAME_SIZE = 960  # 60ms at 16kHz
    with open(opus_path, 'wb') as f:
        offset = 0
        frame_count = 0
        while offset + FRAME_SIZE <= len(samples):
            chunk = samples[offset:offset + FRAME_SIZE]
            pcm_bytes = b''.join(struct.pack('<h', s) for s in chunk)
            try:
                opus_data = encoder.encode(pcm_bytes, FRAME_SIZE)
            except opuslib.OpusError as e:
                print(f"[convert] Opus 编码错误 at offset {offset}: {e}")
                break
            # 写: 2字节大端长度 + Opus数据
            f.write(struct.pack('>H', len(opus_data)))
            f.write(opus_data)
            offset += FRAME_SIZE
            frame_count += 1

        # 处理末尾不足一帧的采样 (补齐到 960)
        remaining = len(samples) - offset
        if remaining > 0 and remaining < FRAME_SIZE:
            chunk = samples[offset:]
            chunk.extend([0] * (FRAME_SIZE - remaining))
            pcm_bytes = b''.join(struct.pack('<h', s) for s in chunk)
            try:
                opus_data = encoder.encode(pcm_bytes, FRAME_SIZE)
                f.write(struct.pack('>H', len(opus_data)))
                f.write(opus_data)
                frame_count += 1
            except opuslib.OpusError as e:
                print(f"[convert] Opus 编码错误 (末尾补齐): {e}")

    size_kb = __import__('os').path.getsize(opus_path) / 1024
    print(f"[convert] 完成: {opus_path} ({frame_count} 帧, {size_kb:.0f} KB)")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
