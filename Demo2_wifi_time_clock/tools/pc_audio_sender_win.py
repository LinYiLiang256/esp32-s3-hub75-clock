#!/usr/bin/env python3
"""
Windows 版音频频谱发送器（WASAPI Loopback）

采集电脑系统音频（扬声器输出），做 FFT 分解为 64 个频带，
通过串口发送给 ESP32，驱动点阵屏的拾音器面板。

依赖安装（在 cmd/PowerShell 中执行）：
    pip install numpy pyserial sounddevice

用法：
    python pc_audio_sender_win.py --port COM3
    python pc_audio_sender_win.py --port COM3 --gain 1.3
    python pc_audio_sender_win.py --port COM3 --list    # 查看可用音频设备

协议（115200 8N1）：
    [0xAA][0x55] + 64 字节频带能量(0~100) + 1 字节校验和
"""

import argparse
import sys
import time

import numpy as np
import serial

BANDS = 64
WAVE_LEN = 256  # 示波器波形点数
FREQ_MIN = 40.0      # Hz（覆盖底鼓 40~60Hz 的低频能量）
FREQ_MAX = 3800.0    # Hz
SAMPLE_RATE = 8000   # 处理采样率（分辨率 7.8Hz）
CAPTURE_RATE = 48000 # 采集采样率（WASAPI 常用）
FFT_SIZE = 1024
FRAME_RATE = 30


def list_devices(sd):
    print("可用音频设备：")
    for i, d in enumerate(sd.query_devices()):
        mark = ""
        if d["max_input_channels"] > 0 and "loopback" in d["name"].lower():
            mark = "  <-- 推荐（Loopback）"
        print("  [%d] %-60s in=%d out=%d%s" % (
            i, d["name"], d["max_input_channels"], d["max_output_channels"], mark))


def find_loopback_device(sd):
    """寻找系统输出的录播设备（[Loopback] 或 立体声混音）"""
    devices = sd.query_devices()

    # 1. 优先 sounddevice 的 [Loopback] 设备（现代 Windows 都有）
    for i, d in enumerate(devices):
        if d["max_input_channels"] > 0 and "loopback" in d["name"].lower():
            return i, d

    # 2. 退而求其次：立体声混音（Stereo Mix）
    for i, d in enumerate(devices):
        name = d["name"].lower()
        if d["max_input_channels"] > 0 and any(
                k in name for k in ("stereo mix", "立体声混音", "混音", "what u hear")):
            return i, d

    return None, None


def main():
    ap = argparse.ArgumentParser(description="电脑音频 → ESP32 频谱（Windows）")
    ap.add_argument("--port", default="COM3", help="ESP32 串口，例如 COM3")
    ap.add_argument("--gain", type=float, default=1.0, help="灵敏度倍率")
    ap.add_argument("--device", type=int, default=None, help="手动指定输入设备索引")
    ap.add_argument("--list", action="store_true", help="列出音频设备后退出")
    args = ap.parse_args()

    try:
        import sounddevice as sd
    except ImportError:
        print("缺少 sounddevice，请先执行: pip install numpy pyserial sounddevice")
        sys.exit(1)

    if args.list:
        list_devices(sd)
        return

    if args.device is not None:
        dev_idx = args.device
        dev = sd.query_devices(dev_idx)
    else:
        dev_idx, dev = find_loopback_device(sd)

    if dev_idx is None:
        print("未找到 Loopback / 立体声混音 设备。")
        print("可以尝试：")
        print("  1. 在 Windows 声音设置中启用'立体声混音'（部分声卡需要）")
        print("  2. 用 --list 查看设备，然后用 --device 索引 手动指定")
        list_devices(sd)
        sys.exit(1)

    print("采集设备: [%d] %s" % (dev_idx, dev["name"]))

    ser = serial.Serial(args.port, 115200, timeout=1)
    ser.dtr = False
    ser.rts = False
    print("串口:", args.port, "（ESP32 拾音器面板将改用电脑音频）")

    # 采集块：48kHz 下 6144 样本 = 128ms，重采样后 1024 样本 @ 8kHz
    chunk = FFT_SIZE * 6
    cap_channels = 2

    freqs = FREQ_MIN * (FREQ_MAX / FREQ_MIN) ** (np.arange(BANDS) / (BANDS - 1))
    fft_freqs = np.fft.rfftfreq(FFT_SIZE, d=1.0 / SAMPLE_RATE)

    peak_global = 1e-6
    smooth = np.zeros(BANDS)
    t_start = time.time()
    frames = 0

    try:
        with sd.InputStream(device=dev_idx, channels=cap_channels,
                            samplerate=CAPTURE_RATE, blocksize=chunk) as stream:
            print("开始发送（Ctrl+C 停止）")
            while True:
                data, overflowed = stream.read(chunk)
                mono = data.mean(axis=1)

                # 48kHz → 8kHz：每 6 个样本取平均（简单低通 + 抽取）
                mono8 = mono.reshape(-1, 6).mean(axis=1)[:FFT_SIZE]

                spec = np.abs(np.fft.rfft(mono8 * np.hanning(FFT_SIZE)))

                band_vals = np.zeros(BANDS)
                for i, f in enumerate(freqs):
                    lo = f * 0.75 if i == 0 else (freqs[i - 1] + f) / 2.0
                    hi = f * 1.25 if i == BANDS - 1 else (f + freqs[i + 1]) / 2.0
                    idx = (fft_freqs >= lo) & (fft_freqs < hi)
                    if idx.any():
                        band_vals[i] = spec[idx].mean()

                peak_global = max(peak_global * 0.995, float(band_vals.max()))
                rel = np.clip(band_vals / max(peak_global, 1e-6), 1e-6, 1.0)
                db = 20.0 * np.log10(rel)
                norm = np.clip(1.0 + db / 45.0, 0.0, 1.0)
                norm = np.sqrt(norm)
                norm[norm < 0.12] = 0.0
                smooth = smooth * 0.5 + norm * 0.5
                vals = (np.clip(smooth * args.gain, 0, 1) * 100).astype(np.uint8)

                payload = vals.tobytes()
                frame = bytes([0xAA, 0x55]) + payload + bytes([sum(payload) & 0xFF])
                ser.write(frame)

                frames += 1
                target = t_start + frames / FRAME_RATE
                delay = target - time.time()
                if delay > 0:
                    time.sleep(delay)

    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
