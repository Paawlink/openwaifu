# OpenWaifu

OpenWaifu 是一个基于 TuyaOpen SDK 和 LVGL 9 的示例应用，运行在 T5AI 开发板上。
它同时扮演 **BLE Peripheral（从机）** 角色，电脑可通过蓝牙连接并向设备发送
UTF-8 文本消息（支持中文），消息会同时输出到串口调试日志并显示在 LCD 屏幕上。

## 功能

- BLE Peripheral，广播名称为 `OpenWaifu`
- 使用 TuyaOpen 标准 GATT 服务（V2）：
  - Service UUID：`0xFD50`（`0000fd50-0000-1000-0880-00805f9b34fb`）
  - Write 特征 UUID：`00000001-0000-1001-8001-00805f9b07d0`
  - Notify 特征 UUID：`00000002-0000-1001-8001-00805f9b07d0`
- 接收 UTF-8 文本消息（支持中文），自动校验 UTF-8 合法性
- 串口日志输出：数据长度、HEX 十六进制、UTF-8 文本
- LCD 屏幕显示：连接状态 + 最近一条消息（使用中文字体）
- 断开连接后自动恢复广播

## 固件构建

```bash
# 在仓库根目录初始化环境
. ./export.sh

tos.py config choice
# 注意选择正确的型号和lcd配置文件
tos.py config menu

# 构建 openwaifu（T5AI）
cd apps/openwaifu
tos.py build
```

构建产物位于 `apps/openwaifu/dist/`。

## 固件烧录

```bash

tos.py flash
```

## 配置说明

| 配置项                           | 说明                              |
|----------------------------------|-----------------------------------|
| `CONFIG_ENABLE_LIBLVGL=y`        | 启用 LVGL 图形库                  |
| `CONFIG_LV_FONT_FMT_TXT_LARGE=y` | 启用大字符集字体支持（>10000 字符） |

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
├── CMakeLists.txt                  # 构建脚本
├── config/
│   └── TUYA_T5AI_BOARD_LCD_3.5.config
├── include/
│   └── openwaifu_font.h            # 中文字体访问接口
├── src/
│   ├── openwaifu.c                 # 主程序（BLE + LVGL）
│   └── fonts/
│       └── openwaifu_font_puhui_18.c  # 中文字体数据
├── tools/
│   ├── ble_client.py               # macOS Python BLE 客户端
│   └── requirements.txt            # Python 依赖
└── README.md                       # 本文件
```
