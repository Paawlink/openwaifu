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
- 串口日志输出：数据长度、HEX 十六进制、UTF-8 文本
- LCD 会话看板：每个活跃会话固定占一行/一张卡片，实时显示状态、任务与本地跳秒的运行时长
- 情绪状态机：按活跃会话数量切换（0=睡觉中、1=摸鱼中、2-3=认真搬砖、4-6=火力全开、>6=要炸了）
- 左下角全局事件状态机（泳道 2）：收到出错/取消事件时瞬时展示，数秒后自动回落中性态
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

## 固件构建

```bash
# 在仓库根目录初始化环境
. ./export.sh

# 注意选择正确的型号和lcd配置文件
cd apps/openwaifu
tos.py config choice
tos.py config menu

# 构建 openwaifu（T5AI）
tos.py build
```

构建产物位于 `apps/openwaifu/dist/`。

## 固件烧录

```bash

tos.py flash
```

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

### 两条泳道

同一条 Write 通道上复用两条泳道，按命令前缀区分，职责互不重叠：

- **泳道 1 · 会话列表（全量快照）**：守护进程周期性下发 `B → S…S → E`，固件据此将屏幕主体看板无闪烁地收敛到与快照一致。
- **泳道 2 · 全局事件（事件驱动）**：会话出错 / 被用户取消等通过 `G` 命令即时下发，驱动屏幕**左下角的全局状态机**（瞬时展示后自动回落中性态）。

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

固件收到事件后将左下角标签切到对应状态（可附带截断后的 detail），超过回落时长（默认 6 秒）后自动回“· 就绪”中性态。全局事件为瞬时态，不参与快照对账，也不在重连后重放。

## 串口日志示例

收到消息后，串口会输出类似如下日志：

```
TUYA N BLE received 24 bytes (conn 0x0000)
BLE RX HEX: E4 BD A0 E5 A5 BD EF BC 8C 4F 70 65 6E 57 61 69 66 75 EF BC 81
TUYA N BLE RX UTF-8: 你好，OpenWaifu！
```

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
