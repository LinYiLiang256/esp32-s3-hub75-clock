# HUB75 全彩 LED 单元板（64×64）demo1

用 **ESP32-S3-N16R8** 驱动 **HUB75 接口的 64×64 全彩 LED 单元板**，实现整屏
**红 → 绿 → 蓝** 每 2 秒切换一次的点亮效果。

- 构建系统：ESP-IDF v5.x（CMake）
- 驱动方式：`LCD_CAM` 的 i80 并行总线 + DMA（颜色数据），普通 GPIO 控制
  行地址 A~E、锁存 LAT、使能 OE，逐行扫描刷新
- 刷新率：约 160 Hz

---

## 1. 硬件要求

| 器件 | 说明 |
| --- | --- |
| ESP32-S3 开发板 | N16R8（16MB Flash + 8MB PSRAM），如 ESP32-S3-DevKitC-1 |
| HUB75 全彩单元板 | 64×64 点阵，1/32 扫描，16 针 HUB75(E) 接口 |
| 5V 电源 | **单元板必须用外部 5V 电源供电**（电流可达 2A 以上），不要从开发板取电 |
| 杜邦线 / IDC 排线 | 连接 16 针接口 |

> ⚠️ ESP32-S3 的 IO 为 3.3V 电平，单元板逻辑为 5V 电平。大多数单元板 3.3V
> 信号可以直接驱动；若出现花屏、颜色错乱，可加 74HCT245 电平转换。

## 2. 接线表

| HUB75 引脚 | ESP32-S3 GPIO | 说明 |
| --- | --- | --- |
| R1 | GPIO4 | 上半屏红色数据 |
| G1 | GPIO5 | 上半屏绿色数据 |
| B1 | GPIO6 | 上半屏蓝色数据 |
| R2 | GPIO7 | 下半屏红色数据 |
| G2 | GPIO8 | 下半屏绿色数据 |
| B2 | GPIO9 | 下半屏蓝色数据 |
| CLK | GPIO10 | 时钟 |
| LAT / STB | GPIO11 | 锁存 |
| OE | GPIO12 | 使能（低电平点亮） |
| A | GPIO13 | 行地址 bit0 |
| B | GPIO14 | 行地址 bit1 |
| C | GPIO15 | 行地址 bit2 |
| D | GPIO16 | 行地址 bit3 |
| E | GPIO17 | 行地址 bit4 |
| GND | GND | 与开发板共地 |
| 5V | 外部 5V 电源 | 单元板电源 |

> 另外，代码中 i80 总线还占用了 **GPIO39、GPIO40、GPIO21** 三个空闲引脚
> （用于补齐 8 位数据宽度与 D/C 信号），**不接单元板、悬空即可**，不要
> 把它们接到别的电路上。

引脚定义在 `main/main.c` 的 `s_panel_cfg` 中，可自行修改；修改后需与接线保持一致。

## 3. 编译环境

本工程使用 ESP-IDF v5.4：

```bash
# 每次打开新终端都要先导出环境变量
. $HOME/esp/esp-idf/export.sh
```

## 4. 编译与烧录

在工程根目录执行：

```bash
make build            # 等同 idf.py build
make flash            # 编译并烧录（串口自动探测）
```

也可以指定串口：

```bash
make flash PORT=/dev/ttyACM0
```

如果习惯直接调用 `idf.py`：

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM0 monitor
```

> 首次烧录前若提示找不到目标，先执行一次 `idf.py set-target esp32s3`。

## 5. 运行现象

- 上电后整屏红色，2 秒后绿色，再 2 秒后蓝色，循环往复；
- 串口日志会打印当前颜色：

```
I (1234) demo1: >>> 红色
I (3234) demo1: >>> 绿色
I (5234) demo1: >>> 蓝色
```

## 6. 代码结构

```
.
├── CMakeLists.txt        # 工程入口（CMake 构建）
├── Makefile              # 便捷命令：make build / make flash / make monitor
├── sdkconfig.defaults    # 默认配置（目标 esp32s3、16MB flash、1000Hz tick）
└── main/
    ├── CMakeLists.txt    # main 组件
    ├── main.c            # demo：红绿蓝 2 秒切换
    ├── hub75.c           # HUB75 驱动（i80 + DMA 逐行扫描）
    └── hub75.h           # 驱动接口
```

## 7. 调试与常见问题

| 现象 | 可能原因 / 处理 |
| --- | --- |
| 全屏不亮 | ① 单元板 5V 未供电或未共地；② OE 极性相反——把 `main/hub75.c` 里的 `HUB75_OE_ACTIVE_LOW` 改为 `0`；③ 排线接反 |
| 只有一半（上半或下半）亮 | 该面板可能是 1/64 扫描而不是 1/32：把 `hub75.c` 中 `scan_rows` 改为 `cfg->height`，并在 `set_addr` 中增加第 6 条地址线，或告诉我面板型号 |
| 画面错位、闪雪花 | 时钟太快或采样沿不对：把 `main.c` 的 `pclk_hz` 降到 `5MHz`；仍不行则把 `hub75.c` 中 `.pclk_active_neg` 改为 `0` 试试 |
| 亮度偏暗 | 增大 `main.c` 中的 `line_on_us`（例如 `300`），代价是刷新率下降 |
| 显示有拖影/鬼影 | 检查 LAT 引脚是否接对（GPIO11）；可把 `set_addr` 前的 `esp_rom_delay_us(2)` 增大 |
| 红绿蓝顺序反了 | 面板 R/G/B 定义不同时，在 `main.c` 的 `hub75_fill(255, 0, 0)` 调用处调整参数 |
| 串口没输出 | 监视器波特率 115200；确认烧录的是 S3 的 UART0（GPIO43/44）或原生 USB |

## 8. 说明

demo1 只做 1 bit/通道 的纯色点亮（每个颜色通道亮或灭），不做 PWM 灰度，
因此亮度均匀、代码简单。后续 demo 若需要显示图形/文字/灰度，可在此基础上
增加 BCM 或 `esp_lcd` 的 RGB 面板驱动。
