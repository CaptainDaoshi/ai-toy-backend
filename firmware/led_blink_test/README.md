# ESP32-S3 后端对话与 MAX98357A 语音测试

这是一个面向现有开发板的 ESP-IDF 6.0.2 测试工程。设备连接 2.4 GHz Wi-Fi 后，会通过 HTTPS 向 `https://68mn1ot67133.vicp.fun/v1/chat/completions` 发送一次“你是谁”，请求体包含稳定的 `device_id`。固件解析返回 JSON 的 `reply`，再调用 `/v1/speech`，把收到的 PCM WAV 边下载边通过 MAX98357A 播放，不会把整段语音装进 ESP32 内存。

根据资料中的 `YD-ESP32-S3-COREBOARD V1.4` 原理图和配套 Arduino 示例，板载可编程灯为一颗 WS2812B RGB LED，数据输入连接 GPIO48。它现在用作网络状态灯：

- 蓝色：正在连接 Wi-Fi。
- 绿色：后端请求及语音播放全部成功。
- 红色：Wi-Fi、后端鉴权、HTTPS、TTS 或语音播放失败。

默认亮度为 32/255，避免灯光刺眼并降低测试电流。板载 RGB 灯无需外部接线。

## MAX98357A 接线

固件默认按当前面包板接线配置：

| ESP32-S3 | MAX98357A |
| --- | --- |
| GPIO7 | DIN |
| GPIO15 | BCLK |
| GPIO16 | LRC / LRCLK / WS |
| 3V3 | VIN / VCC，并短接到 SD |
| GND | GND，并短接到 GAIN |

扬声器接在功放模块的 `SPK+` 和 `SPK-` 之间，`SPK-` 不能接 GND。GAIN 接地对应 12 dB 硬件增益，第一次上电时请让扬声器远离耳朵，并确认接线牢固。

播放使用标准 Philips I2S，不需要 MCLK。后端返回 16 位单声道 PCM WAV；固件读取 WAV 头中的实际采样率配置 I2S，并以 4 KiB 缓冲区流式播放。

## 为什么不控制旁边的 PWR、TX、RX 灯

- PWR 灯直接接在 3.3V 电源上，设备上电后常亮，不能由程序控制。
- TX、RX 灯连接 UART0 和 CH343P USB 转串口芯片。强行把对应引脚当普通输出使用会干扰烧录和串口日志，RX 线路还可能发生输出冲突。
- 因此测试使用 WS2812B 内部的红、绿、蓝三个发光通道交替显示。

资料中的通用引脚图把 `RGB_LED` 标在 GPIO38，但这块板的原理图和两个配套 Arduino Wi-Fi 示例均明确使用 GPIO48；本工程以相互一致的原理图和示例代码为准。

## 编译和烧录

工程默认目标已固定为 `esp32s3`。第一次联调先在后端项目目录安装新增语音依赖，然后重启正在运行的 uvicorn，让 `/v1/speech` 生效：

```powershell
cd C:\Users\26606\Documents\ai玩具
pip install -r requirements.txt
# 停止旧 uvicorn 后，按原来的命令重新启动服务
```

设备 ID 与令牌必须严格对应。默认设备 ID 是 `esp32-s3-001`，请在后端当前虚拟环境中生成它的令牌：

```powershell
python scripts\generate_device_token.py esp32-s3-001
```

只复制命令输出的 64 位设备令牌，不要把 `.env` 中的 `DEVICE_AUTH_SECRET` 写进固件。然后在已载入 ESP-IDF 6.0.2 环境的 PowerShell 中打开配置：

```powershell
cd C:\Users\26606\Documents\ai玩具\firmware\led_blink_test
.\idf.ps1 menuconfig
```

进入 `Wi-Fi Test Configuration`，填写：

- `Wi-Fi SSID`：路由器或手机热点名称。
- `Wi-Fi password`：Wi-Fi 密码；仅开放网络可以留空。
- `Maximum connection retries`：失败重试次数，默认 10。

再进入 `Backend API and Speech Test Configuration`，确认或填写：

- `Backend base URL`：默认已经是 `https://68mn1ot67133.vicp.fun`，末尾不要加 `/`。
- `Device ID`：默认 `esp32-s3-001`。
- `X-Device-Token`：粘贴为上面这个设备 ID 生成的令牌。
- `Optional global API bearer token`：后端 `.env` 设置了 `TOY_API_TOKEN` 才填写，否则留空。
- `One-shot test message`：默认“你是谁”。

`device_id` 会决定设备鉴权、固定回复模式和连续会话记忆。固件首次请求明确使用 `normal` 模式；如果这个 ID 已经被永久绑定成 `emotion` 模式，请改用一个新 ID，并重新生成对应令牌。

保存退出后编译并通过当前 COM3 端口烧录：

```powershell
.\idf.ps1 build
.\idf.ps1 -p COM3 flash monitor
```

按 `Ctrl+]` 退出串口监视器。

成功时板载 RGB 灯变绿，串口会看到类似日志：

```text
Got IPv4 address: 192.168.1.123
Wi-Fi connected: channel 6, RSSI -45 dBm
Sending test message to https://68mn1ot67133.vicp.fun as device_id 'esp32-s3-001': 你是谁
Backend reply: ...
Playing network speech: 24000 Hz, 16-bit mono, ... bytes, BCLK GPIO15, LRC GPIO16, DIN GPIO7
Network speech playback completed
Backend request and speech playback test passed
```

ESP32-S3 仅支持 2.4 GHz Wi-Fi。如果使用手机热点，请确认热点没有设置为“仅 5 GHz”。固件不会在日志中打印 Wi-Fi 密码。

首次编译时，ESP-IDF Component Manager 会下载 Espressif 官方 `led_strip` 组件。后续构建会使用缓存。

## 修改音频和状态灯参数

运行：

```powershell
.\idf.ps1 menuconfig
```

进入 `MAX98357A Audio Test Configuration`，可修改：

- DIN 数据脚，默认 GPIO7。
- BCLK 位时钟脚，默认 GPIO15。
- LRC/WS 左右声道时钟脚，默认 GPIO16。

进入 `On-board RGB LED Test Configuration`，可修改：

- WS2812B 数据引脚，默认 GPIO48。
- 状态灯亮度，默认 32/255。

## Windows 中文路径说明

ESP-IDF 6.0.2 的 Kconfig 工具在部分 Windows 环境中会把 UTF-8 路径错误解码。例如日志中的 `ai玩具` 变成 `ai鐜╁叿`，随后出现 `kconfigs.in not found`。这不是 LED 代码或 Python 环境的问题。

`idf.ps1` 会把固件源文件同步到 `%LOCALAPPDATA%\ai-toy-idf\led_blink_test`，再从该纯英文目录调用 `idf.py`。`idf.py` 的其他参数也可以原样传入：

```powershell
.\idf.ps1 menuconfig
.\idf.ps1 fullclean
.\idf.ps1 build
```

配置和编译产物会保留在该缓存目录中。Wi-Fi 密码以明文形式存在本机 staging 目录的 `sdkconfig` 中，但不会写入当前 Git 工作区；不要把该文件发给其他人。若以后把整个仓库移动到不含中文的路径，也可以直接使用普通的 `idf.py` 命令。
