# AI Passport 农场桥接

把 **AI Passport（ESP32-C3）工牌**变成一个挂在胸前的「3D 打印农场遥控器」。

桥接服务跑在你的电脑上，实时读取拓竹（Bambu Lab）打印机农场的状态，
通过 **USB 串口**或 **WiFi** 把数据推给工牌；工牌用 240×320 彩屏显示每台
机器的进度与状态，异常时响铃告警，还能直接在工牌上下发打印任务。

## 能干什么

- **33 台一屏掌握**：打印中 / 空闲 / 暂停 / 失败 / 离线，逐级下钻到单机
- **实时事件推送**：状态跳变立刻刷新，HMS 故障码红色高亮
- **声音告警**：完成 / 异常两种音效，5 档音量（含「超大」档）
- **反向下发**：在工牌上直接给空闲机器派发排队中的任务
- **托盘常驻**：右下角彩色圆点一眼看健康度，右键可暂停/重启服务
- **双链路**：优先 WiFi，自动回退 USB 串口；支持 OTA 无线升级固件

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-C3（8MB Flash，无 PSRAM，RV32IMC **无硬件 FPU**） |
| 屏幕 | ST7789P3 240×320 TFT |
| 音频 | ES8311 codec（I2S：DOUT=2 / WS=3 / DIN=4 / BCLK=5 / MCLK=6） |
| 连接 | WiFi（UDP 发现 + TCP 8897）或 USB 串口 |

> ⚠️ ESP32-C3 **没有 FPU**，`float`/`double` 全靠软件模拟。音频合成必须走
> 整数查表 + 定点运算，否则 I2S DMA 会被饿死，表现为声音卡顿/沙哑。

## 快速开始

### 1. 安装依赖

```bash
npm install
```

### 2. 配置 WiFi（工牌联网用）

```bash
cd firmware/aipassport_farm
cp wifi_config.h.example wifi_config.h
```

然后编辑 `wifi_config.h`，填入你自己的 WiFi：

```cpp
#define WIFI_SSID   "你的WiFi名"
#define WIFI_PASS   "你的WiFi密码"
```

> - `wifi_config.h` 已在 `.gitignore` 中，**真实密码不会进仓库**。
> - 即使不创建它，固件也**照样能编译**（会回退到占位符，只是连不上网）。
> - 既没下发过、编译期也是占位符时，工牌不会瞎连，会提示"未配置 WiFi"。

**烧录后想换 WiFi？不用改代码、不用重新烧录** —— 让桥接直接下发：

```bash
node tools/set_wifi.js <WiFi名> <密码>

# 或者不带参数，交互式输入（密码不回显）：
node tools/set_wifi.js
```

工牌收到后把凭据存进 **NVS（掉电不丢）**，并立刻用新凭据重连。
**凭据优先级：桥接下发的（NVS） > 编译期的 `wifi_config.h`**。

> 换 WiFi 时无线会先断开；此时桥接会自动改走 **USB 串口**通道下发，
> 所以建议保留 USB 连接作为兜底。

### 3. 烧录固件

用 [Arduino CLI](https://arduino.github.io/arduino-cli/)（体积较大，未随仓库提供）：

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,CPUFreq=160 \
  --output-dir build firmware/aipassport_farm

arduino-cli upload -p COMx --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,CPUFreq=160 --input-dir build
```

Windows 用户可直接双击 `一键烧录.bat`。

改了界面上的中文？**记得重跑字库生成**，否则显示方块：

```bash
npm run font      # python tools/genfont.py
```

### 4. 启动桥接

**Windows 托盘模式（推荐）** —— 双击 `启动桥接.bat`，右下角出现彩色圆点：

| 颜色 | 含义 |
|---|---|
| 🟢 绿 | 正常：桥接在跑、工牌在线 |
| 🟠 橙 | 桥接在跑，但工牌没连上（检查工牌 WiFi / 是否开机） |
| 🔵 蓝灰 | 已手动暂停 |
| ⚪ 灰 | 桥接没在运行（会自动重试拉起） |

右键菜单：**暂停服务 / 恢复服务 / 重启服务**、打开状态页、查看运行日志、
开机自动启动、退出。

**命令行模式**：

```bash
npm start                 # 或 node server/bridge.js
```

状态页：<http://127.0.0.1:8899/state>

> ⚠️ 想让打印机列表正常显示，**Bambu Farm Manager 客户端需要处于运行状态**——
> 桥接从它的进程内存中提取 API 凭据（JWT / sec-ts）。客户端没开时会看到 0 台设备。

## 目录结构

```
server/       桥接服务（Node.js）：农场 API 客户端、串口/无线收发、HTTP 状态接口
firmware/     ESP32-C3 工牌固件（含字库、图标生成脚本）
tools/        工具集：字库生成、OTA 升级、音效预览、桌面快捷方式
credentials/  【不入库】mTLS 证书链与私钥，需自行准备
```

## 已知限制

- 打印机数据依赖 Bambu Farm Manager 客户端运行并从中提取凭据。
- ESP32-C3 无 FPU，音频与图形都要避开浮点热路径。
- 农场 API 为本地逆向接口，厂商更新客户端后可能需要重新适配。

## 免责声明

- 本项目以逆向方式调用拓竹农场本地 API，**仅供个人学习与管理自有设备使用**。
- `credentials/` 下的 mTLS 私钥属于高度敏感信息，**请勿分享或提交到任何仓库**。
- 请遵守当地法律法规及设备厂商的服务条款，勿用于未授权的设备。

## License

GPL-3.0 —— 详见 [LICENSE](LICENSE)。
