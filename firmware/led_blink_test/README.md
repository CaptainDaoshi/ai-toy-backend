# ESP32-S3 麦克风对话完整链路测试

这是一个面向现有开发板的 ESP-IDF 6.0.2 测试工程。设备连接 2.4 GHz Wi-Fi 后先播报联网和麦克风检测结果。按住 GPIO39 按钮时，INMP441 录音以 HTTP chunked 方式实时上传；松开按钮立即结束录音。后端识别出的文字会携带稳定的 `device_id` 进入 `/v1/chat/completions`，回复随后提交给 `/v1/speech`，收到的 PCM WAV 边下载边通过 MAX98357A 播放。录音和播放都使用固定小缓冲区，不会把整段音频装进 ESP32 内存。

根据资料中的 `YD-ESP32-S3-COREBOARD V1.4` 原理图和配套 Arduino 示例，板载可编程灯为一颗 WS2812B RGB LED，数据输入连接 GPIO48。它现在用作网络状态灯：

- 蓝色：正在连接 Wi-Fi。
- 绿色：设备就绪。
- 青色常亮：正在按住说话和录音。
- 黄色常亮：录音已结束，正在识别或等待后端回答。
- 紫色缓慢呼吸：喇叭正在播放语音。
- 红色：Wi-Fi、麦克风、后端鉴权、HTTP、TTS 或语音播放失败。

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

## INMP441 接线与录音流程

| ESP32-S3 | INMP441 |
| --- | --- |
| GPIO4 | WS |
| GPIO5 | SCK / BCLK |
| GPIO6 | SD |
| 3V3 | VDD |
| GND | GND，并短接到 L/R |

INMP441 只能使用 3.3V 供电。L/R 接地选择左声道；固件以 16 kHz 读取 24 位有效数据。启动时保持安静约 2 秒，固件会检测有效 DMA slot 并播报“麦克风连接成功”。之后按住说话按钮即可立即录音，松开结束；默认最长 10 秒，用于防止按钮卡住：

```text
I (...) inmp441: Keep quiet for 2 seconds while the microphone slot is calibrated
I (...) inmp441: Microphone preparation passed: active SLOT1, calibration peak=...
I (...) wifi_test: Talk button pressed; recording until release
I (...) inmp441: RECORDING START: hold the talk button, maximum 10000 ms (active SLOT1, digital gain x4)
I (...) inmp441: Recording level: avg=... peak=..., .../... frames
I (...) inmp441: RECORDING END: captured ... PCM frames (... bytes)
I (...) backend: Recognized speech: 你是谁
```

若日志显示数据全为零，请依次检查 VDD、共地、L/R 接地以及 GPIO4/5/6。录音完成后固件会释放 I2S RX，再创建 GPIO7/15/16 的 I2S TX 播放通道，两套外设不会占用同一个 I2S 通道。

## 说话按钮与软电源按钮

根据 `YD-ESP32-S3-COREBOARD V1.4` 原理图，GPIO39 和 GPIO40 都直接引出到 J2 排针，没有连接板载外设。两者同时也是 JTAG 复用脚，使用这些按钮时不要连接外部 JTAG 调试器。

| ESP32-S3 | 按钮 | 接法 |
| --- | --- | --- |
| GPIO39 | 按住说话 | 常开按钮一端接 GPIO39，另一端接 GND |
| GPIO40 | 软开关机 | 常开按钮一端接 GPIO40，另一端接 GND |

固件启用内部上拉，因此不需要外接上拉电阻。按钮按下时为低电平，并带默认 30 ms 软件消抖。GPIO40 当前实现应用级软关机：灯灭、忽略说话按钮，但 Wi-Fi 和芯片仍然供电，以便再次按键唤醒。若产品必须真正切断电源，需要额外加入自锁电源或负载开关电路，不能只靠 GPIO 实现。

## 0.91 寸 I2C OLED 接线

固件按常见的 SSD1306 128×32 四针模块实现，并会自动探测 `0x3C` 和 `0x3D` 两个常用地址：

| ESP32-S3 | OLED |
| --- | --- |
| GPIO41 | SDA |
| GPIO42 | SCL / SCK |
| 3V3 | VCC |
| GND | GND |

屏幕显示设备状态：`WIFI CONNECTING`、`WIFI CONNECTED`、`MIC READY`、`LISTENING` 和 `ERROR`。录音结束、等待识别及大模型响应时，会显示眼睛左右移动、偶尔眨眼、三个等待点轮流跳动的思考表情；喇叭播放时会显示眨眼和嘴巴张合的说话表情。动画由独立低优先级任务刷新，因此后端 HTTP 请求阻塞时也能继续运行。进入软待机时 OLED 熄屏，唤醒后恢复。

显示驱动只使用一份 512 字节静态帧缓冲和精简英文字体，动画任务是唯一的 I2C 写入者，避免并发访问。静态页面不会持续刷新；若启动时未检测到 OLED，只会输出错误日志，语音与按钮流程仍可继续运行。

## 为什么不控制旁边的 PWR、TX、RX 灯

- PWR 灯直接接在 3.3V 电源上，设备上电后常亮，不能由程序控制。
- TX、RX 灯连接 UART0 和 CH343P USB 转串口芯片。强行把对应引脚当普通输出使用会干扰烧录和串口日志，RX 线路还可能发生输出冲突。
- 因此测试使用 WS2812B 内部的红、绿、蓝三个发光通道交替显示。

资料中的通用引脚图把 `RGB_LED` 标在 GPIO38，但这块板的原理图和两个配套 Arduino Wi-Fi 示例均明确使用 GPIO48；本工程以相互一致的原理图和示例代码为准。

## 编译和烧录

工程默认目标已固定为 `esp32s3`。第一次联调先在后端项目目录安装语音依赖、下载 Vosk 中文模型，再重启正在运行的 uvicorn，让 `/v1/audio/transcriptions` 和 `/v1/speech` 生效：

```powershell
cd C:\Users\26606\Documents\ai玩具
pip install -r requirements.txt
# 按仓库根目录 README 下载模型到 %LOCALAPPDATA%\ai-toy-vosk
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

- `Backend base URL`：确认是 `https://ni102fd610004.vicp.fun`，末尾不要加 `/`。
- `Device ID`：默认 `esp32-s3-001`。
- `X-Device-Token`：粘贴为上面这个设备 ID 生成的令牌。
- `Optional global API bearer token`：后端 `.env` 设置了 `TOY_API_TOKEN` 才填写，否则留空。

`device_id` 会决定设备鉴权、固定回复模式和连续会话记忆。固件首次请求明确使用 `normal` 模式；如果这个 ID 已经被永久绑定成 `emotion` 模式，请改用一个新 ID，并重新生成对应令牌。

花生壳免费映射可能在新客户端首次访问时返回需要浏览器等待和执行 JavaScript 的 HTML 引导页。ESP32 不会执行网页脚本，因此同一局域网内联调时建议把 `Backend base URL` 暂时设为 Caddy 的局域网入口，例如当前电脑使用 `http://192.168.100.31:8080`。正式离开局域网使用时，需要换成不会插入 HTML 页的 API 映射或升级花生壳服务。

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
Talk button pressed; recording until release
RECORDING START: hold the talk button, maximum 10000 ms (active SLOT1, digital gain x4)
Recognized speech: 你是谁
Sending recognized text to https://ni102fd610004.vicp.fun as device_id 'esp32-s3-001': 你是谁
Backend reply: ...
Playing network speech: 24000 Hz, 16-bit mono, ... bytes, BCLK GPIO15, LRC GPIO16, DIN GPIO7
Network speech playback completed
Backend request and speech playback test passed
```

ESP32-S3 仅支持 2.4 GHz Wi-Fi。如果使用手机热点，请确认热点没有设置为“仅 5 GHz”。固件不会在日志中打印 Wi-Fi 密码。

首次编译时，ESP-IDF Component Manager 会下载 Espressif 官方 `led_strip` 组件。后续构建会使用缓存。

完整固件已超过 ESP-IDF 默认 1 MB factory 分区，因此工程通过 `sdkconfig.defaults` 使用 1500 KB 的单应用分区。烧录时必须执行完整的 `flash`，让新的分区表和应用同时写入；不要只执行 `app-flash`。

## 修改音频和状态灯参数

运行：

```powershell
.\idf.ps1 menuconfig
```

进入 `MAX98357A Audio Test Configuration`，可修改：

- DIN 数据脚，默认 GPIO7。
- BCLK 位时钟脚，默认 GPIO15。
- LRC/WS 左右声道时钟脚，默认 GPIO16。

进入 `INMP441 Microphone Test Configuration`，可修改：

- WS 数据选择脚，默认 GPIO4。
- SCK/BCLK 时钟脚，默认 GPIO5。
- SD 麦克风数据输入脚，默认 GPIO6。
- 采样率，默认 16 kHz。
- 按键录音最长保护时间，默认 10 秒。
- 24 位到 16 位转换的数字增益，默认 4 倍；若 `peak` 经常达到 32767，请调低。

进入 `Push-to-talk and Power Buttons`，可修改：

- 按住说话按钮，默认 GPIO39。
- 软开关机按钮，默认 GPIO40。
- 按键消抖时间，默认 30 ms。

进入 `On-board RGB LED Test Configuration`，可修改：

- WS2812B 数据引脚，默认 GPIO48。
- 状态灯亮度，默认 32/255。

进入 `0.91-inch SSD1306 OLED Display`，可修改：

- SDA 数据脚，默认 GPIO41。
- SCL/SCK 时钟脚，默认 GPIO42。
- I2C 时钟，默认 400 kHz；长线或显示不稳定时可降至 100 kHz。

## Windows 中文路径说明

ESP-IDF 6.0.2 的 Kconfig 工具在部分 Windows 环境中会把 UTF-8 路径错误解码。例如日志中的 `ai玩具` 变成 `ai鐜╁叿`，随后出现 `kconfigs.in not found`。这不是 LED 代码或 Python 环境的问题。

`idf.ps1` 会把固件源文件同步到 `%LOCALAPPDATA%\ai-toy-idf\led_blink_test`，再从该纯英文目录调用 `idf.py`。`idf.py` 的其他参数也可以原样传入：

```powershell
.\idf.ps1 menuconfig
.\idf.ps1 fullclean
.\idf.ps1 build
```

配置和编译产物会保留在该缓存目录中。Wi-Fi 密码以明文形式存在本机 staging 目录的 `sdkconfig` 中，但不会写入当前 Git 工作区；不要把该文件发给其他人。若以后把整个仓库移动到不含中文的路径，也可以直接使用普通的 `idf.py` 命令。
