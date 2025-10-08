# 🎤 ESP32-S3 语音命令识别系统

> **基于豆包AI的智能语音助手** | 支持语音唤醒和实时对话

## 🌟 项目简介

这是一个完整的智能语音助手实现，基于ESP32-S3开发板和豆包AI语音服务。系统支持语音唤醒和实时语音对话功能。

当你说"你好小智"时，系统会播放欢迎音频并进入对话模式。你可以与AI进行自然语言对话，系统会将AI的语音回应实时播放出来。

## ⚡ 核心功能

- ✅ **语音唤醒检测** - 支持"你好小智"唤醒词
- ✅ **实时语音对话** - 与豆包AI进行实时语音对话
- ✅ **音频流处理** - 实时音频录制和播放
- ✅ **WebSocket通信** - 与服务器进行实时数据传输
- ✅ **智能网络管理** - 自动连接WiFi和重连机制

## 📦 硬件清单

| 部件   | 推荐型号           | 备注                       |
| ------ | ------------------ | -------------------------- |
| 开发板 | ESP32-S3-DevKitC-1 | 必须是 S3 版本，需要 PSRAM |
| 麦克风 | INMP441            | 约 5 元/个                 |
| 功放   | MAX98357A          | 约 8 元/个                 |
| 喇叭   | 4Ω 3W 小喇叭       | 约 3 元/个                 |

## 🔌 硬件接线

```text
麦克风(INMP441) → ESP32开发板
-----------------------------
VDD（麦克风）→ 3.3V（开发板）  // 接电源正极
GND（麦克风）→ GND（开发板）   // 接电源负极
SD  （麦克风）→ GPIO6         // 数据线
WS  （麦克风）→ GPIO4         // 左右声道选择
SCK （麦克风）→ GPIO5         // 时钟线

功放(MAX98357A) → ESP32开发板
-----------------------------
VIN（功放）→ 3.3V（开发板）   // 接电源正极
GND（功放）→ GND（开发板）    // 接电源负极
DIN（功放）→ GPIO7           // 音频数据输入
BCLK（功放）→ GPIO15         // 位时钟
LRC（功放）→ GPIO16          // 左右声道时钟

喇叭连接
--------
喇叭正极 → 功放 +
喇叭负极 → 功放 -
```

## 🚀 快速开始

### 1. 环境准备

确保已安装 ESP-IDF 开发环境，推荐版本为 v5.0 或更高。

### 2. 配置项目

编辑 `main/project_config.h` 文件，配置WiFi信息和服务器地址：

```c
// WiFi配置
#define CONFIG_EXAMPLE_WIFI_SSID "你的WiFi名称"
#define CONFIG_EXAMPLE_WIFI_PASSWORD "你的WiFi密码"

// WebSocket服务器配置
#define CONFIG_EXAMPLE_WEBSOCKET_URI "ws://你的服务器IP:8888"
```

### 3. 配置唤醒词模型

默认唤醒词是“你好，小智”

```bash
idf.py menuconfig
```

进入 `ESP Speech Recognition` → `Load Multiple Wake Words` 选择唤醒词模型，例如：
- `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS` (你好小智)

### 4. 编译和烧录

```bash
# 编译项目
idf.py build

# 连接开发板并烧录
idf.py flash

# 查看串口输出
idf.py monitor
```

## 🖥️ 服务器端配置

### 运行语音服务器

使用 Python 3.9+ 运行服务器：

```bash
# 安装依赖
pip install -r server/requirements.txt

# 运行服务器
python server/server.py
```

服务器将监听 8888 端口，ESP32 将连接到此服务器进行语音对话。

### 服务器功能

- 与豆包AI建立实时语音对话连接
- 处理ESP32发送的音频数据
- 将豆包AI的语音回应重采样后发送回ESP32
- 支持流式音频传输，实现实时对话体验

## 🎯 使用方法

### 基本语音交互流程

1. **唤醒阶段**：对着麦克风说"你好小智"
   - 系统检测到唤醒词后播放欢迎音频
   - 自动连接到语音服务器并开始对话模式

2. **对话阶段**：与AI进行自然语言对话
   - 说话时系统会自动识别语音并发送到豆包AI
   - AI的回应会实时播放出来

3. **退出**：停止说话几秒钟后自动结束对话

### 系统状态指示

- **等待唤醒**：串口显示"等待唤醒词 '你好小智'"
- **对话模式**：串口显示"WebSocket已连接"和音频数据传输信息
- **连接错误**：串口显示连接失败信息并尝试重连

## ⚙️ 自定义配置

### 更换唤醒词

通过 `idf.py menuconfig` 在 `ESP Speech Recognition` → `Load Multiple Wake Words` 中选择其他唤醒词。

### 调整检测灵敏度

修改 `main/main.cc` 中的检测模式：
- `DET_MODE_95` - 最严格（减少误触发）
- `DET_MODE_90` - 推荐值（平衡型）
- `DET_MODE_80` - 最宽松（提高检测率）

### 修改WiFi和服务器配置

编辑 `main/project_config.h` 文件中的配置参数。

## 📁 项目结构

```text
├── main/                    # 主程序源代码
│   ├── main.cc             # 主程序入口
│   ├── bsp_board.cc/h      # 硬件抽象层
│   ├── audio_manager.cc/h  # 音频管理器
│   ├── websocket_client.cc/h # WebSocket客户端
│   ├── wifi_manager.cc/h   # WiFi管理器
│   ├── project_config.h    # 项目配置文件
│   └── mock_voices/        # 音频文件目录
├── server/                 # 服务器端代码
│   └── server.py           # 语音对话服务器
├── tools/                  # 工具脚本
└── managed_components/     # ESP-IDF管理的组件
```

## 📜 开源协议

Apache 2.0 - 可自由用于个人/商业项目，**注明原作者即可**

## 🆘 常见问题

请查看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md) 文件获取常见问题的解决方案。