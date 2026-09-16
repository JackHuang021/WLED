# WLED 软件架构与运行流程

> 本文基于仓库当前检出版本（`17.0.0-devV5`，`VERSION 2607201`，代号 `Kagayaki`）逐文件分析整理。
> 所有引用采用 `路径:行号` 形式，便于跳转核对。

---

## 目录

1. [总览](#1-总览)
2. [代码结构与模块划分](#2-代码结构与模块划分)
3. [构建系统](#3-构建系统)
4. [启动流程 `setup()`](#4-启动流程-setup)
5. [主循环 `loop()`](#5-主循环-loop)
6. [核心数据模型](#6-核心数据模型)
7. [渲染管线：一帧的生命周期](#7-渲染管线一帧的生命周期)
8. [网络子系统](#8-网络子系统)
9. [配置、持久化与 API](#9-配置持久化与-api)
10. [Usermod 插件机制](#10-usermod-插件机制)
11. [端到端典型流程](#11-端到端典型流程)
12. [扩展点与开发注意事项](#12-扩展点与开发注意事项)

---

## 1. 总览

WLED 是运行在 ESP8266 / ESP32 系列 MCU 上的可寻址 LED 控制固件。它在单一 `loop()` 任务中协程式地完成了 LED 渲染、Wi-Fi 管理、HTTP 服务、十余种实时灯光协议的收发、配置持久化和插件调度。

**技术栈**

| 层面 | 选用 | 说明 |
|---|---|---|
| 语言 / 框架 | C++ (Arduino framework) | 全部环境都是 `framework = arduino`，包括 V4/V5 "IDF" 目标 —— 区别是所捆绑的 arduino-esp32 / IDF 版本，而非框架本身 |
| 构建 | PlatformIO | 43 个 `[env:*]`，通过 ini 段继承组织 |
| 像素输出 | **NeoPixelBus** (Makuna) | 唯一的输出驱动。FastLED 仅保留精简版用于 `CRGB` / 调色板 / HSV |
| HTTP / WS | ESPAsyncWebServer（WLED 定制 fork） | 同时承载 `/settings`、`/json`、OTA、Alexa、WebSocket |
| JSON | ArduinoJson v6（vendored 在 `wled00/src/dependencies/json/`） | 全局单例 `pDoc` + 互斥锁 |
| 文件系统 | LittleFS (`WLED_FS`) | ESP8266/ESP32 统一 |
| 前端 | 原生 HTML/JS（`wled00/data/`） | 由 Node 脚本预压缩内嵌为 `html_*.h` |

**核心设计取向**

- **单线程 + 协作式调度**：没有 RTOS 任务，没有像素缓冲锁。所有耗时操作靠"状态机 + 分帧"来避免阻塞，需要修改几何/配置时用 `strip.suspend()` / `waitForIt()` 让出。
- **全局变量即状态**：所有状态定义在 [`wled00/wled.h`](../wled00/wled.h) 中，用 `WLED_GLOBAL` / `_INIT()` 宏对，并在唯一的 `wled.cpp` 中 `#define WLED_DEFINE_GLOBAL_VARS` 落地。
- **前端资源编入 Flash**：HTML/JS/CSS 预压缩成 gzip 字节数组放进 PROGMEM，运行时直接 `beginResponse_P` 送出，不占文件系统。
- **插件靠链接器发现**：Usermod 通过自定义 ELF section 自注册，无中心注册表。

---

## 2. 代码结构与模块划分

```
wled00/                    固件源码（src_dir），扁平组织
├── wled_main.cpp          Arduino 入口：setup() / loop() → WLED::instance()
├── wled.h                 全局变量、宏、平台抽象、外部声明（"总头文件"）
├── wled.cpp               WLED 类：setup() / loop() / 连接状态机
├── const.h                编译期常量（总线数、段数、端口、枚举）
├── fcn_declare.h          全局函数与 Usermod 基类声明
├── colors.{h,cpp}         颜色模型、gamma、调色板类型、色温换算
├── palettes.cpp           PROGMEM 调色板数据表
│
├── FX.{h,cpp}             特效函数库（~220 个效果）+ mode 数据表
├── FX_fcn.cpp             WS2812FX / Segment 的运行时逻辑（service、show、过渡）
├── FX_2Dfcn.cpp           2D 矩阵绘制原语、ledmap 构建
├── FXparticleSystem.{h,cpp} 粒子物理引擎（2D/1D）
│
├── bus_manager.{h,cpp}    Bus 抽象层（BusDigital/BusPwm/BusOnOff/BusNetwork/Hub75）
├── bus_wrapper.h          PolyBus：NeoPixelBus 的类型擦除封装
├── pin_manager.{h,cpp}    GPIO / LEDC 通道的全局分配器
├── led.cpp                亮度、过渡、夜间灯、状态收敛
├── overlay.cpp            叠加层（模拟时钟）绘制回调
│
├── network.cpp            Wi-Fi / Ethernet 连接管理、事件回调
├── wled_server.cpp        HTTP 路由表与处理器
├── json.cpp               状态/信息序列化 + /json 分发
├── set.cpp                传统 /win API + /settings 表单处理
├── cfg.cpp                配置读写与文件持久化
├── xml.cpp                传统 XML 响应 + 设置页 JS 生成
├── file.cpp               JSON 文件"就地补丁"键值存储引擎
├── presets.cpp / playlist.cpp  预设与播放列表
├── udp.cpp                实时协议入口、notify/sync、节点发现
├── e131.cpp               E1.31 / Art-Net / DDP 解析与扇出
├── ws.cpp                 WebSocket（JSON + 二进制实时流）
├── mqtt.cpp / alexa.cpp / hue.cpp / ntp.cpp / ir.cpp / dmx_*.cpp / remote.cpp / improv.cpp
├── ota_update.cpp         Web OTA + bootloader 更新 + 元数据校验
├── usermod.cpp / um_manager.cpp  Usermod 调度
├── wled_metadata.cpp     编译期注入的版本元数据结构
│
├── src/                   不参与编译的 vendored 依赖与字体（被 #include 进来）
├── data/                  前端源码（tab 缩进；`npm run build` 生成 html_*.h）
└── html_*.h / js_*.h      自动生成，**禁止手工编辑或提交**

usermods/                  社区插件（每个含 library.json，libArchive 必须为 false）
lib/                       ESP8266PWM、NeoESP32RmtHI、wled_espnow
platformio.ini             构建配置
pio-scripts/               SCons 构建钩子
tools/                     前端打包（cdata.js）等
```

**分层视图**

```
                    ┌──────────────────────────────────────┐
   输入源            │ HTTP / WS / MQTT / UDP / E1.31 /     │
                    │ Art-Net / DDP / IR / 按钮 / 定时器    │
                    └───────────────┬──────────────────────┘
                                    │ 都收敛到 deserializeState() / applyPreset()
                    ┌───────────────▼──────────────────────┐
   状态层            │ 全局变量（wled.h）：bri, colPri,        │
                    │ strip(Segment[]), realtimeMode, ...   │
                    └───────────────┬──────────────────────┘
                                    │ strip.service() 每帧读取
                    ┌───────────────▼──────────────────────┐
   渲染层            │ 效果函数 → Segment::pixels           │
                    │ blendSegment() → WS2812FX::_pixels    │
                    └───────────────┬──────────────────────┘
                                    │ gamma / CCT / ledmap
                    ┌───────────────▼──────────────────────┐
   输出层            │ BusManager → Bus → PolyBus →        │
                    │ NeoPixelBus → RMT / I2S / SPI / PWM  │
                    └──────────────────────────────────────┘
```

---

## 3. 构建系统

### 3.1 PlatformIO 段继承

`platformio.ini` 用文本插值 `${section.key}` 组织成继承树：

```
[platformio]          default_envs(43 个 CI 目标)、src_dir、extra_configs
[common]              debug_flags、跨平台 build_flags、default_usermods
[scripts_defaults]    extra_scripts 列表
[env]                 基类：framework=arduino、lib_deps、monitor_speed
├── [esp8266]         espressif8266@4.2.1
├── [esp32_idf_V4]    Tasmota platform 2024.06.00 → arduino-esp32 2.0.18 / IDF 4.4.8
├── [esp32_idf_V5]    **默认** Tasmota platform 2026.05.50 → arduino core 3.3.8 / IDF 5.5.4
└── [esp32s2|c3|c5|c6|p4|s3]  各 MCU 的 CONFIG_IDF_TARGET_* 定义
[env:<board>]         43 个具体板型
```

`extra_configs` 会额外加载 `platformio_override.ini`（本地，gitignore）用于个性化构建 —— 当前该文件只做一件事：把 `default_envs` 钉在 `esp32c3dev_qio`，以免 PlatformIO 用列表首项（ESP8266 的 `nodemcuv2`）生成 `.vscode/c_cpp_properties.json` 导致 IntelliSense 认错架构。

**注意**：并不存在纯 ESP-IDF 构建路径。`[esp32_idf_V4]` / `[esp32_idf_V5]` 中的 "IDF" 指的是所捆绑的 ESP-IDF 版本，二者都是 `framework = arduino`。

### 3.2 关键编译期常量

多数常量集中在 [`wled00/const.h`](../wled00/const.h)，全部用 `#ifndef` 包裹，构建 flag 可覆盖。

| 常量 | 默认值 | 位置 |
|---|---|---|
| `WLED_MAX_BUSSES` | `WLED_MAX_DIGITAL_CHANNELS + WLED_MAX_ANALOG_CHANNELS`（`constexpr`，非宏，非数组尺寸） | [`const.h:122-129`](../wled00/const.h#L122-L129) |
| `WLED_MAX_DIGITAL_CHANNELS` | RMT + I2S：ESP32 8+8，S3 4+8，C3 2+0，ESP8266 0+0 | [`const.h:65-121`](../wled00/const.h#L65-L121) |
| `MAX_LEDS` | ESP8266 1536，S2 2048，C3/C5/C6 4096，ESP32/S3/P4 16384 | [`const.h:565-575`](../wled00/const.h#L565-L575) |
| `MAX_LED_MEMORY` | ESP8266 8 KB … S3/P4 192 KB | [`const.h:578-596`](../wled00/const.h#L578-L596) |
| `MAX_LEDS_PER_BUS` | 2048 | [`const.h:598-600`](../wled00/const.h#L598-L600) |
| `MAX_NUM_SEGMENTS` | ESP8266 16，S2 32，其他 32（有 PSRAM 时 64） | [`FX.h:84-103`](../wled00/FX.h#L84-L103) |
| `MAX_SEGMENT_DATA` | ESP8266 6 KB，S2 20 KB，其他 64 KB | 同上 |
| `MODE_COUNT` | 220 | [`FX.h:377`](../wled00/FX.h#L377) |
| `WLED_FPS` / `FRAMETIME_FIXED` | 42 fps / 23 ms | [`FX.h:61-62`](../wled00/FX.h#L61-L62) |
| `WLED_MAX_USERMODS` | ESP8266/S2 为 4，其他 6 | [`const.h:57-63`](../wled00/const.h#L57-L63) |
| `WLED_NUM_PINS` / `WLED_MAX_BUTTONS` | ESP32 按钮上限 32 | [`const.h:141-152`](../wled00/const.h#L141-L152) |

`PIXEL_COUNTS`、`DATA_PINS`、`LED_TYPES`、`DEFAULT_LED_COLOR_ORDER` 不是全局上限，而是 `cfg.cpp` 局部概念 —— 首次启动时写入默认配置用的（[`cfg.cpp:9-22`](../wled00/cfg.cpp#L9-L22)）。

用户侧编译期开关是 [`wled00/my_config.h`](../wled00/my_config.h)（`WLED_USE_MY_CONFIG` 在 `[common]` 全局定义），由 `pio-scripts/user_config_copy.py` 从 `my_config_sample.h` 自动生成（仅在不存在时），可设 `CLIENT_SSID` / `CLIENT_PASS` / `MAX_LEDS` / `MDNS_NAME`。

### 3.3 pio-scripts（SCons 钩子）

按 `extra_scripts` 顺序执行：

| 脚本 | 阶段 | 作用 |
|---|---|---|
| `set_metadata.py` | pre | 把 `package.json` 的版本与 git remote 解析出的仓库名作为 `CPPDEFINES` 只注入 `wled_metadata.cpp` |
| `user_config_copy.py` | pre | 生成 `my_config.h`（若缺失） |
| `load_usermods.py` | pre | **Usermod 发现与注册**：解析 `custom_usermods`，`*` 展开为所有含 `library.json` 的子目录；把每个 mod 以 `symlink://` 加入 `lib_deps`；monkey-patch 库构建器为其追加 `wled00` 头文件路径；**若某 usermod 的 `libArchive` 非 false 则直接 `Exit(1)`** |
| `build_ui.py` | pre | 执行 `npm ci && npm run build`，生成 `html_*.h` / `js_*.h`。**缺失 Node 或构建失败会中断固件构建** |
| `output_bins.py` | post | 拷贝产物到 `build_output/`，附带 `.map` |
| `strip-floats.py` | post | 从 `LINKFLAGS` 移除 `-u _printf_float` / `-u _scanf_float` 以省 Flash |
| `dynarray.py` | post | **链接脚本补丁**：注入 `KEEP(*(SORT_BY_INIT_PRIORITY(.dynarray.*)))`，保证 usermod 注册段不被 GC。ESP32 改 `sections.ld`，ESP8266 改 `local.eagle.app.v6.common.ld` |
| `validate_modules.py` | post | **链接后校验**：用 `readelf --debug-dump=info` 比对 ELF 中每个 usermod 的编译单元；任何模块没进最终镜像 → 构建失败 |

### 3.4 前端构建

`wled00/data/` 下的 HTML/JS/CSS 是源文件（**tab 缩进**）。`tools/cdata.js` 负责打包、gzip 压缩并生成为 C 头文件：

```bash
npm ci && npm run build     # → wled00/html_*.h, wled00/js_*.h
npm run dev                 # watch 模式
```

生成的 `html_*.h` / `js_*.h` **不要手工编辑或提交**。因为 `pio run` 依赖它们，构建固件前必须先跑一次前端构建。

---

## 4. 启动流程 `setup()`

```
setup()                                   wled_main.cpp:18
└─ WLED::instance().setup()               wled.cpp:388
   ├─ 关 brownout 检测 / RX 引脚下拉 / Serial.begin(115200)
   ├─ 打印版本、芯片、Flash 信息
   ├─ BOARD_HAS_PSRAM 时把 JSON 缓冲 (pDoc) 分配到 PSRAM（2×JSON_BUFFER_SIZE）
   ├─ WLED_FS.begin(true)  挂载 LittleFS（ESP32 失败即格式化）
   ├─ handleBootLoop()      检查连续崩溃并按等级恢复
   ├─ initPresetsFile()     确保 /presets.json 存在
   ├─ updateFSInfo()
   ├─ 取 MAC → escapedMac（ESP32 上 macAddress() 全 0 时回退读 eFuse）
   ├─ WLED_SET_AP_SSID()
   ├─ verifyConfig() → 失败则 restoreConfig() → 再失败 resetConfig()
   ├─ deserializeConfigFromFS()      ★ 读 /cfg.json 与 /wsec.json 并**立即生效**
   │    └─ 内部会调用 initEthernet()  （配置一旦读出就建以太网）
   ├─ beginStrip()                   ★ LED 子系统初始化
   │    ├─ strip.finalizeInit()      → 两遍：先算 iType 资源，再建 Bus
   │    ├─ strip.makeAutoSegments()
   │    ├─ strip.setShowCallback(handleOverlayDraw)
   │    ├─ handleOnOff(true)         初始化继电器并强制熄灭
   │    ├─ 应用 turnOnAtBoot / briS / bootPreset / DEFAULT_COLOR(橙色)
   │    └─ strip.setTransition(transitionDelayDefault)
   ├─ userSetup() + UsermodManager::setup()      ★ 插件初始化
   ├─ needsCfgSave 时 serializeConfigToFS()      （usermod 补了新参数）
   ├─ bootPreset > 0 时 handlePresets() / handlePlaylist()
   ├─ Wi-Fi / 以太网准备
   │    ├─ WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)  (ESP32)
   │    ├─ getWLEDhostname() → WiFi.setHostname()   （必须在首次 DHCP 前）
   │    ├─ WiFi.onEvent(WiFiEvent)
   │    ├─ WiFi.mode(WIFI_STA)
   │    ├─ WiFi.setBandMode(AUTO)   (IDF ≥ 5.4.2)
   │    └─ findWiFi(true)           **异步**扫描，不阻塞
   ├─ serialCanRX/TX 判定（GPIO1/3 是否已被占用）
   ├─ 填默认 mDNS 名 / MQTT topic / clientId（基于 MAC 后 6 位）
   ├─ 配置 ArduinoOTA 回调（若 WLED_ENABLE_AOTA）
   ├─ initDMXOutput() / dmxInput.init()
   ├─ initServer()          ★ 注册全部 HTTP 路由（此时不监听）
   ├─ initIR()
   ├─ installIPv6RABlocker()  (ESP32 + LWIP_IPV6 + IDF<5)
   ├─ enableWatchdog()
   └─ markOTAvalid()        确认当前固件，取消回滚
```

**要点**

- `setup()` **不连接网络**，只发起异步扫描。真正的连接由主循环中的 `handleConnection()` 驱动。
- `deserializeConfigFromFS()` 同时完成"反序列化"和"应用"两件事（[`cfg.cpp:821-822`](../wled00/cfg.cpp#L821-L822) 有注释说明），所以 `initEthernet()` 藏在 `deserializeConfig()` 里（[`cfg.cpp:139`](../wled00/cfg.cpp#L139)）。
- HTTP 服务器路由在 `setup()` 注册，但套接字要等到 `initInterfaces()`（STA 连上）或 `initAP()` 才 `begin()`。

---

## 5. 主循环 `loop()`

入口 [`wled_main.cpp:22`](../wled00/wled_main.cpp#L22) → [`WLED::loop()` wled.cpp:59](../wled00/wled.cpp#L59)。每次迭代大致按以下顺序执行：

| 行号 | 调用 | 职责 |
|---|---|---|
| `82` | `handleTime()` | NTP / Toki 时间推进、定时器 |
| `84` | `handleIR()` | 红外解码（ESP32 上需调用两次，`:114` 再调一次） |
| `86` | **`handleConnection()`** | Wi-Fi 连接状态机（见 §8.2） |
| `88` | `handleSerial()` | Adalight / 串口 JSON / Improv |
| `90` | `handleImprovWifiScan()` | 非阻塞 Improv 扫描 |
| `91` | **`handleNotifications()`** | UDP 收包、**实时模式超时检查**、`e131NewData` 触发 show、UDP API |
| `92` | `handleTransitions()` | `updateInterfaces()`（WS/MQTT/Alexa）+ 亮度渐变 |
| `94` | `handleDMXOutput()` | DMX 串口输出 |
| `97` | `dmxInput.update()` | DMX 串口输入 |
| `103-104` | `userLoop()` + `UsermodManager::loop()` | 插件钩子 |
| `112` | `handleIO()` | 按钮、模拟输入、继电器 |
| `117` | `handleRemote()` | ESP-NOW / WiZ 遥控 |
| `120` | `handleAlexa()` | `espalexa.loop()` |
| `123-126` | `closeFile()` | 延迟的文件关闭（避开 LED 刷新窗口） |
| `131` | **实时模式闸门** | `if (!realtimeMode \|\| realtimeOverride \|\| useMainSegmentOnly)` |
| `133` | `dnsServer.processNextRequest()` | 强制门户 DNS（仅 `apActive`） |
| `135` | `ArduinoOTA.handle()` | espota（需已连接 + 启用 + 未锁 + PIN 正确） |
| `137` | `handleNightlight()` | 夜间灯渐变 |
| `141` | `handleHue()` | Hue 桥轮询 |
| `146` | `handlePlaylist()` | 播放列表推进（`presetNeedsSaving()` 时跳过） |
| `149` | `handlePresets()` | 预设加载/保存的实际执行 |
| `152-153` | **`strip.service()`** | ★ 动画与渲染入口 |
| `167` | `MDNS.update()` | 仅 ESP8266 |
| `171-176` | millis 回绕处理 | 强制 NTP 重同步 + `strip.restartRuntime()` |
| `177-187` | `initMqtt()` / `refreshNodeList()` / `sendSysInfoUDP()` | 每 30 s 一次 |
| `190-192` | PIN 超时 | 15 分钟无操作后 `correctPIN = false` |
| `194-235` | **堆内存看门狗** | 每 5 s 检查，连续低内存时分级降级（见下） |
| `239-248` | `doInitBusses` | LED 设置保存后重建总线 |
| `254` | `serializeConfigToFS()` | `configNeedsWrite` 时落盘 |
| `257` | `handleWs()` | WebSocket 清理 + 实时 LED 流（40 ms） |
| `277` | `doReboot` | 延迟重启（等配置写完） |

**最大的行为分叉：`realtimeMode` 闸门（`:131`）**

一旦进入实时模式（WARLS / Adalight / E1.31 …），`dnsServer`、`ArduinoOTA`、夜间灯、Hue、播放列表、预设、以及 `strip.service()` **全部被跳过**，除非设置了 `realtimeOverride` 或启用 `useMainSegmentOnly`。这是理解 WLED"实时优先"行为的核心。

**堆内存降级阶梯**（`wled.cpp:207-233`，以连续低内存秒数计）：

| 累计 | 动作 |
|---|---|
| 15 s | `strip.purgeSegments()` + 全部段切到 `FX_MODE_STATIC` 释放效果内存 |
| 30 s | `strip.resetSegments()` 只保留一个段 |
| 45 s | 析构并重建整个 `WS2812FX` 对象，必要时 `forceReconnect` |

---

## 6. 核心数据模型

### 6.1 全局状态（[`wled.h`](../wled00/wled.h)）

用 `WLED_GLOBAL` / `_INIT()` 宏对声明，仅在 `wled.cpp` 中 `WLED_DEFINE_GLOBAL_VARS` 展开为定义：

```c
// wled.h:262-280 的机制
WLED_GLOBAL byte bri _INIT(0);            // 全局亮度
WLED_GLOBAL byte colPri[] _INIT_N(({0,0,0,0}));  // 主色 RGBW
WLED_GLOBAL uint8_t realtimeMode _INIT(REALTIME_MODE_INACTIVE);
```

**持久配置类**（`cfg.cpp` 读写）：`multiWiFi`、`apSSID`/`apBehavior`、`busConfigs`、`buttons`、`irPin`、`rlyPin`、`transitionDelayDefault`、`gammaCorrect*`、`nightlight*`、`syncGroups`/`receiveGroups`、`mqtt*`、`ntp*` … 完整清单见 §9.1。

**运行时状态类**（`json.cpp` 读写）：`bri`、`Segment` 各字段、`transitionDelay`、`blendingStyle`、`nightlightActive`、`realtimeMode`、`currentPreset`、`currentPlaylist` …

**实时模式共享状态**：

| 变量 | 含义 | 位置 |
|---|---|---|
| `realtimeMode` | `REALTIME_MODE_{INACTIVE,GENERIC,UDP,HYPERION,E131,ADALIGHT,ARTNET,TPM2NET,DDP,DMX}` | [`wled.h:722`](../wled00/wled.h#L722) |
| `realtimeOverride` | 覆盖实时模式（让效果继续跑） | [`wled.h:723`](../wled00/wled.h#L723) |
| `realtimeIP` | 最近一次发送方 IP | [`wled.h:724`](../wled00/wled.h#L724) |
| `realtimeTimeout` | 退出实时模式的 `millis()` 截止点 | [`wled.h:725`](../wled00/wled.h#L725) |
| `realtimeTimeoutMs` | 默认锁定时长，2500 ms | [`wled.h:432`](../wled00/wled.h#L432) |
| `useMainSegmentOnly` | liveview 单段模式 | [`wled.h:730`](../wled00/wled.h#L730) |

### 6.2 `Segment`（[`FX.h:421`](../wled00/FX.h#L421)）

一个 Segment 是"物理 LED 带上的一段连续区间 + 独立的渲染状态"。

**公开字段**：`colors[3]`（每段 3 个颜色）、`start`/`stop`、`startY`/`stopY`、`offset`、`options`（位域）、`grouping`/`spacing`、`opacity`/`cct`、`mode`/`palette`/`speed`/`intensity`、`custom1-3`、`blendMode`、`name`。

`options` 位域（[`FX.h:430-446`](../wled00/FX.h#L430-L446)）：

| bit | 字段 | 宏 |
|---|---|---|
| 0 | `selected` | `SELECTED 0x0001` |
| 1 | `reverse` | `REVERSE 0x0002` |
| 2 | `on` | `SEGMENT_ON 0x0004` |
| 3 | `mirror` | `MIRROR 0x0008` |
| 4 | `freeze` | `FROZEN 0x0010` |
| 5 | `reset` | `RESET_REQ 0x0020` |
| 6-7 | `reverse_y` / `mirror_y` | `REVERSE_Y_2D` / `MIRROR_Y_2D` |
| 8 | `transpose` | `TRANSPOSED 0x0100` |
| 9-11 | `map1D2D`（3 bit） | `mapping1D2D_t` |
| 12-13 | `soundSim` | 声音模拟 |
| 14-15 | `set`（UI 分组） | |

**私有状态**：

- `uint32_t *pixels` —— **该段私有的渲染缓冲**，构造时按 `length() * 4` 分配（优先 PSRAM），失败则 `stop = 0` 并置 `errorFlag = ERR_NORAM_PX`（[`FX.h:598-604`](../wled00/FX.h#L598-L604)）。
- 一批 **static "暂存"变量**供绘制期间共享：`_currentColors[3]`、`_currentPalette`、`_vWidth`/`_vHeight`/`_vLength`、`_clipStart/_clipStop/...`、`_modeBlend`（[`FX.h:488-500`](../wled00/FX.h#L488-L500)）。
- `Transition *_t` —— 过渡状态，内含**旧段的一份堆拷贝** `_oldSegment`（[`FX.h:503-528`](../wled00/FX.h#L503-L528)）。

**`_segments` 是 `std::vector<Segment>`**，因此 `MAX_NUM_SEGMENTS` 是上限而非数组尺寸。

### 6.3 `Bus`（[`bus_manager.h:113`](../wled00/bus_manager.h#L113)）

```
Bus                      纯虚基类，唯一必实现的是 show() 与 setPixelColor()
├── BusDigital           RMT / I2S / SPI / BitBang 驱动的地址able LED
├── BusPwm               模拟 PWM（LEDC / 8266 sigma-delta）
├── BusOnOff             继电器 / 二值输出
├── BusNetwork           作为 E1.31 / Art-Net / DDP **输出源**广播
├── BusPlaceholder       配置占位（超资源时保留配置但不驱动）
└── BusHub75Matrix       HUB75 LED 面板（I2S-DMA）
```

关键点：**`Bus` 自己不持有像素缓冲**。老的 `Bus::_buf` 在 PolyBus 重构后移入了 `BusDigital::_busPtr` 指向的 `NeoPixelBus<...>` 实例内部。`PolyBus`（[`bus_wrapper.h:352`](../wled00/bus_wrapper.h#L352)）是一组 `static` 方法，把 `void*` 类型擦除后 `static_cast` 回正确的 `NeoPixelBus<Feature, Method>` 特化。

**两阶段构建**（[`FX_fcn.cpp:1228-1261`](../wled00/FX_fcn.cpp#L1228-L1261)）：

1. `BusManager::getI()` → `PolyBus::getI()` 为每个 `BusConfig` 计算内部类型 `iType`，**这一步负责预留 RMT / I2S / SPI 通道**，必须对全部总线先跑完（并行 I2S 的分配依赖此）。
2. `BusManager::add()` 才真正构造 `Bus` 子类对象。分配顺序：Placeholder → Network → Hub75 → Digital → OnOff → Pwm。

**芯片类型选择**（[`bus_wrapper.h:1306-1409`](../wled00/bus_wrapper.h#L1306-L1409)）：

- ESP32：前 `WLED_MAX_RMT_CHANNELS` 条走 RMT，其余走 I2S；第一条 I2S 会**锁定** `_parallelBusItype`，后续 I2S 强制同型并置 `_useParallelI2S`。S3 上始终为 true（走 LCD 并行驱动）。
- 2 脚 SPI 芯片（APA102/LPD8806/LPD6803/WS2801/P9813）：第一条被提升为硬件 SPI。
- ESP8266：按 `pins[0]-1` 在 UART0/UART1/DMA/BitBang 之间选择。
- 资源耗尽 → `I_NONE`，总线创建干净失败（转 Placeholder）。

### 6.4 颜色模型

**存储格式统一为 `uint32_t`，布局 `0xWWRRGGBB`**。宏定义在 [`colors.h:23-27`](../wled00/colors.h#L23-L27)（`bus_wrapper.h:349` 有一份完全相同的副本，让 wrapper 自给自足）：

```c
#define RGBW32(r,g,b,w) (uint32_t((byte(w) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b)))
#define R(c) (byte((c) >> 16))
#define G(c) (byte((c) >> 8))
#define B(c) (byte(c))
#define W(c) (byte((c) >> 24))
```

**颜色顺序** `colorOrder` 是**双半字节编码**：

- **低半字节**：RGB 通道顺序（`GRB`=0 默认，`RGB`=1，`BRG`=2，`RBG`=3，`BGR`=4，`GBR`=5，见 [`const.h:394-400`](../wled00/const.h#L394-L400)）。
- **高半字节**：白通道交换（1 = W↔B，2 = W↔G，3 = W↔R，4 = WW↔CW）。

写入在 `PolyBus::setPixelColor()`（[`bus_wrapper.h:791-815`](../wled00/bus_wrapper.h#L791-L815)）完成，读取在 `getPixelColor()` 反向还原。

按像素区间的覆盖由 `ColorOrderMap` 提供，最多 `WLED_MAX_COLOR_ORDER_MAPPINGS`（5 或 10）条 `{start, len, colorOrder}`。

**Gamma** 走 WLED 自管的表 `NeoGammaWLEDMethod`（[`colors.h:34-54`](../wled00/colors.h#L34-L54)），因此可以在运行时重配。

---

## 7. 渲染管线：一帧的生命周期

这是 WLED 最核心的部分，分 **三个阶段、三块缓冲**。

### 7.1 阶段一：逐段绘制

入口 [`FX_fcn.cpp:1306`](../wled00/FX_fcn.cpp#L1306) `WS2812FX::service()`：

```c
nowUp    = millis();
elapsed  = nowUp - _lastServiceShow;
timeToShow = (elapsed >= _frametime);              // 所有段共用同一个帧率
if (_triggered || _targetFps == FPS_UNLIMITED) timeToShow = true;
now = nowUp + timebase;                            // 效果时间基准
if (!timeToShow) return;                           // 帧未到
if (_suspend || elapsed <= MIN_FRAME_DELAY) return; // 保 Wi-Fi 存活

for (每个 segment) {
    seg.handleTransition();     // 更新过渡进度
    seg.resetIfRequired();      // 需要时清空运行时数据
    if (!seg.isActive()) continue;
    if (!seg.freeze) {
        seg.beginDraw(seg.progress());   // 设置调色板、颜色混合、绘制尺寸
        _currentSegment = &seg;
        _mode[seg.mode]();               // ★ 效果函数写 Segment::pixels
        seg.call++;
        // 过渡中还要再跑一遍"旧段"的效果
        if (segO && segO->isActive() && 模式或过渡样式需要) {
            Segment::modeBlend(true);
            segO->beginDraw(prog);
            _currentSegment = segO;
            _mode[segO->mode]();         // ★ 旧效果
            Segment::modeBlend(false);
        }
    }
}
if (doShow && !_suspend) {
    Segment::handleRandomPalette();
    _lastServiceShow = nowUp;
    show();                              // ★ 阶段二 + 阶段三
}
_triggered = false;
```

**效果函数签名是 `void (*)()`**（[`FX.h:813`](../wled00/FX.h#L813)），不是早期版本的 `uint16_t (*)(void)`。效果自己写像素、不返回帧延时；需要控制节奏时用 `FRAMETIME` 宏。

**效果表机制**：

- `_mode` 是 `std::vector<mode_ptr>`，`_modeData` 是 `std::vector<const char*>`（PROGMEM 字符串）。
- 构造时预留 `MODE_COUNT`(220) 个槽位；分配失败会退化成只保留 Solid（[`FX.h:858-861`](../wled00/FX.h#L858-L861)）。
- `setupEffectData()` 先把所有槽位填成 `mode_static` + 占位串 `"RSVD"`，再由 ~220 次 `addEffect()` 替换（[`FX.cpp:10972-10981`](../wled00/FX.cpp#L10972-L10981)）。**未被认领的槽位刻意留空**，以保证效果 ID 跨版本稳定。
- `addEffect(255, ...)` 表示"取第一个空闲槽"，返回实际 ID；**返回 255 表示注册失败**。Usermod 自定义效果走这条路。

**每段的效果元数据串**格式为 `名称@滑块1,滑块2,...;复选1,...;下拉;默认值`，最后一段分号后是默认参数，由 `extractModeDefaults()` 解析（[`util.cpp:481-499`](../wled00/util.cpp#L481-L499)），例如：

```
"Slow Transition@Time (min),,,,,,Sweep;!;!;1;pal=2,sx=0,ix=0"
```

### 7.2 阶段二：合成

[`FX_fcn.cpp:1726`](../wled00/FX_fcn.cpp#L1726) `WS2812FX::show()`：

```c
if (!_pixels) { errorFlag = ERR_NORAM; return; }
if ((hasCCTBus() || correctWB) && !cctFromRgb) {
    _pixelCCT = allocate_buffer(totalLen);
    memset(_pixelCCT, 127, totalLen);        // 中性 50:50
}
if (非实时 || useMainSegmentOnly || 有 override) {
    memset(_pixels, 0, 4 * totalLen);        // 清空全局帧缓冲
    for (每个 active segment)
        blendSegment(seg);                   // ★ 按段序合成，后画的在上层
}
if (_callback) _callback();                  // 叠加层（模拟时钟 / usermod）
```

`blendSegment()`（[`FX_fcn.cpp:1406-1724`](../wled00/FX_fcn.cpp#L1406-L1724)）实现了 17 种 Photoshop 风格混合模式，函数表在 `:1410-1416`：

```
_dummy, _dummy, _dummy, _subtract, _difference, _average, _dummy, _divide,
_lighten, _darken, _screen, _overlay, _hardlight, _softlight, _dodge, _burn, _dummy
```

属性 `opacity = topSegment.currentBri()`（过渡混合后的值），开启 gamma 时还要过一遍 `gamma8inv()`。

**过渡样式实现为裁剪矩形**：`switch (blendingStyle)` 设置 `_clipStart/_clipStop/_clipStartY/_clipStopY`，`Segment::isPixelClipped()` / `isPixelXYClipped()` 据此判断是否绘制。`FAIRY_DUST` 和 `CIRCULAR_IN/OUT` 例外 —— 它们逐像素判定，所以裁剪矩形设为整段。

**⚠️ 三个容易混淆的"blend"**：

| 名称 | 作用域 | 含义 |
|---|---|---|
| `blendingStyle` | **全局**（[`wled.h:592`](../wled00/wled.h#L592)） | **过渡样式**：淡入淡出 / 滑动 / 推挤 / 圆形… |
| `SEGMENT.blendMode` | 每段（[`FX.h:461`](../wled00/FX.h#L461)，0-16） | **合成模式**：该段叠加在下层之上时怎么算 |
| `paletteBlend` | 全局 | **调色板插值环绕方式**（NOBLEND / LINEARBLEND / LINEARBLEND_NOWRAP） |
| `Segment::_modeBlend` | static bool | 信号量："当前正在跑旧段的效果" |

注意 `FX_MODE_BLENDS`（id 115）只是一个名叫 "Blends" 的**效果**，与上述机制无关。

### 7.3 阶段三：输出

```c
useGammaCorrection = gammaCorrectCol && !(realtimeMode && arlsDisableGammaCorrection && !realtimeOverride);
for (i in [0, totalLen)) {
    if (_pixelCCT && CCT 变化) BusManager::setSegmentCCT(_pixelCCT[i], correctWB);
    c = _pixels[i];
    if (c > 0 && useGammaCorrection) c = gamma32(c);
    BusManager::setPixelColor(getMappedPixelIndex(i), c);   // ledmap 重映射
}
Bus::setCCT(oldCCT);
BusManager::show();                                          // ★ 真正推线
// FPS 统计（指数移动平均）
```

`BusManager::show()`（[`bus_manager.cpp:1438`](../wled00/bus_manager.cpp#L1438)）先 `applyABL()` 再逐 bus `->show()`。最终 `BusDigital::show()` → `PolyBus::show()` → `NeoPixelBus::Show(consistent)`。

### 7.4 各环节修正的施加位置

| 环节 | 位置 | 机制 |
|---|---|---|
| 段合成混合 | `FX_fcn.cpp:1406-1724` | 17 种混合模式，opacity 来自 `currentBri()` |
| 逐像素白平衡 (CCT) | `FX_fcn.cpp:1767-1769` → `BusManager::setSegmentCCT()` | `correctWB` 时 0-255 转 Kelvin（`1900 + (cct<<5)`），在 `BusDigital::setPixelColor` 中生效 |
| 段级 CCT → WW/CW 拆分 | `Bus::calculateCCT()` [`bus_manager.cpp:70`](../wled00/bus_manager.cpp#L70) | `_cctBlend` (-127…+127) 控制重叠 |
| 自动白光 (RGBW) | `Bus::autoWhiteCalc()` [`bus_manager.cpp:102`](../wled00/bus_manager.cpp#L102) | 全局 `_gAWM` 覆盖每总线设置 |
| **全局亮度** | `WS2812FX::setBrightness()` → 各 `Bus::_bri`；每像素在 `BusDigital::setPixelColor` 中 `color_fade(c, _bri, true)` | 另有 `briMultiplier`（`scaledBri()`）和夜间灯/过渡渐变 |
| Gamma | `FX_fcn.cpp:1772-1773` | 仅当 `gammaCorrectCol` |
| **ABL 限流** | `BusManager::applyABL()` [`bus_manager.cpp:1510`](../wled00/bus_manager.cpp#L1510) | 由 `_colorSum` 估算毫安数，重算 `_NPBbri` **并重绘 NPB 缓冲** |
| 颜色顺序 | `PolyBus::setPixelColor()` | 低半字节 RGB 置换 + 高半字节 W 交换 |
| 线上格式 | NPB Feature | 如 `NeoGrbwFeature`、`Rgb48Color`、`Rgbww80Color` |

**顺序要点**：ABL 在 `bus->show()` **之前**跑；`_colorSum` 在应用亮度**之后**累加（[`bus_manager.cpp:284-292`](../wled00/bus_manager.cpp#L284-L292)），即限流估算基于已缩放的颜色。

模拟总线略有不同：`BusPwm::setPixelColor()` 应用白平衡与自动白光，但**不**应用亮度（[`bus_manager.cpp:483`](../wled00/bus_manager.cpp#L483) 注释："brightness is applied in show()"）；`BusPwm::show()` 才施加 CIE 亮度曲线、可选的 4-bit LEDC 抖动，并对各通道做相位错开以削峰。

### 7.5 帧率控制

```c
#define WLED_FPS         42                  // FX.h:61
#define FRAMETIME_FIXED  (1000/WLED_FPS)     // FX.h:62  = 23 ms
#define FRAMETIME        strip.getFrameTime()// FX.h:63  效果用这个
#define FPS_UNLIMITED    0                   // FX.h:73
```

`setTargetFps(fps)`（[`FX_fcn.cpp:1832`](../wled00/FX_fcn.cpp#L1832)）：`fps <= 250` 时 `_targetFps = fps; _frametime = 1000/fps`；`fps == 0` 表示不限速，此时 `_frametime = MIN_FRAME_DELAY`（ESP32 双核 2 ms / 单核 3 ms / ESP8266 8 ms）。

闸门在 `service()` 开头；`_lastServiceShow` 只在真正出帧时更新，因此慢效果不会累积漂移。`_triggered`（`strip.trigger()`）可强制立即出帧。

实测 FPS 由 `show()` 中的指数移动平均给出（`FPS_CALC_AVG = 7`），经 `getFps()` 读取；超过 2 秒没出帧则报 0。

**`_frametime` 只控制出帧节奏**；效果自身的快慢来自 `SEGMENT.speed`（通常经 `SPEED_FORMULA_L`）和 `SEGENV.step += FRAMETIME`。

### 7.6 2D 与 ledmap

`WS2812FX::setUpMatrix()`（[`FX_2Dfcn.cpp:21-141`](../wled00/FX_2Dfcn.cpp#L21-L141)）根据 `panel[]` 向量构建 `customMappingTable`：

1. 计算所有 panel 的包围盒 → `Segment::maxWidth/maxHeight`。
2. 安全检查：超过 `MAX_LEDS`、任一维 >255 或 ≤1 则退回 1D。
3. 分配 `uint16_t[getLengthTotal()]`，先填 `0xFFFF`（未映射），矩阵之后的"拖尾灯带"填恒等映射。
4. 可选读 `/2d-gaps.json`（`-1` 缺灯 / `0` 不激活 / `1` 激活）。
5. 逐 panel 遍历，按 `bottomStart` / `rightStart` / `vertical` / `serpentine` 算出每个逻辑 `(x,y)` 对应的物理索引。

应用点只有一处：`show()` 中的 `getMappedPixelIndex(i)`（[`FX.h:950`](../wled00/FX.h#L950)），且**实时模式下默认不生效**（除非开启 `realtimeRespectLedMaps`）。

1D→2D 映射由 `Segment::map1D2D` 控制（[`FX.h:409-415`](../wled00/FX.h#L409-L415)）：`M12_Pixels`(整条带)、`M12_pBar`(竖条)、`M12_pArc`(螺旋)、`M12_pCorner`(拐角)、`M12_sPinwheel`(风车)。

### 7.7 粒子系统

[`FXparticleSystem.{h,cpp}`](../wled00/FXparticleSystem.cpp) 是一个可复用的物理引擎（重力、碰撞、反弹、环绕、发射器、摩擦），供火焰/烟花/蜜蜂/八爪鱼/GEQ/弹球等效果共用。

**内存模型是关键**：它不是全局单例，而是**活在每个段的效果数据缓冲里**：

- `allocateParticleSystemMemory2D()` 调 `SEGMENT.allocateData()` 申请空间，受 `MAX_SEGMENT_DATA` / `FAIR_DATA_PER_SEG` 约束。
- `initParticleSystem2D()` 会**在分配失败时把粒子数减半重试**，下限为 5。
- 分配成功后 placement-new 到缓冲上：`PartSys = new (SEGENV.data) ParticleSystem2D(...)`。
- `updatePSpointers()` 中 `framebuffer = SEGMENT.getPixels()` —— 粒子直接渲染进**该段自己的像素缓冲**。

从段循环视角看，它就是另一个往 `Segment::pixels` 写像素的效果函数，后续合成/过渡/CCT/gamma 全部走通用路径。

---

## 8. 网络子系统

### 8.1 抽象层

[`wled00/src/dependencies/network/Network.cpp`](../wled00/src/dependencies/network/Network.cpp) 中的 `WLEDNetwork`（单例 `WLEDNetworkClass`）把 Wi-Fi 与以太网统一：

```c
isConnected() = (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED) || isEthernet()
```

对外暴露为宏 `WLED_CONNECTED`（[`wled.h:909`](../wled00/wled.h#L909)）。`localIP()` / `subnetMask()` / `gatewayIP()` / `localMAC()` 等在 ESP32 + `WLED_USE_ETHERNET` 时优先走 `ETH`，否则回退 `WiFi`。

**以太网每启动周期只能配置一次**（[`network.cpp:198`](../wled00/network.cpp#L198) 的 static 守卫）。板型来自编译期表 `ethernetBoards[]`（15 种：WT32-ETH01、ESP32-POE、WESP32、QuinLed、Gledopto …），RMII 引脚通过 `PinManager::allocateMultiplePins(..., PinOwner::Ethernet)` 统一占用。

### 8.2 连接状态机 `handleConnection()`

[`wled.cpp:962-1101`](../wled00/wled.cpp#L962-L1101)，每次 `loop()` 都跑。状态变量：`apActive`、`apClients`、`forceReconnect`、`lastReconnectAttempt`、`interfacesInited`、`wasConnected`、`selectedWiFi`、`multiWiFi`。

```
未连接分支：
  ├─ 多个 SSID 且扫描完成 → findWiFi(true) 重新扫描，下一轮再试
  ├─ > 18 s（有 AP 客户端时 300 s）→ 轮换 selectedWiFi 后 initConnection()
  ├─ > 12 s 仍未连上且未开 AP → initAP()（受 apBehavior 控制）
  ├─ 临时 AP 且超时无客户端 → 关 AP
  └─ Improv 24 s 后上报失败

已连接分支（首次进入）：
  ├─ esp_wifi_set_storage(WIFI_STORAGE_RAM)   停止写 NVM，避免 Flash 磨损
  ├─ initInterfaces()                          ★ 一次性启动所有服务
  ├─ userConnected() / UsermodManager::connected()
  ├─ lastMqttReconnectAttempt = 0             强制立即更新
  └─ 非 AP_BEHAVIOR_ALWAYS 时关掉 AP
```

`initInterfaces()`（[`wled.cpp:908`](../wled00/wled.cpp#L908)）顺序固定：Hue IP 默认 → `alexaInit()` → `ArduinoOTA.begin()` → mDNS（`http/tcp:80`、`wled/tcp:80`、TXT `mac`）→ `server.begin()` → 绑定 UDP 套接字（`udpPort` 21324、`udpRgbPort` 19446、`udpPort2` 65506，与 `ntpLocalPort` 去重）→ `ntpUdp.begin()` → `e131.begin()` + `ddp.begin(4048)` → `reconnectHue()` → `interfacesInited = true`。

`WiFiEvent()`（[`network.cpp:440-531`](../wled00/network.cpp#L440-L531)）处理事件驱动的状态变更。ESP8266 与 IDF3 的事件名被 `#define` 到现代的 `ARDUINO_EVENT_WIFI_*` 上。特别地：`ETH_DISCONNECTED` 会把 `forceReconnect` 置位并重启 Wi-Fi 扫描 —— 因为以太网只能配置一次，Wi-Fi 是唯一的恢复路径。

### 8.3 HTTP 服务器

`AsyncWebServer server`（端口 80）+ `AsyncWebSocket ws("/ws")`，路由在 `initServer()`（[`wled_server.cpp:349-693`](../wled00/wled_server.cpp#L349-L693)）一次性注册。CORS 全局放开 `Access-Control-Allow-Origin: *`。

**主要端点**

| URL | 处理器 |
|---|---|
| `/` | 强制门户检查 → 首页 或 欢迎页 |
| `/settings`, `/welcome` | `serveSettings()` |
| `POST /settings` | `handleSettingsSet()` |
| `/settings/s.js?p=N` | `getSettingsJS()` 生成表单预填 JS |
| `GET /json` / `POST /json` | `serveJson()` / `AsyncCallbackJsonWebHandler` |
| `/json/{state,info,si,nodes,eff,pal,palx,fxdata,net,cfg,pins,live}` | 见 §9.4 |
| `/update`（GET/POST）、`/updatebootloader` | OTA |
| `/edit`、`/upload` | 文件系统编辑/上传 |
| `/liveview`, `/liveview2D`, `/pixart.htm`, `/pxmagic.htm`, `/pixelforge.htm`, `/cpal.htm` | 附加页面 |
| `/reset`, `/version`, `/uptime`, `/freeheap` | 工具端点 |
| `/ws` | WebSocket |
| **`onNotFound`** | → `handleSet()`（传统 API）→ `espalexa.handleAlexaApiCall()` → 404 |

**静态资源服务**统一走 `handleStaticContent()`（[`wled_server.cpp:140-147`](../wled00/wled_server.cpp#L140-L147)）：

1. 若路径非空且文件系统里真有该文件，`handleFileRead()` 优先 —— 用户可覆盖内置页面；
2. `If-None-Match` 命中 → 304；
3. 否则 `beginResponse_P()` 直接送 PROGMEM 中已 gzip 压缩的页面，附 `Content-Encoding: gzip`。

ETag 由 `WEB_BUILD_TIME` + `cacheInvalidate` + 后缀生成。

**强制门户**：`captivePortal()`（[`wled_server.cpp:333`](../wled00/wled_server.cpp#L333)）仅在 `apActive` 时，对 Host 头既不是 IP、也不含 `wled.me` / `cmDNS` / 端口的请求返回 302 到 `http://4.3.2.1`。

### 8.4 实时协议的汇聚

**所有实时协议都汇聚到同一个入口** `realtimeLock(timeoutMs, mode)`（[`udp.cpp:408-437`](../wled00/udp.cpp#L408-L437)）：

```c
if (!realtimeMode && !realtimeOverride) {          // 首次进入实时
    if (useMainSegmentOnly) { 清空主段、freeze=true、bri==0 时冻结其他段 }
    else strip.fill(BLACK);
    if (briT == 0) 恢复上次亮度;
}
realtimeTimeout = (timeoutMs == 255001 || timeoutMs == 65000) ? UINT32_MAX : millis() + timeoutMs;
realtimeMode = md;
if (arlsForceMaxBri) strip.setBrightness(255, true);
```

**超时不在定时器里，而在主循环中检查**（[`udp.cpp:482-483`](../wled00/udp.cpp#L482-L483)）：

```c
if (realtimeMode && millis() > realtimeTimeout) exitRealtime();
```

**协议入口一览**

| 协议 | 传输入口 | 模式 |
|---|---|---|
| WLED notifier（同步） | `notifierUdp` 21324 / `notifier2Udp` 65506 | 非实时，`realtimeMode` 时忽略 |
| Hyperion / 原始 RGB | `rgbUdp` 19446 | `REALTIME_MODE_HYPERION` |
| TPM2.NET | `notifierUdp` (0x9c/0xda) | `REALTIME_MODE_TPM2NET`，多包重组 |
| WARLS/DRGB/DRGBW/DNRGB/DNRGBW | `notifierUdp` byte0 = 1..5 | `REALTIME_MODE_UDP`，byte1 为超时秒数 |
| E1.31 (sACN) | `e131.begin()` | `REALTIME_MODE_E131` |
| Art-Net | 同套接字（6454） | `REALTIME_MODE_ARTNET` |
| DDP | `ddp.begin(false, 4048)` | `REALTIME_MODE_DDP` |
| E1.31/Art-Net/DDP over WebSocket | `ws.cpp` 二进制帧，首字节选协议 | 同上 |
| Adalight / 串口 | `wled_serial.cpp` | `REALTIME_MODE_ADALIGHT` |
| DMX 输入 | `dmxInput.update()` | `REALTIME_MODE_DMX` |
| HTTP JSON `"live":true` | `deserializeState` | `REALTIME_MODE_GENERIC`（`realtimeLock(65000)`） |

**渲染节流**：E1.31 / Art-Net / DDP 的数据进入后只置 `e131NewData = true`，实际 show 由 `handleNotifications()` 统一控制（[`udp.cpp:475-480`](../wled00/udp.cpp#L475-L480)）：

```c
if (e131NewData && millis() - strip.getLastShow() > 15) {
    e131NewData = false;
    if (useMainSegmentOnly) strip.trigger(); else strip.show();
}
```

这个 **15 ms 节流**把"包到达"与"LED 重绘"解耦 —— 所有协议只往像素缓冲写，show 被限速。

**E1.31 优先级**：`E131Priority(3)` 对象（局部于 `e131.cpp:15`）在 `e131Priority != 0` 时允许高优先级源抢占、丢弃低优先级。乱序包通过 `e131LastSequenceNumber[]` + `e131SkipOutOfSequence` 拒绝。

### 8.5 多机同步

**发送** `notify(callMode, followUp)`（[`udp.cpp:20-204`](../wled00/udp.cpp#L20-L204)），广播到 `~subnetMask | gatewayIP`。包长 `WLEDPACKETSIZE = 41 + getMaxSegments()*36`。

| 字节 | 内容 |
|---|---|
| 0 | 0 = WLED notifier 协议 |
| 1 | callMode |
| 2 | `bri` |
| 3-5 / 10 | 主段主色 R,G,B / W |
| 8, 9 | 主段 mode / speed |
| **11** | **协议兼容版本 = 12** |
| 12-15 | 次色 RGBW |
| 16-19 | intensity / transitionDelay / palette |
| 24 | `followUp` |
| 25-28 | `millis() + strip.timebase`（大端） |
| 30-36 | unix 秒/毫秒、`syncGroups` |
| 37-40 | CCT 有效性、段数、每段记录长度（36） |
| 41+ | 每段 36 字节记录 |

版本字节的注释块（[`udp.cpp:54-59`](../wled00/udp.cpp#L54-L59)）完整记录了该协议从 v0 到 v12 的演进。

**接收** `parseNotifyPacket()`（[`udp.cpp:206-405`](../wled00/udp.cpp#L206-L405)）：

- 回声抑制：1 秒内自己发过通知则忽略；源 IP 等于自己则忽略。
- **同步组**：老版本（<9）视为组 1；否则 `if (!(receiveGroups & udpIn[36])) return;`。
- 版本 >10 且开启段同步时，逐段重建几何、选项、颜色、custom slider。
- **时间基准同步**（版本 >5）：`strip.timebase = 发送方millis + 网络延迟估计 - millis()`。
- **系统时间同步**（版本 >7）：按时间源优劣决定是 `toki.adjust()` 还是用 `toki.msDifference()` 精修 timebase。
- 最后 `stateUpdated(CALL_MODE_NOTIFICATION)` —— 该 callMode 刻意**不会**再次广播，避免风暴。

**节点发现**：`sendSysInfoUDP()` 发 44 字节补充包到 `255.255.255.255:udpPort2`，接收后写入全局 `Nodes` map（上限 `WLED_MAX_NODES`：24 或 150），`refreshNodeList()` 对条目做 10 个 tick 的老化。每 30 秒广播一次，与 MQTT 重连共用节拍。

### 8.6 OTA

**两条独立机制**

**(a) Web `/update`**（[`ota_update.cpp`](../wled00/ota_update.cpp)）：

- 首块三重校验：子网检查 → `correctPIN` → `!otaLock`（[`wled_server.cpp:542-558`](../wled00/wled_server.cpp#L542-L558)）。注意**设置了 settings PIN 会取消 `otaSameSubnet` 要求**。
- `beginOTA()`：关看门狗 → `UsermodManager::onUpdateBegin(true)` → `strip.suspend()` → `backupConfig()` → `strip.resetSegments()` → `Update.begin()`。
- 边写边校验：在 `METADATA_OFFSET`（ESP32 +256，ESP8266 +0x1000）读 512 字节元数据，`shouldAllowOTA()` 检查**发布名匹配**与**最低版本要求**。
- ESP8266 额外支持 **gzip 固件**：用 uzlib 解压前 `METADATA_OFFSET + 512` 字节找元数据，写入的仍是原始 gzip 流（eboot 启动时解压）。
- `/updatebootloader`（仅 ESP32）：缓冲最多 64 KB，校验 magic / 段数 / chip ID / SHA256 后写 `BOOTLOADER_OFFSET`。
- `markOTAvalid()` 在 `setup()` 末尾确认固件、取消回滚。

**(b) ArduinoOTA / espota**（需 `-D WLED_ENABLE_AOTA`）：在 `initInterfaces()` 中 `begin()`，主循环中 `ArduinoOTA.handle()`，受 `aOtaEnabled && !otaLock && correctPIN` 三重门控。

---

## 9. 配置、持久化与 API

### 9.1 两个 JSON 宇宙

WLED 有**两套完全独立**的 JSON：**配置(cfg)** 与**状态(state)**。

| | **Config** | **State** |
|---|---|---|
| 序列化 | `serializeConfig()` [`cfg.cpp:850`](../wled00/cfg.cpp#L850) | `serializeState()` [`json.cpp:647`](../wled00/json.cpp#L647) |
| 反序列化 | `deserializeConfig()` [`cfg.cpp:47`](../wled00/cfg.cpp#L47) | `deserializeState()` [`json.cpp:370`](../wled00/json.cpp#L370) |
| 落盘 | `/cfg.json` + `/wsec.json` | `/presets.json`、`/tmp.json`（"当前状态"本身不落盘） |
| 生命周期 | 跨重启持久 | 易失，只有存进预设才持久 |
| HTTP | `GET/POST /json/cfg`、`POST /settings` | `GET/POST /json/state`、WS、MQTT、UDP、IR |

**关键不对称**：

- `serializeConfig()` 是**全量**的：无条件写出每个字段，含 `rev:[1,0]` 和 `vid:VERSION`。密码**不写入** —— 只写长度（`pskl`）。
- `serializeState()` 是**有损且上下文相关**的，签名：

```c
void serializeState(JsonObject root, bool forPreset = false, bool includeBri = true,
                    bool segmentBounds = true, bool selectedSegmentsOnly = false);
```

| 参数 | 效果 |
|---|---|
| `includeBri=false` | 省略 `on`/`bri`/`transition`/`bs`（存预设时不想改亮度） |
| `forPreset=true` | 抑制运行时专属字段 `error`/`ps`/`pl`/`ledmap`/`nl.*`/`udpn.*`/usermod 数据 |
| `segmentBounds=false` | 省略 `start`/`stop`/`startY`/`stopY`/`len` |
| 预设模式 | 未激活的段写成 `{"stop":0}` 墓碑以**禁用**它们 |

- `deserializeState()` 相当宽容：每个字段可选，用 ArduinoJson 的 `|` 回退。它还带有**超出状态之外的副作用** —— 可以存/删预设、执行传统 HTTP API（`win` 键）、加载播放列表、删自定义调色板、开关 AP。

**`CJSON` 惯用法**（[`cfg.cpp:41`](../wled00/cfg.cpp#L41)）：

```c
#define CJSON(a,b) a = b | a
```

"仅当 JSON 值非空时才覆盖全局" —— 这正是部分 `POST /json/cfg` 与前后兼容性得以工作的原因。

### 9.2 文件系统布局

| 路径 | 内容 |
|---|---|
| `/cfg.json` | 主配置（无密码） |
| `/wsec.json` | Wi-Fi / AP / MQTT / OTA 密码、`settingsPIN`、Hue API key、OTA 锁标志 |
| `/presets.json` | 预设 1-250 |
| `/tmp.json` | 临时预设 255（非持久） |
| `/bkp.cfg.json` | 每次写入前的自动备份 |
| `/rst.cfg.json` | `resetConfig()` 重命名的坏配置 |
| `/ir.json`, `/remote.json` | 红外 / ESP-NOW 遥控命令映射 |
| `/paletteN.json`, `/ledmap*.json` | 自定义调色板 / LED 映射 |

安全防护：`handleFileRead()` 拒绝任何含 `"sec"` 的路径（[`file.cpp:438`](../wled00/file.cpp#L438)），`/edit` 目录列表跳过 `wsec*`。上游 `ESPAsyncWebServer` 被硬性 `#error` 拒绝，原因正是它的文件服务可能泄露 `wsec.json`（[`wled.h:240-243`](../wled00/wled.h#L240-L243)）。

### 9.3 `file.cpp` —— JSON 就地补丁引擎

WLED **不重写** JSON 文件，而是把它当作可追加、可就地打补丁的键值存储。结构契约（[`file.cpp:25-36`](../wled00/file.cpp#L25-L36)）：必须是 JSON 对象、`{` 开头、根键与其值之间**无空白**、头部有一个 `"0":{}` 哑元（便于删除首个真实对象而无需处理逗号）、除空闲空间外不得有超过 5 个连续空格。

核心函数：`bufferedFind()`（256 字节块搜索）、`bufferedFindObjectEnd()`（花括号深度扫描）、`bufferedFindSpace()`（空闲空间分配器）、`writeObjectToFile()`（四种情形：原地替换+填充 / 用尾随空间替换 / 删除+填充 / 删除+追加）。

### 9.4 JSON API 面

`serveJson()`（[`json.cpp:1350`](../wled00/json.cpp#L1350)）按 URL **子串**匹配分发：

| URL 子串 | 目标 |
|---|---|
| `state` | `serializeState()` |
| `info` | `serializeInfo()` |
| `si` | state + info |
| `nodes` | UDP 发现的节点列表 |
| `eff` | 效果名列表 |
| `palx` | 调色板颜色数据（分页） |
| `fxda` | 效果元数据（分块流式） |
| `net` | 扫描到的 SSID |
| `cfg` | `serializeConfig()` |
| `pins` | 引脚占用 |
| `live` | 实时 LED 数据（需 `WLED_ENABLE_JSONLIVE`） |
| `pal` | 调色板名数组 |
| 无匹配且 `len>6` | 501 `ERR_NOT_IMPL` |
| 无匹配且 `len==5` | state + info + effects + palettes |

**响应管道**：`LockedJsonResponse`（[`json.cpp:1327-1348`](../wled00/json.cpp#L1327-L1348)）是 `AsyncJsonResponse` 的子类，其析构/`_fillBuffer` 在恰当时机释放 JSON 缓冲锁。

**单一全局 JSON 缓冲**是全项目的重要约束：所有 JSON 工作（HTTP POST、WS 收发、MQTT、UDP API、设置页）都串行化在 `requestJSONBufferLock(lockId)` 上。抢不到锁的请求要么 `request->deferResponse()`，要么下一轮 loop 重试。

**`POST /json` 流程**（[`wled_server.cpp:422-477`](../wled00/wled_server.cpp#L422-L477)）：

```c
deserializeJson(*pDoc, request->_tempObject);
if (root.containsKey("pin")) checkSettingsPIN(root["pin"]);
isConfig = url.indexOf("cfg") > -1;
if (!isConfig) verboseResponse = deserializeState(root);
else { if (!correctPIN && strlen(settingsPIN)>0) → 401; verboseResponse = deserializeConfig(root); }
```

**`callMode`** 是调用方的意图（不是 JSON 键），传给 `stateUpdated(callMode)` 决定是否广播通知。取值见 [`const.h:273-285`](../wled00/const.h#L273-L285)：`INIT=0`（不更新界面）、`DIRECT_CHANGE=1`、`BUTTON=2`、`NOTIFICATION=3`、`NIGHTLIGHT=4`、`NO_NOTIFY=5`、`HUE=7`、`ALEXA=10`、`WS_SEND=11`、`BUTTON_PRESET=12`。

**`deserializeState` 顶层键**（部分）：`v`（详细响应）、`bri`/`on`、`transition`/`tt`/`tb`/`bs`、`nl{on,dur,mode,tbri}`、`udpn{send,sgrp,rgrp,nn}`、`time`、`rb`、`mainseg`/`lor`/`live`、`seg`（对象或数组）、`rSeg`、`ledmap`、`psave`/`pdel`、`win`、`pd`/`ps`、`playlist`、`rmcpal`、`np`、`wifi{ap}`、`debug`。

**预设载荷标志**（不属于 `deserializeState`）：`n`（名字）、`ql`（快捷加载）、`ib`（含亮度）、`sb`（段边界）、`sc`（仅选中段）、`bootps`（同时设为启动预设）、`o`（"这是命令/播放列表而非状态快照"）、`playlist`。

### 9.5 预设与播放列表

**槽位语义**（核心约定）：

| 槽位 | 含义 |
|---|---|
| **0** | "当前状态"，永远不是存储的预设。任何改动都会把 `currentPreset` 归零（[`led.cpp:93`](../wled00/led.cpp#L93)）。写入会被拒绝 |
| 1-250 | 持久预设，一条 `/presets.json` 记录 |
| 251-254 | 保留/无效 |
| **255** | 临时预设，ESP32 上序列化到 PSRAM 缓冲而非文件系统 |

**写路径是异步的**：`savePreset()` 只置 `presetToSave` 等标志；真正的写由主循环中的 `handlePresets()` → `doSaveState()` 完成，且会等 `strip.isUpdating()` 清零（`presets.cpp:32-33` 注释："accessing FS during sendout causes glitches"）。**读路径同理**：`applyPreset()` 只是入队，`handlePresets()` 才执行。

**播放列表**是载荷含 `playlist` 对象的预设：`ps[]`（预设 ID）、`dur[]`（1/10 秒）、`transition[]`、`repeat`、`end`、`r`。最多 100 项，支持**一层嵌套**。`repeat=0` 无限循环，`repeat<0` 无限 + 随机，`end==255` 表示"恢复播放列表前的预设"。

### 9.6 版本与迁移

- **`VERSION`** = `2607201`（[`wled.h:10`](../wled00/wled.h#L10)），代号 `Kagayaki`。这是**配置 schema 版本**，写入每个配置的 `vid` 键与 `/json/info`。
- **`WLED_VERSION`** 是构建字符串，由 `pio-scripts/set_metadata.py` 注入到 `wled_metadata.cpp` 的 `wled_metadata_t` 结构，放在专用链接段 `BUILD_METADATA_SECTION` 中，供 OTA 在固定偏移读取。

迁移以 `vid` 为枢纽（[`cfg.cpp:52`](../wled00/cfg.cpp#L52)）：

```c
long vid = doc[F("vid")] | VERSION;   // 只有首次调用时可用于判断旧版本
```

已有的迁移包括：定时器 `hour==255` 的日出/日落语义变化（`vid < 2605010`）、`if_sync_send["twice"]` → `udpNumRetries`、全局 `hw_led["rev"]` 迁移到 bus 0、单个 `ip`/`gw`/`sn` → 4 元素数组、`linked_remote` 从字符串到数组、gamma 从浮点对到布尔+共享值。

**损坏恢复阶梯**（[`wled.cpp:523-527`](../wled00/wled.cpp#L523-L527)）：`verifyConfig()` 失败 → `restoreConfig()`（从 `/bkp.cfg.json`）→ `resetConfig()`（重命名为 `/rst.cfg.json` 并重启）。另有 `handleBootLoop()` 按崩溃次数升级：恢复配置 → 重置 → OTA 回滚 → 导出文件到串口。

### 9.7 安全

| 机制 | 说明 |
|---|---|
| `settingsPIN` | 4 位，存在 `/wsec.json`。`checkSettingsPIN()` 有 3 秒重试冷却（防暴力），主循环 15 分钟空闲后失效 |
| 解锁 JSON | `POST /json` 带 `{"pin":"1234"}` |
| `POST /json/cfg` | 设置了 PIN 时要求 `correctPIN`，否则 401 |
| `/settings/s.js` | PIN 不正确时只返回 `alert('PIN incorrect.')` |
| `otaLock` / `otaPass` | 锁定时 `/update` GET 直接 401；POST 另有子网 + PIN + 锁三重检查 |
| 子网限制 | `POST /settings` 要求 `inLocalSubnet()`；OTA 默认 `otaSameSubnet` |
| `wifiLock` | OTA 锁定时同时禁止修改 Wi-Fi 设置页 |

**注意**：本版本**不存在** `WLED_AUTH`，也**没有** `createSettingsPIN()` —— PIN 要么是编译期常量（`-D WLED_PIN=...`），要么通过安全页设置，没有每设备随机生成。PIN + OTA 密码 + 子网检查就是全部访问控制模型。

---

## 10. Usermod 插件机制

### 10.1 是什么

一个 Usermod 就是一个 **PlatformIO 库**：`usermods/<name>/` 目录 + `library.json`。其中 **`"build": {"libArchive": false}` 是强制的** —— `load_usermods.py` 发现该字段为真会直接 `Exit(1)`，因为静态库模式下链接器会把未经引用的 usermod 连同自注册信息一起丢弃。

Usermod 通过继承 `Usermod` 基类并链接进固件来生效，**没有中心注册文件**。

### 10.2 生命周期钩子（[`fcn_declare.h:360-399`](../wled00/fcn_declare.h#L360-L399)）

纯虚（必须实现）：`setup()`、`loop()`。

| 钩子 | 调用点 | 语义 |
|---|---|---|
| `setup()` | `wled.cpp:546` | 在 `beginStrip()` 之后，`readFromConfig()` 已跑过 |
| `loop()` | `wled.cpp:104` | 紧跟 `userLoop()` |
| `connected()` | `wled.cpp:1090` | 每次 Wi-Fi (重)连 |
| `handleButton(b)` | `button.cpp:278` | 返回 true 则跳过内置按钮处理（**非短路，所有 mod 都会看到**） |
| `handleOverlayDraw()` | `overlay.cpp:94` | 效果之后、`show()` 之前 |
| `onStateChange(callMode)` | `led.cpp:123` | 状态收敛时 |
| `addToConfig()` / `readFromConfig()` | `cfg.cpp:1276` / `cfg.cpp:772` | 配置读写。`readFromConfig` 返回 false 表示"我需要保存默认值" |
| `addToJsonState()` / `readFromJsonState()` / `addToJsonInfo()` | `json.cpp:663` / `:500` / `:895` | JSON 集成 |
| `appendConfigData(Print&)` | `xml.cpp:720` | 向设置页注入 JS（下拉框、帮助文本） |
| `onMqttConnect()` / `onMqttMessage()` | `mqtt.cpp:53` / `:103` | MQTT |
| `onEspNowMessage()` / `onUdpPacket()` | `udp.cpp:905` / `:662` | ESP-NOW / UDP（**短路，首个返回 true 的胜出**） |
| `onUpdateBegin(bool)` | `ota_update.cpp:239` / `:215` | OTA 开始 / 失败 |
| `getUMData(&data)` | `FX.cpp` 多处 | Usermod 间数据交换（音频反应效果用） |
| `getId()` | — | 默认 `USERMOD_ID_UNSPECIFIED`(1) |

**`oappend()` 兼容 shim**（[`fcn_declare.h:386-398`](../wled00/fcn_declare.h#L386-L398)）：`appendConfigData()`（无参）是 `private virtual`，通过公开的 `appendConfigData(Print&)` 重载派发 —— 后者把 `Print&` 存进静态 `oappend_shim`，调用虚函数，再清空。这样老 usermod 里直接调用 `oappend(F("..."))` 仍能编译。

### 10.3 注册机制：链接段

```c
// fcn_declare.h:429
#define REGISTER_USERMOD(x) DYNARRAY_MEMBER(Usermod*, usermods, um_##x, 1) = &x
```

`DYNARRAY_MEMBER` 把 `const` 指针放进 `.dynarray.usermods.1` 段。边界来自 `DECLARE_DYNARRAY(Usermod*, usermods)`，在 [`um_manager.cpp:13`](../wled00/um_manager.cpp#L13) 中**恰好调用一次**，它在 `.dynarray.usermods.0` 和 `.dynarray.usermods.99999` 放出零长哨兵数组。

三段式保障：

1. `REGISTER_USERMOD` 写入指针；
2. `dynarray.py` 通过链接脚本补丁保证该段被 KEEP 住；
3. `validate_modules.py` 用 ELF 调试信息反查，任何声明的 mod 没进镜像就构建失败。

典型用法（文件最后两行）：

```cpp
static UsermodTemperature temperature;
REGISTER_USERMOD(temperature);
```

### 10.4 调度语义（[`um_manager.cpp`](../wled00/um_manager.cpp)）

`UsermodManager` 是一组遍历该段的自由函数，语义有讲究：

| 语义 | 钩子 |
|---|---|
| 广播（全部调用） | `setup` `connected` `loop` `handleOverlayDraw` `appendConfigData` `addToJsonState` `addToJsonInfo` `readFromJsonState` `addToConfig` `onMqttConnect` `onUpdateBegin` `onStateChange` |
| **短路 OR**（首个 true 胜出） | `getUMData` `onMqttMessage` `onEspNowMessage` `onUdpPacket` |
| **非短路 OR**（都调用，返回值只告诉调用方是否跳过默认行为） | `handleButton` |
| **AND**（全部返回 true 才为 true） | `readFromConfig` |

另有 `lookup(mod_id)` 按 ID 取另一个 mod 的指针，支持 mod 间互调。

---

## 11. 端到端典型流程

### 11.1 浏览器改颜色

```
UI → POST /json  {"seg":[{"col":[[255,0,0]]}]}
  → AsyncCallbackJsonWebHandler (wled_server.cpp:422)
      deserializeJson 到全局 pDoc（持 JSON_LOCK_SERVER）
      deserializeState(root)                          json.cpp:370
        写入 Segment::colors[]
        stateUpdated(CALL_MODE_DIRECT_CHANGE)         led.cpp:87
          ├─ setValuesFromFirstSelectedSeg()
          ├─ currentPreset = 0        （不再是某个预设了）
          ├─ notify(callMode)         → UDP 广播给同步组内的其他 WLED
          └─ interfaceUpdateCallMode = callMode
  → 响应 {"success":true}

下一轮 loop:
  handleTransitions() → updateInterfaces()
      sendDataWs()  → WebSocket 推送新状态给所有 UI
      publishMqtt() → MQTT 发布
  strip.service()
      Segment 效果函数按新颜色重绘 → blendSegment → show()
      gamma / CCT / ledmap → BusManager::show() → NeoPixelBus → RMT/I2S → 灯亮
```

### 11.2 E1.31 (sACN) 实时输入

```
网络 → ESPAsyncE131 回调 → handleE131Packet(P_E131)     e131.cpp:101
  优先级检查（e131Priority != 0 时）
  乱序序号检查
  handleDMXData(uni, ch, data, REALTIME_MODE_E131, prev) e131.cpp:177
      switch (DMXMode):
        SINGLE_RGB / MULTIPLE_RGB → 逐像素写入
        PRESET  → applyPreset()
        EFFECT  → 写 FX/speed/palette/colors
      realtimeLock(realtimeTimeoutMs, REALTIME_MODE_E131)
      e131NewData = true

下一轮 loop:
  handleNotifications()                                   udp.cpp:475
      if (e131NewData && millis() - getLastShow() > 15)
          strip.show()          ← 15 ms 节流
      超时检查：millis() > realtimeTimeout → exitRealtime()

注意：loop() 第 131 行的 realtimeMode 闸门此时生效，
      strip.service() 被跳过 —— 效果引擎完全让位给实时数据。
```

### 11.3 按键 → 预设 → 播放列表

```
handleIO() → handleButton()                              button.cpp:262
  先问 UsermodManager::handleButton(b)，true 则跳过
  短按 → shortPressAction(b) → applyPreset(macroButton, CALL_MODE_BUTTON_PRESET)
                                        ↑ 只是入队

下一轮 loop:
  handlePresets()                                        presets.cpp:145
      从 /presets.json 读对象（异步，等 strip.isUpdating() 清零）
      若含 "win" → handleSet("win&IN&" + ...)  传统 API 路径
      否则 → deserializeState(fdo, CALL_MODE_NO_NOTIFY, presetId)
      若含 "playlist" → loadPlaylist()
  handlePlaylist()                                       playlist.cpp:151
      按 dur[] 计时推进到下一条，逐条 applyPreset()
```

---

## 12. 扩展点与开发注意事项

### 12.1 常见扩展位置

| 需求 | 修改点 |
|---|---|
| 新增效果 | 在 [`FX.cpp`](../wled00/FX.cpp) 写 `void mode_xxx(void)`，在 `FX.h` 加 `FX_MODE_XXX`，在 `setupEffectData()` 里 `addEffect()` |
| 新增芯片类型 | `const.h` 加 `TYPE_*`，`bus_wrapper.h` 加 `B_*` 宏与 `getI()` 分支，`bus_manager.cpp` 加 `setPixelColor` 分支与 `getLEDTypes()` 条目 |
| 新增协议 | 参照 `udp.cpp` / `e131.cpp`，入口处调用 `realtimeLock()` + 写像素 + 置 `e131NewData` |
| 新增设置项 | `wled.h` 加 `WLED_GLOBAL`，`cfg.cpp` 的 `serializeConfig`/`deserializeConfig` 各加一处，`set.cpp` 加表单处理，`wled00/data/settings_*.htm` 加控件 |
| 新增 API 端点 | `wled_server.cpp` 的 `initServer()` 注册，`json.cpp` 的 `serveJson()` 加分支 |
| 新增功能模块 | **优先用 Usermod**，不要改核心 |

### 12.2 必须遵守的约束

1. **构建顺序**：`npm ci && npm run build` 必须早于 `pio run`，否则 `html_*.h` 缺失导致编译失败。
2. **不要提交生成文件**：`wled00/html_*.h` / `js_*.h` 是构建产物。
3. **JSON 缓冲是单例**：任何使用 `pDoc` 的代码必须 `requestJSONBufferLock()` / `releaseJSONBufferLock()` 成对，抢不到锁时用 `deferResponse()`（HTTP）或下一轮重试（预设）。
4. **不要在 LED 刷新窗口做 Flash I/O**：写入一律入队，由主循环的 `handlePresets()` / `serializeConfigToFS()` / `closeFile()` 执行。
5. **改动几何或配置前先 `strip.suspend()`**，改完 `strip.resume()`，必要时 `strip.waitForIt()`。
6. **Usermod 的 `library.json` 必须 `libArchive: false`**，否则构建直接失败。
7. **新增的全局变量走 `WLED_GLOBAL` / `_INIT()` 宏**，不要引入分散的 `extern` 块。
8. **`Segment::_currentSegment` 在 `service()` 之外指向 segment 0** —— `SEGMENT` / `SEGENV` 宏只能在该窗口内使用。

### 12.3 已知的坑

- `blendingStyle`（过渡样式）是**全局**的，无法按段设置；可变的只有 `SEGMENT.blendMode`（合成模式）。
- `addEffect()` 失败返回 255，而 `Segment::setMode()` 会跳过 `"RSVD"` 槽位 —— usermod 注册失败会**静默降级**为 `mode_static` 而非报错。
- `BusDigital::show()` 传给 `PolyBus::show()` 的第三个参数是 `_skip`，语义是"没有跳过的 LED 就不需要缓冲一致性检查"，与直觉相反。
- `PolyBus::getPixelColor()` 处理了高半字节 W 交换 1/2/3，但**没有**处理 4（WW↔CW），而写路径有。
- `Bus::isDigital()` 对 2 脚 SPI 类型也返回 true，因此调用处普遍要写 `isDigital(type) && !is2Pin(type)`。
- `TYPE_GS8608`(23) 与 `TYPE_DIGITAL_1CH`(18) 在 `PolyBus::getI()` 中不可达，也未出现在 `BusDigital::getLEDTypes()` 里。
- `FX_MODE_BLENDS` 是一个效果名，与混合机制无关。

---

## 附录：快速定位表

| 想找什么 | 去哪里 |
|---|---|
| 程序入口 | [`wled_main.cpp:18`](../wled00/wled_main.cpp#L18) |
| 主循环 | [`wled.cpp:59`](../wled00/wled.cpp#L59) |
| 初始化 | [`wled.cpp:388`](../wled00/wled.cpp#L388) |
| Wi-Fi 状态机 | [`wled.cpp:962`](../wled00/wled.cpp#L962) |
| 渲染入口 | [`FX_fcn.cpp:1306`](../wled00/FX_fcn.cpp#L1306) |
| 像素输出 | [`FX_fcn.cpp:1726`](../wled00/FX_fcn.cpp#L1726) |
| 总线抽象 | [`bus_manager.h:113`](../wled00/bus_manager.h#L113) |
| NeoPixelBus 封装 | [`bus_wrapper.h:352`](../wled00/bus_wrapper.h#L352) |
| 实时模式入口 | [`udp.cpp:408`](../wled00/udp.cpp#L408) |
| 状态反序列化 | [`json.cpp:370`](../wled00/json.cpp#L370) |
| 配置反序列化 | [`cfg.cpp:47`](../wled00/cfg.cpp#L47) |
| HTTP 路由表 | [`wled_server.cpp:349`](../wled00/wled_server.cpp#L349) |
| Usermod 基类 | [`fcn_declare.h:360`](../wled00/fcn_declare.h#L360) |
| 编译期常量 | [`const.h`](../wled00/const.h) |
| 全局变量 | [`wled.h:262`](../wled00/wled.h#L262) |
