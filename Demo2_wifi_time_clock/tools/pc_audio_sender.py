#!/usr/bin/env python3
"""
PC 端音频频谱发送器

采集电脑系统音频（扬声器输出），做 FFT 分解为 64 个频带，
通过 USB 串口发送给 ESP32，驱动点阵屏的拾音器面板（环形/横排/密集）。

用法：
    python3 pc_audio_sender.py                    # 自动找默认输出设备的 monitor 源
    python3 pc_audio_sender.py --port /dev/ttyACM0
    python3 pc_audio_sender.py --source alsa_output.pci-0000_00_1f.3.HiFi__Speaker__sink.monitor
    python3 pc_audio_sender.py --gain 1.5         # 调整灵敏度

依赖：numpy、pyserial（parec 用于抓取 PulseAudio/PipeWire 音频流）

协议（115200 8N1）：
    [0xAA][0x55] + 64 字节频带能量(0~100) + 1 字节校验和
"""

import argparse
import os
import select
import subprocess
import sys
import time

import numpy as np
import serial

BANDS = 64
WAVE_LEN = 256  # 示波器波形点数
FREQ_MIN = 40.0      # Hz（覆盖底鼓 40~60Hz 的低频能量）
FREQ_MAX = 3800.0    # Hz（8kHz 采样的 Nyquist 4kHz 以内）
SAMPLE_RATE = 8000   # 8kHz：频率分辨率 7.8Hz，低频 60~200Hz 每个频带都能独立对应
FFT_SIZE = 1024
FRAME_RATE = 30      # 发送帧率


def read_latest(stream, size):
    """读取最新的 size 字节，丢弃积压的旧数据（防止延迟随时间累积）"""
    fd = stream.fileno()
    data = b""
    while True:
        r, _, _ = select.select([fd], [], [], 0)
        if not r:
            break
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        data += chunk
    if len(data) >= size:
        return data[-size:]
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            return b""
        data += chunk
    return data[-size:]


def default_monitor_source():
    """用 pactl 查默认输出设备的 monitor 源"""
    try:
        sink = subprocess.check_output(
            ["pactl", "get-default-sink"], stderr=subprocess.DEVNULL
        ).decode().strip()
        return sink + ".monitor"
    except Exception:
        return None


def list_sources():
    try:
        out = subprocess.check_output(
            ["pactl", "list", "short", "sources"], stderr=subprocess.DEVNULL
        ).decode()
        for line in out.splitlines():
            print(" ", line)
    except Exception as e:
        print("无法列出音频源:", e)


def main():
    ap = argparse.ArgumentParser(description="电脑音频 → ESP32 频谱")
    ap.add_argument("--port", default="/dev/ttyACM0", help="ESP32 串口，默认 /dev/ttyACM0")
    ap.add_argument("--source", default=None,
                    help="PulseAudio monitor 源；默认用当前默认输出的 monitor")
    ap.add_argument("--gain", type=float, default=1.0, help="灵敏度倍率（默认 1.0）")
    ap.add_argument("--freq-min", type=float, default=FREQ_MIN)
    ap.add_argument("--freq-max", type=float, default=FREQ_MAX)
    ap.add_argument("--rate", type=int, default=SAMPLE_RATE, help="采样率（默认 8000）")
    args = ap.parse_args()

    source = args.source or default_monitor_source()
    if not source:
        print("未找到音频源，可用 --source 手动指定。可用源列表：")
        list_sources()
        sys.exit(1)
    print("采集源:", source)

    # 启动 parec 抓取 audio 流（s16le stereo 48kHz）
    parec = subprocess.Popen(
        ["parec", "--device=" + source, "--format=s16le",
         "--rate=%d" % args.rate, "--channels=2", "--latency-msec=20"],
        stdout=subprocess.PIPE,
    )

    ser = serial.Serial(args.port, 115200, timeout=1)
    # 复位 ESP32 并等待启动（避免打开串口时停在下载模式）
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    ser.dtr = False
    print("串口:", args.port, "已复位 ESP32，等待启动（约 4 秒）...")
    time.sleep(4.0)
    print("开始发送音频频谱数据（Ctrl+C 停止）")

    # 64 个频带的中心频率（对数分布）
    freqs = args.freq_min * (args.freq_max / args.freq_min) ** (
        np.arange(BANDS) / (BANDS - 1)
    )
    fft_freqs = np.fft.rfftfreq(FFT_SIZE, d=1.0 / args.rate)

    # 全局滚动峰值（所有频带共用），避免安静频带的底噪被放大到满格
    peak_global = 1e-6
    smooth = np.zeros(BANDS)

    chunk_bytes = FFT_SIZE * 2 * 2  # stereo s16le
    t_start = time.time()
    frames = 0

    try:
        while True:
            # 只取最新一帧，丢弃积压数据，保持稳定低延迟
            chunk = read_latest(parec.stdout, chunk_bytes)
            if not chunk:
                print("parec 已退出")
                return

            audio = np.frombuffer(chunk, dtype=np.int16).astype(np.float32) / 32768.0
            stereo = audio.reshape(-1, 2)
            mono = stereo.mean(axis=1)

            spec = np.abs(np.fft.rfft(mono * np.hanning(FFT_SIZE)))

            band_vals = np.zeros(BANDS)
            for i, f in enumerate(freqs):
                lo = f * 0.75 if i == 0 else (freqs[i - 1] + f) / 2.0
                hi = f * 1.25 if i == BANDS - 1 else (f + freqs[i + 1]) / 2.0
                idx = (fft_freqs >= lo) & (fft_freqs < hi)
                if idx.any():
                    band_vals[i] = spec[idx].mean()

            # 相邻频带平滑（1-2-1）：让相邻频带能量连续，
            # 避免某些"空档"频率（如 64~90Hz）的柱子完全不动
            band_vals = np.convolve(band_vals, np.array([0.25, 0.5, 0.25]), mode="same")

            # 全局峰值归一化（所有频带共用，保留相对强弱）
            peak_global = max(peak_global * 0.995, float(band_vals.max()))
            rel = np.clip(band_vals / max(peak_global, 1e-6), 1e-6, 1.0)

            # dB 压缩：把宽动态范围映射到 0~1（-45dB~0dB），弱信号（高频）也能看到
            db = 20.0 * np.log10(rel)
            norm = np.clip(1.0 + db / 45.0, 0.0, 1.0)

            # 再开方提升低端，让柱子更饱满
            norm = np.sqrt(norm)

            # 噪声门：归一化后低于 12% 视为底噪，直接归零
            norm[norm < 0.12] = 0.0

            # 平滑：上升快、回落慢
            smooth = smooth * 0.5 + norm * 0.5
            vals = (np.clip(smooth * args.gain, 0, 1) * 100).astype(np.uint8)

            # 频带帧（隔帧发送，给波形帧留带宽）：AA 55 01 + 64 字节 + 校验
            if frames % 2 == 0:
                payload = vals.tobytes()
                frame = bytes([0xAA, 0x55, 0x01]) + payload + bytes([sum(payload) & 0xFF])
                ser.write(frame)

            # 波形帧（每帧发送，30fps）：AA 55 02 + 左声道128 + 右声道128 + 校验
            l_ch = stereo[:, 0]
            r_ch = stereo[:, 1]
            lw = l_ch[::4][:WAVE_LEN // 2].astype(np.float32)
            rw = r_ch[::4][:WAVE_LEN // 2].astype(np.float32)
            if len(lw) < WAVE_LEN // 2:
                lw = np.pad(lw, (0, WAVE_LEN // 2 - len(lw)))
                rw = np.pad(rw, (0, WAVE_LEN // 2 - len(rw)))
            # 左右共同峰值归一化（保持立体声 XY 比例，图形不变形）
            wmax = float(max(np.max(np.abs(lw)), np.max(np.abs(rw))))
            if wmax > 0.005:
                lw = np.clip(lw / wmax * 127.0, -127, 127).astype(np.int8)
                rw = np.clip(rw / wmax * 127.0, -127, 127).astype(np.int8)
            else:
                lw = np.zeros(WAVE_LEN // 2, dtype=np.int8)
                rw = np.zeros(WAVE_LEN // 2, dtype=np.int8)
            wb = lw.tobytes() + rw.tobytes()
            wframe = bytes([0xAA, 0x55, 0x02]) + wb + bytes([sum(wb) & 0xFF])
            ser.write(wframe)

            frames += 1
            target = t_start + frames / FRAME_RATE
            delay = target - time.time()
            if delay > 0:
                time.sleep(delay)

    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        parec.terminate()
        ser.close()


if __name__ == "__main__":
    main()
