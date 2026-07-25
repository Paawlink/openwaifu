# OpenWaifu

OpenWaifu 是一个基于 TuyaOpen SDK 和 LVGL 9 的示例应用，运行在 T5AI 开发板上。
它扮演 **BLE Peripheral（从机）** 角色，电脑端（OpenWaifuD 守护进程）可通过蓝牙
连接并向设备推送 AI Agent 的会话状态（UTF-8 命令，支持中文）。屏幕以**会话看板**
形式展示：**每个活跃会话固定占一行/一张卡片**，实时显示状态与已运行时长，并
根据活跃会话数量切换设备「情绪」。

## 功能

- BLE Peripheral，广播名称为 `OpenWaifu`
- 使用 TuyaOpen 标准 GATT 服务（V2）：
  - Service UUID：`0xFD50`（`0000fd50-0000-1000-0880-00805f9b34fb`）
  - Write 特征 UUID：`00000001-0000-1001-8001-00805f9b07d0`
  - Notify 特征 UUID：`00000002-0000-1001-8001-00805f9b07d0`
- 接收 UTF-8 命令行（支持中文），自动校验 UTF-8 合法性并按 FIFO 环形缓冲暂存
- 单击设备按键开始录音，通过 Notify 向 OpenWaifuD 发送 PCM 音频
- LCD 会话看板：每个活跃会话固定占一行/一张卡片，实时显示状态、任务与本地跳秒的运行时长
- 情绪状态机：按活跃会话数量切换（0=睡觉中、1=摸鱼中、2-3=认真搬砖、4-6=火力全开、>6=要炸了）
- 左下角全局事件状态机（泳道 2）：收到完成/出错/取消事件时瞬时展示，数秒后自动回落中性态
- 断开连接后自动恢复广播

## 源码模块划分

应用按职责拆分为三个模块，便于阅读与维护：

| 文件                                              | 职责                                                   |
|---------------------------------------------------|--------------------------------------------------------|
| `src/openwaifu.c`                                 | 应用主入口，按顺序初始化日志/硬件/UI/BLE                |
| `src/openwaifu_ble.c` + `include/openwaifu_ble.h` | BLE 从机：广播、事件回调、UTF-8 校验、命令行 FIFO 环形缓冲 |
| `src/openwaifu_ui.c` + `include/openwaifu_ui.h`   | LVGL 界面：会话看板 + 情绪状态机、本地实时计时           |

UI 模块通过 BLE 模块暴露的接口（`openwaifu_ble_is_connected` / `openwaifu_ble_fetch_message`）
单向拉取连接状态与新命令行，解析为会话看板的增/删/改，两个模块之间不直接依赖 LVGL 与 BLE 协议栈细节。

## 部署（构建与烧录）

### 环境要求

- 已克隆 [TuyaOpen](https://github.com/tuya/TuyaOpen) 仓库（本应用位于 `apps/openwaifu`）
- Python 3（`export.sh` 首次运行会自动创建 `.venv` 并安装 SDK 工具链，需要网络）
- 硬件：T5AI 开发板（TUYA_T5AI_BOARD，3.5 寸 LCD）+ USB 串口线

### 1. 初始化环境

```bash
# 在 TuyaOpen 仓库根目录执行（每个新 shell 会话都需要 source 一次）
cd TuyaOpen
. ./export.sh

# 可选：检查工具链与子模块是否就绪
tos.py check
```

### 2. 选择板型配置

```bash
cd apps/openwaifu

# 交互式选择配置文件，选 config/TUYA_T5AI_BOARD_LCD_3.5.config
tos.py config choice

# 可选：需要微调配置项时再进菜单
tos.py config menu
```

默认的 `app_default.config` 面向 TUYA_T5AI_BOARD；带 3.5 寸 LCD 的板子请务必选择 `TUYA_T5AI_BOARD_LCD_3.5.config`，否则屏幕无显示。

### 3. 构建

```bash
tos.py build
```

首次构建会拉取 T5AI 平台 SDK，耗时较长；构建产物位于 `apps/openwaifu/dist/`（含 QIO 全量固件与 OTA 包）。

### 4. 烧录

```bash
# 接上 USB 串口线后烧录（不指定 -p 时会引导选择串口）
tos.py flash

# 指定串口与波特率（macOS 串口一般为 /dev/cu.usbserial-*）
tos.py flash -p /dev/cu.usbserial-XXXX -b 921600
```

烧录卡在等待设备时，按一下板上的复位（RST）键让设备进入下载模式。

### 5. 验证

```bash
# 查看设备日志（退出：Ctrl-C）
tos.py monitor -p /dev/cu.usbserial-XXXX
```

烧录成功后设备会：

1. 屏幕显示会话看板，左栏为桌宠形象，底部图例显示「请连接蓝牙」；
2. 以 `OpenWaifu` 名称开始 BLE 广播；
3. 电脑端启动 OpenWaifuD 后自动被连接，图例切换为状态说明，即部署完成。

后续部署顺序：先烧录本固件 → 再运行电脑端守护进程 OpenWaifuD → 最后接入 Agent 桥接（见各自仓库 README）。

## 配置说明

| 配置项                           | 说明                                                |
|----------------------------------|-----------------------------------------------------|
| `CONFIG_ENABLE_LIBLVGL=y`        | 启用 LVGL 图形库                                    |
| `CONFIG_LV_FONT_FMT_TXT_LARGE=y` | 启用大字符集字体支持（>10000 字符）                   |
| `CONFIG_LV_FONT_MONTSERRAT_40=y` | 启用 Montserrat 40 大号字体（单任务卡片的大号计时器） |

T5AI 板载默认已启用 Bluetooth / NimBLE / BT Service，无需额外配置。

> **注意**：应用内置了一份约 6000+ 常用字符的中文字体（基于阿里巴巴普惠体），
> 源文件约 3MB（编译后固件增量取决于实际链接的字形位图）。极生僻的汉字可能
> 无法显示。

## 电脑端客户端（macOS）

### 安装依赖

```bash
cd apps/openwaifu/tools
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 运行

1. 先烧录固件并确认设备屏幕显示 `BLE: Waiting...`
2. 在 Mac 上运行客户端：

```bash
python3 tools/ble_client.py
```

3. 客户端会自动扫描 `OpenWaifu`，连接后可输入消息：

```
msg> 你好，OpenWaifu！
[+] Sent 24 bytes: b'\xe4\xbd\xa0\xe5\xa5\xbd...'
```

4. 输入 `:quit` 或按 `Ctrl-C` 断开连接

### macOS 蓝牙权限

macOS 首次运行使用 CoreBluetooth 的程序时，系统会请求蓝牙权限，请在弹窗中
点击「允许」。如果使用的是自带 Python（非虚拟环境），可能需要授予终端应用
蓝牙权限（系统设置 → 隐私与安全性 → 蓝牙）。

## GATT 通信协议

| 项目         | 值                                     |
|--------------|----------------------------------------|
| 服务 UUID    | `0000fd50-0000-1000-0880-00805f9b34fb` |
| Write 特征   | `00000001-0000-1001-8001-00805f9b07d0` |
| 最大消息长度 | 240 字节（UTF-8 编码后）                 |
| 编码         | UTF-8                                  |

Notify 特征同时承载文本状态与二进制音频。二进制音频包均以 `OWA` 开头，
多字节整数使用小端序：

| 类型 | 字节布局 | 含义 |
|------|----------|------|
| 开始 `1` | `OWA, type, stream_id:u32, sample_rate:u16, bits:u8, channels:u8, reserved:u16` | 开始一次录音 |
| 数据 `2` | `OWA, type, stream_id:u32, sequence:u16, PCM...` | PCM 分片，最大 230 字节 |
| 结束 `3` | `OWA, type, stream_id:u32, pcm_bytes:u32, dropped_bytes:u32` | 结束录音并报告丢弃量 |

当前音频格式由板级音频配置上报，T5AI 默认是 16 kHz、16-bit、单声道 PCM。
单击设备按键后固定录制 5 秒，设备端不初始化或使用 VAD/KWS。
录音中的静音由 OpenWaifuD 的 Whisper VAD 在识别时过滤。

OpenWaifuD 的 Agent 回复通过 Write 特征以 `OWT` 二进制流下发：开始帧声明
PCM 格式与总长度，数据帧携带递增序号，结束帧确认长度。设备使用 PSRAM
环形缓冲边收边播；流 ID、序号、格式或长度不一致时丢弃该次播放。

### 两条泳道

同一条 Write 通道上复用两条泳道，按命令前缀区分，职责互不重叠：

- **泳道 1 · 会话列表（全量快照）**：守护进程周期性下发 `B → S…S → E`，固件据此将屏幕主体看板无闪烁地收敛到与快照一致。
- **泳道 2 · 全局事件（事件驱动）**：会话完成 / 出错 / 被用户取消等通过 `G` 命令即时下发，驱动屏幕**左下角的全局状态机**（瞬时展示后自动回落中性态）。

### 泳道 1 · 会话列表命令（全量快照）

字段用 `|` 分隔：

| 命令 | 格式                                          | 含义                                     |
|------|-----------------------------------------------|------------------------------------------|
| 开始 | `B`                                           | 快照同步开始（固件把现有会话标记为“未见”） |
| 更新 | `S\|<sid>\|<st>\|<elapsed>\|<plugin>\|<task>` | 新增或更新某会话                         |
| 结束 | `E`                                           | 快照同步结束（固件移除本轮未再出现的会话） |

`<st>` 为单字符状态码：`T`=思考中、`C`=编码中、`V`=测试中、`E`=出错、`I`=完成；
`<elapsed>` 为已运行秒数（固件在本地按秒继续跳动）。B/E 用于周期性快照对账。

### 泳道 2 · 全局事件命令（事件驱动）

独立于列表的即时通知，驱动左下角全局状态机：

| 命令 | 格式                | 含义                     |
|------|---------------------|--------------------------|
| 事件 | `G\|<ev>\|<detail>` | 全局事件，`detail` 可为空 |

`<ev>` 为单字符事件码：

| 事件码 | 状态   | 屏幕左下角     |
|--------|--------|----------------|
| `E`    | 出错   | ⚠ 出错（红色）   |
| `X`    | 已取消 | ✋ 已取消（橙色） |
| `D`    | 已完成 | 庆祝动画           |

固件收到事件后将左下角标签切到对应状态（可附带截断后的 detail），超过回落时长（默认 6 秒）后自动回“· 就绪”中性态。全局事件为瞬时态，不参与快照对账，也不在重连后重放。

## 目录结构

```
apps/openwaifu/
├── app_default.config              # 默认配置
├── CMakeLists.txt                  # 构建脚本（自动编译 src/*.c）
├── config/
│   └── TUYA_T5AI_BOARD_LCD_3.5.config
├── include/
│   ├── openwaifu_ble.h             # BLE 模块接口
│   ├── openwaifu_ui.h              # UI 模块接口
│   └── openwaifu_font.h            # 中文字体访问接口
├── src/
│   ├── openwaifu.c                 # 主程序（入口）
│   ├── openwaifu_ble.c             # BLE 从机实现（命令行 FIFO）
│   ├── openwaifu_ui.c              # LVGL 会话看板 + 情绪状态机
│   └── fonts/
│       └── openwaifu_font_puhui_18.c  # 中文字体数据
├── tools/
│   ├── ble_client.py               # macOS Python BLE 客户端（手动调试用）
│   └── requirements.txt            # Python 依赖
└── README.md                       # 本文件
```
