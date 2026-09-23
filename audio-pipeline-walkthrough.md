# 小智音频管线与屏幕状态链路 · 代码走读笔记

> 适用硬件：`bread-compact-wifi`（ESP32-S3）+ I²S 麦克风 + I²S 喇叭功放 + 0.91 寸 SSD1306 128×32 OLED
> SDK：ESP-IDF v6.x（FreeRTOS）
>
> 本文回答两个问题：
> 1. 声音从麦克风进来、到通过网络发出去，中间经过了哪些任务和队列？它们怎么协作？
> 2. 从麦克风采集到 **OLED 屏幕上状态改变**，完整的代码调用路径是什么？

---

## 目录

- [1. 一张图看懂全局](#1-一张图看懂全局)
- [2. 前置知识：声音在计算机里的样子](#2-前置知识声音在计算机里的样子)
- [3. 硬件层：I²S、DMA 与 NoAudioCodec](#3-硬件层i²sdma-与-noaudiocodec)
- [4. FreeRTOS 任务清单（谁在干活）](#4-freertos-任务清单谁在干活)
- [5. 队列清单（任务之间的传送带）](#5-队列清单任务之间的传送带)
- [6. 事件组清单（任务之间的"按铃"）](#6-事件组清单任务之间的按铃)
- [7. 上行数据流：麦克风 → 网络（逐站代码路径）](#7-上行数据流麦克风--网络逐站代码路径)
- [8. 下行数据流：网络 → 喇叭（逐站代码路径）](#8-下行数据流网络--喇叭逐站代码路径)
- [9. 核心：AFE 音频引擎内部机制](#9-核心afe-音频引擎内部机制)
- [10. 完整时序：唤醒词 → 屏幕状态改变](#10-完整时序唤醒词--屏幕状态改变)
- [11. 屏幕显示侧：Display → LVGL → SSD1306](#11-屏幕显示侧display--lvgl--ssd1306)
- [12. 线程安全协作规则（为什么代码长这样）](#12-线程安全协作规则为什么代码长这样)
- [13. 背压、丢帧与延迟控制](#13-背压丢帧与延迟控制)
- [14. 省电：音频输入输出的自动开关](#14-省电音频输入输出的自动开关)
- [15. 关键参数速查表](#15-关键参数速查表)
- [16. 动手调试切入点](#16-动手调试切入点)

---

## 1. 一张图看懂全局

先给结论图，后面每一节都在解释这张图。图中有 **4 个 FreeRTOS 任务**、**5 个有界队列**（另有 1 个辅助时间戳队列）、**2 组事件位**。

```
                        ┌──────────────────────── 主任务 (Application::Run, prio=10) ───────────────────────┐
                        │                                                                                   │
   服务器(云端)          │  MAIN_EVENT_SEND_AUDIO → PopPacketFromSendQueue() → protocol_->SendAudio()         │
        ▲               │                                                                      ▲            │
        │               │                                                              通知协议层          │
        │ Opus二进制帧   │                                                                                   │
  ┌─────┴──────┐        │   MAIN_EVENT_WAKE_WORD_DETECTED / VAD_CHANGE / STATE_CHANGED / CLOCK_TICK …        │
  │ WebSocket  │◄───────┴───────────────────────────────────────────────────────────────────────────────────┤
  │ (或MQTT+UDP)│                                                                                            │
  └─────┬──────┘         网络接收回调 (OnData, 网络任务上下文)                                                │
        │                                                                        │ ① PushPacketToDecodeQueue
        │ ② OnIncomingJson ──► 解析JSON ──► Schedule(...) ──► 主任务里改屏幕状态   │   (仅 Speaking 状态)
        │                                                                        ▼
        │                                            ┌─────────────────────────────────────────────┐
        │                                            │  OpusCodecTask (prio=2, 24KB 栈)             │
        │                              ┌────────────►│  - 解码: decode_queue → Opus解码 → playback  │
        │                              │             │  - 编码: encode_queue → Opus编码 → send      │
        │                              │             └───────┬───────────────────────▲─────────────┘
        │                              │                     │ ③ PCM                 │ ④ Opus包
        │                              │                     ▼                       │
        │                              │             audio_playback_queue_   audio_send_queue_
        │                              │              (容量2, PCM)             (容量40, Opus)
        │                              │                     │                       ▲
        │                              │                     ▼                       │ on_send_queue_available
        │                              │             AudioOutputTask (prio=4)          │ → MAIN_EVENT_SEND_AUDIO
        │                              │                     │ ⑤ OutputData(PCM)      │
        │                              │                     ▼                       │
        │                              │             I²S TX → DMA → 喇叭功放            │
        │                              │                                             │
        │                              │   ┌─────────────────────────────────────────┴──────────┐
        │                              │   │ AudioInputTask (prio=8, S3上钉在Core0)                │
        │                              │   │  循环等待 AS_EVENT_WAKE_WORD_RUNNING /               │
        │                              │   │             AS_EVENT_AUDIO_PROCESSOR_RUNNING …       │
        │                              │   │  每次 ReadAudioData() 读 160 采样点(10ms)            │
        │                              └───┤  → audio_engine_->Feed(data)                          │
        │                                  └──────────────────────┬──────────────────────────────┘
        │                                                         │ 10ms PCM
        │                                                         ▼
        │                                  ┌────────────────────────────────────────────┐
        │                                  │ AFE ProcessingTask ("audio_afe", prio=3)    │
        └──────────────────────────────────┤  feed(麦克风数据+可选参考声)                  │
                                           │  → WakeNet 唤醒检测 / VAD / AEC             │
                                           │  → fetch 结果 → 攒成 60ms 帧 → output 回调   │
                                           └────────────────────────────────────────────┘

  麦克风(I²S) ──DMA──► I²S RX                                                    喇叭(I²S)
```

**三句话总结协作机制：**

1. **任务之间不直接调用，只通过"队列传数据、事件位按铃"通信。**
2. **所有需要修改应用状态的回调（音频任务、网络任务、按键）都不自己改状态，而是按铃或 `Schedule()`，由主任务串行处理。**
3. **所有队列都是有界的。音频是实时数据，宁可丢旧帧，也不能阻塞流水线或让延迟无限增长。**

---

## 2. 前置知识：声音在计算机里的样子

| 名词   | 大白话解释                                                           | 本项目的值                                                                                                                           |
| ------ | -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| PCM    | 最原始的声音数据：一串数字，每个数字代表某一瞬间声音的大小（采样值） | `int16_t`，即每个采样点占 2 字节，范围 −32768~32767                                                                                  |
| 采样率 | 每秒采多少次                                                         | 麦克风 **16000 Hz**（人声识别够用）；喇叭 **24000 Hz**（音质更好），见 [config.h:6-7](main/boards/bread-compact-wifi/config.h#L6-L7) |
| 声道   | 单声道/立体声                                                        | 单声道（mono）                                                                                                                       |
| Opus   | 一种音频**压缩格式**，专为人声通话设计，延迟低、压缩率高             | 每 **60ms** 压成一帧                                                                                                                 |
| Ogg    | 一种"容器"，里面装 Opus 帧；内置提示音是 `.ogg` 文件                 | 见 `main/assets/`                                                                                                                    |

**为什么必须压缩？** 原始上行 PCM = 16000 × 2 字节 = **每秒 32 KB**，网络扛不住实时传输。Opus 压缩后每秒约十几 KB，且每帧只有 60ms，延迟可控。

**记住本项目的"时间节拍"换算（后面到处出现）：**

- 麦克风读一次：**160 个采样点 = 10ms**（16000 × 0.01）
- AFE 喂入块（feed chunk）：S3 上通常 512 采样点 ≈ **32ms**
- Opus 一帧：**960 采样点 = 60ms**（16000 × 0.06）
- 喇叭侧 60ms = **1440 采样点**（24000 × 0.06）

---

## 3. 硬件层：I²S、DMA 与 NoAudioCodec

### 3.1 什么是 I²S 和 DMA

- **I²S**：芯片和音频设备之间传声音的专用总线。本板麦克风 3 根线（SCK 时钟 / WS 帧同步 / DIN 数据入），喇叭 3 根线（BCLK / LRCK / DOUT 数据出）。
- **DMA**：外设和内存之间的"自动搬运工"。声音数据由硬件直接在 I²S 外设和内存缓冲区之间搬运，不需要 CPU 干预。CPU 只在 DMA 搬完一块时被通知。

引脚映射见 [config.h:14-19](main/boards/bread-compact-wifi/config.h#L14-L19)（Simplex 单工模式，麦克风和喇叭各用一套 I²S 控制器）：

```
麦克风: WS=GPIO4,  SCK=GPIO5, DIN=GPIO6   (I²S 控制器 1)
喇叭:  DOUT=GPIO7, BCLK=GPIO15, LRCK=GPIO16 (I²S 控制器 0)
```

### 3.2 为什么叫 "NoAudioCodec"

"Codec"（编解码芯片）本指 ES8311、ES8388 这类通过 I²C 配置的真实声卡芯片。面包板方案没有这种芯片，麦克风和功放直接走 I²S，所以驱动类叫 `NoAudioCodec`（"没有声卡芯片"）。**注意别和 Opus 软件编解码搞混——那是 `OpusCodecTask` 干的另一件事。**

类继承关系：

```
AudioCodec            (抽象基类，main/audio/audio_codec.h)
 └─ NoAudioCodec      (直接驱动 I²S，main/audio/codecs/no_audio_codec.cc)
     ├─ NoAudioCodecSimplex   ← 本板使用：收发分开，两条 I²S 通道
     └─ NoAudioCodecDuplex    收发共用一条通道（带参考声，可做硬件 AEC）
```

### 3.3 通道创建（构造函数）

板子在 `GetAudioCodec()` 里创建，见 [compact_wifi_board.cc:173-179](main/boards/bread-compact-wifi/compact_wifi_board.cc#L173-L179)。

通道参数在 [no_audio_codec.cc:85-93](main/audio/codecs/no_audio_codec.cc#L85-L93)：

```cpp
i2s_chan_config_t chan_cfg = {
    .id = 0,                                  // 喇叭用控制器0
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM, // 6 个 DMA 描述符
    .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,// 每个描述符 240 帧
    ...
};
i2s_new_channel(&chan_cfg, &tx_handle_, nullptr);  // 只发(TX)
```

I²S 标准模式关键配置 [no_audio_codec.cc:106-113](main/audio/codecs/no_audio_codec.cc#L106-L113)：物理位宽 **32 bit**、单声道；麦克风通道类似，控制器 id=1，采样率 16k，见 [no_audio_codec.cc:136-144](main/audio/codecs/no_audio_codec.cc#L136-L144)。

DMA 缓冲大小常量见 [audio_codec.h:15-16](main/audio/audio_codec.h#L15-L16)。

### 3.4 底层读 / 写：位宽转换与音量曲线

**写（播放）** [no_audio_codec.cc:218-239](main/audio/codecs/no_audio_codec.cc#L218-L239)：

```cpp
int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536; // 音量用平方曲线
// int16 PCM → 乘音量因子 → int32，削波保护
i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), ...);
```

- 应用层用 16bit PCM，硬件是 32bit，写出前扩展并乘音量。
- 音量 0–100，用 `pow(v/100, 2)` 二次曲线，使小音量调节更细腻（符合人耳感知）。

**读（录音）** [no_audio_codec.cc:241-256](main/audio/codecs/no_audio_codec.cc#L241-L256)：

```cpp
std::vector<int32_t> bit32_buffer(samples);
i2s_channel_read(rx_handle_, bit32_buffer.data(), ..., 200 /*ms超时*/);
for (...) {
    int32_t value = bit32_buffer[i] >> 12;   // 32bit 右移12位 → 16bit有效范围
    dest[i] = 限幅后转 int16;
}
```

- 读超时 200ms，失败返回 0（上层不会因此退出任务）。
- 通道真正使能/关闭在 [no_audio_codec.cc:258-282](main/audio/codecs/no_audio_codec.cc#L258-L282)，即 `i2s_channel_enable/disable()`。

---

## 4. FreeRTOS 任务清单（谁在干活）

| 任务名                              | 优先级 | 栈大小                         | 运行位置          | 职责                                                                             | 创建位置                                                                                                                                           |
| ----------------------------------- | ------ | ------------------------------ | ----------------- | -------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| **主任务**                          | 10     | 系统默认                       | 主任务            | 事件循环，所有状态决策的唯一入口                                                 | [main.cc:27-28](main/main.cc#L27-L28)，循环见 [application.cc:186-285](main/application.cc#L186-L285)                                              |
| **audio_input**                     | 8      | 6144 B（带处理器时）/ 4096 B   | S3 上钉 **Core0** | 从 I²S 读麦克风数据，喂给音频引擎                                                | [audio_service.cc:127-151](main/audio/audio_service.cc#L127-L151)，循环 [L242-324](main/audio/audio_service.cc#L242-L324)                          |
| **opus_codec**                      | 2      | **24576 B**（Opus 很吃栈）     | 不限核            | Opus 编码 + 解码（一个任务轮询两侧）                                             | [audio_service.cc:164-170](main/audio/audio_service.cc#L164-L170)，循环 [L381-530](main/audio/audio_service.cc#L381-L530)                          |
| **audio_output**                    | 4      | 4096 B（带处理器）/ 2048 B     | 不限核            | 取播放队列 PCM 写入 I²S                                                          | [audio_service.cc:136-160](main/audio/audio_service.cc#L136-L160)，循环 [L326-379](main/audio/audio_service.cc#L326-L379)                          |
| **audio_afe**（AFE ProcessingTask） | 3      | 4096 B（静态分配，栈在 PSRAM） | 不限核            | feed 数据进 AFE、fetch 处理结果（唤醒/VAD）                                      | [afe_audio_engine.cc:222-229](main/audio/engines/afe_audio_engine.cc#L222-L229)，循环 [L406-442](main/audio/engines/afe_audio_engine.cc#L406-L442) |
| LVGL 任务                           | 1      | esp_lvgl_port 默认             | 多核时 Core1      | 定时刷新屏幕、执行 LVGL 动画                                                     | LVGL 初始化 [oled_display.cc:44-51](main/display/oled_display.cc#L44-L51)                                                                          |
| 唤醒词编码任务（临时）              | —      | 24 KB（PSRAM）                 | —                 | 仅 `CONFIG_SEND_WAKE_WORD_DATA` 开启时，把唤醒前后的录音压成 Opus 上传，用完即删 | [afe_audio_engine.cc:536-549](main/audio/engines/afe_audio_engine.cc#L536-L549)                                                                    |

> 优先级数字越大越先被调度。主任务 10 最高，保证状态响应及时；opus_codec 只有 2，避免占满 CPU 影响实时录音放音。

**为什么音频输入任务在 S3 上钉在 Core0？** 见 [audio_service.cc:127-133](main/audio/audio_service.cc#L127-L133) 的 `xTaskCreatePinnedToCore`：ESP-SR 的 AFE 内部计算任务默认在另一个核，分开钉核可减少争抢、降低音频抖动。

**C3/C5/C6 等资源少的芯片**不使用 AFE，改用 `LiteAudioEngine`（精简引擎），选择逻辑见 [audio_service.cc:20-24](main/audio/audio_service.cc#L20-L24) 和 [L82-86](main/audio/audio_service.cc#L82-L86)。本板是 S3，走 AFE。

---

## 5. 队列清单（任务之间的传送带）

所有队列都是模板类 `FixedQueue<T, Capacity>`（[fixed_queue.h](main/audio/fixed_queue.h)）：定长环形数组，`push_back` 在满时返回 `false`，`pop_front` 在空时直接 abort（编程错误保护）。

| #   | 队列                    | 容量                   | 装什么                                     | 生产者 → 消费者                                  |
| --- | ----------------------- | ---------------------- | ------------------------------------------ | ------------------------------------------------ |
| 1   | `audio_encode_queue_`   | **2**                  | `AudioTask`（60ms PCM，待编码）            | AFE 引擎 output 回调 → OpusCodecTask             |
| 2   | `audio_send_queue_`     | **40**（=2400ms）      | `AudioStreamPacket`（已编码 Opus，待发送） | OpusCodecTask → 主任务 → 网络                    |
| 3   | `audio_decode_queue_`   | **20**（=1200ms）      | `AudioStreamPacket`（收到的 Opus，待解码） | 协议层收包 → OpusCodecTask                       |
| 4   | `audio_playback_queue_` | **2**                  | `AudioTask`（解码后 PCM，待播放）          | OpusCodecTask → AudioOutputTask                  |
| 5   | `audio_testing_queue_`  | ≈**166**（10000ms/60） | Opus 包（配网模式下的"录音自检"）          | AudioInputTask → 结束后整体 swap 进 decode_queue |
| 辅  | `timestamp_queue_`      | **3**                  | 时间戳（服务端 AEC 用）                    | AudioOutputTask → 上行取包时附带                 |

定义见 [audio_service.h:185-198](main/audio/audio_service.h#L185-L198)，容量宏见 [audio_service.h:40-46](main/audio/audio_service.h#L40-L46)。

**为什么 encode/playback 只有 2、而 send/decode 有 40/20？**

- encode、playback 里放的是**大块 PCM**，且紧邻生产/消费端，缓冲大了只会增加延迟。
- send、decode 里放的是**很小的 Opus 包**，要留出"网络抖动缓冲"：网络偶尔卡几百毫秒，靠队列里的存货平滑过去，声音不会断断续续。
- 但缓冲也不能无限大——1.2s / 2.4s 是延迟与流畅度的折中。

**队列的统一保护方式：** 一把互斥锁 `audio_queue_mutex_` + 条件变量 `audio_queue_cv_`。所有队列操作都在锁内，消费者用条件变量 `wait()` 睡眠，数据到来时 `notify_all()` 唤醒，不浪费 CPU 空转。

---

## 6. 事件组清单（任务之间的"按铃"）

项目有**两个独立的事件组**，分别服务音频层和应用层。

### 6.1 AudioService 自己的事件组（控制音频子系统内部）

| 位                                  | 含义                                         |
| ----------------------------------- | -------------------------------------------- |
| `AS_EVENT_AUDIO_TESTING_RUNNING`    | 配网自检模式运行中                           |
| `AS_EVENT_WAKE_WORD_RUNNING`        | 唤醒词检测运行中                             |
| `AS_EVENT_AUDIO_PROCESSOR_RUNNING`  | 语音处理（VAD/上行）运行中                   |
| `AS_EVENT_AUDIO_INPUT_STOP_REQUEST` | 请求输入任务关闭麦克风（省电，由定时器置位） |

定义 [audio_service.h:51-54](main/audio/audio_service.h#L51-L54)。AudioInputTask 睡眠等待前三个"运行位"之一，见 [audio_service.cc:243-250](main/audio/audio_service.cc#L243-L250)。

### 6.2 Application 的主事件组（唤醒主任务）

| 位                                                          | 谁置位                           | 主任务做什么                         |
| ----------------------------------------------------------- | -------------------------------- | ------------------------------------ |
| `MAIN_EVENT_SEND_AUDIO`                                     | OpusCodecTask（send 队列有货）   | 取出 Opus 包发网络                   |
| `MAIN_EVENT_WAKE_WORD_DETECTED`                             | 音频引擎检测到唤醒词             | 开始一次对话                         |
| `MAIN_EVENT_VAD_CHANGE`                                     | AFE 的 VAD 状态变化              | LED 状态变化                         |
| `MAIN_EVENT_STATE_CHANGED`                                  | 状态机监听器                     | 执行进入新状态的全部动作（含改屏幕） |
| `MAIN_EVENT_NETWORK_CONNECTED/DISCONNECTED`                 | 网络回调                         | 创建/重置协议                        |
| `MAIN_EVENT_TOGGLE_CHAT / START_LISTENING / STOP_LISTENING` | 按键                             | 对话开关、按住说话                   |
| `MAIN_EVENT_CLOCK_TICK`                                     | 1 秒周期定时器                   | 刷新状态栏                           |
| `MAIN_EVENT_PLAYBACK_DRAINED`                               | AudioOutputTask（队列排空）      | 通知播放结束、恢复听音               |
| `MAIN_EVENT_ERROR / ACTIVATION_DONE / SCHEDULE`             | 协议错误 / 激活任务 / Schedule() | 报错弹窗 / 激活完成 / 执行插队函数   |

定义 [application.h:25-38](main/application.h#L25-L38)；主任务等待的全部位见 [application.cc:179-184](main/application.cc#L179-L184)。

---

## 7. 上行数据流：麦克风 → 网络（逐站代码路径）

以"设备已进入 Listening 状态，用户正在说话"为例，共 7 站。

### 第 1 站：输入任务被唤醒，读 10ms 数据

[audio_service.cc:309-317](main/audio/audio_service.cc#L309-L317)：

```cpp
if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
    int samples = 160;                          // 10ms @16k
    std::vector<int16_t> data;
    if (ReadAudioData(data, 16000, samples)) {
        audio_engine_->Feed(std::move(data));   // → 第2站
    }
}
```

### 第 2 站：ReadAudioData 打开麦克风、读 I²S、必要时重采样

[audio_service.cc:195-240](main/audio/audio_service.cc#L195-L240)：

1. 若麦克风未使能：切换定时器为 1 秒检查间隔，`codec_->EnableInput(true)`（首次读时才真正开 I²S，省电）。
2. 按目标采样率读数据（`codec_->InputData()` → 3.4 节的底层 `Read()`）。
3. 若麦克风原始率不是 16k，用 `input_resampler_` 重采样到 16k。
4. 更新 `last_input_time_`（给省电定时器用）。

### 第 3 站：Feed 进 AFE 引擎，攒成整块喂入

[afe_audio_engine.cc:252-271](main/audio/engines/afe_audio_engine.cc#L252-L271)：

```cpp
input_buffer_.insert(...data...);                       // 10ms 追加进缓冲
size_t chunk_size = afe_iface_->get_feed_chunksize(...) // 整块大小(如512=32ms)
                   * codec_->input_channels();
while (input_buffer_.size() >= chunk_size) {
    afe_iface_->feed(afe_data_, input_buffer_.data());  // 整块喂给 AFE
    input_buffer_.erase(...);
}
```

> 若未开启"AFE 处理语音"，`Feed()` 开头会走 `OutputRawAudio()` 旁路（[L253-256](main/audio/engines/afe_audio_engine.cc#L253-L256)），直接把原始 PCM 切回 60ms 帧送出，省掉 AFE 计算。

### 第 4 站：AFE ProcessingTask fetch 结果，攒成 60ms 帧

[afe_audio_engine.cc:473-503](main/audio/engines/afe_audio_engine.cc#L473-L503)（`HandleVoiceResult`）：

1. 先处理 VAD 状态（见下节）。
2. fetch 出的处理后 PCM 追加进 `output_buffer_`。
3. 每攒够 `frame_samples_`（960 = 60ms）调一次 `output_callback_`。

回调在 AudioService 里注册 [audio_service.cc:87-89](main/audio/audio_service.cc#L87-L89)：

```cpp
audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
    PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data)); // → 第5站
});
```

### 第 5 站：进入编码队列（满则丢最旧帧）

[audio_service.cc:569-595](main/audio/audio_service.cc#L569-L595)：

```cpp
if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
    audio_encode_queue_.pop_front();          // 实时音频：丢旧不阻塞
    ...统计...
}
audio_encode_queue_.push_back(std::move(task));
audio_queue_cv_.notify_all();                 // 唤醒 OpusCodecTask
```

### 第 6 站：OpusCodecTask 编码，放入发送队列

[audio_service.cc:466-515](main/audio/audio_service.cc#L466-L515)：

```cpp
if (opus_encoder_ && task.pcm.size() == encoder_frame_size_) { // 必须恰好 960 采样
    esp_opus_enc_process(opus_encoder_, &in, &out);            // Opus 压缩
    ...
    if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
        audio_send_queue_.pop_front();           // 发送队列满也丢最旧
    }
    audio_send_queue_.push_back(std::move(packet));
}
callbacks_.on_send_queue_available();           // → 第7站：按铃
```

编码器配置（16k 单声道、60ms 帧、VBR、DTX、复杂度 0 最省算力）见 [audio_service.h:67-80](main/audio/audio_service.h#L67-L80)。

### 第 7 站：主任务取出，交给协议层发送

主任务收到铃 [application.cc:239-251](main/application.cc#L239-L251)：

```cpp
while (auto packet = audio_service_.PopPacketFromSendQueue()) {
    if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
        while (audio_service_.PopPacketFromSendQueue()); // 发送失败：清空队列，防止死锁
        break;
    }
}
```

`SendAudio()` 在 WebSocket 下加二进制帧头（`BinaryProtocol2/3`）发出，见 [websocket_protocol.cc:25-54](main/protocols/websocket_protocol.cc#L25-L54)。

**上行全链路一句话：**

```
I²S DMA → audio_input(10ms) → AFE feed(32ms块) → AFE fetch → 攒60ms
       → encode_queue(2) → Opus编码 → send_queue(40) → 主任务 → WebSocket → 云端
```

---

## 8. 下行数据流：网络 → 喇叭（逐站代码路径）

### 第 1 站：网络任务收到数据，区分二进制 / JSON

WebSocket 的 `OnData` 回调注册于 [websocket_protocol.cc:108-159](main/protocols/websocket_protocol.cc#L108-L159)，它运行在**网络任务上下文**：

- **二进制帧**：按协议版本解析帧头（v2/v3），构造 `AudioStreamPacket`，调 `on_incoming_audio_`。
- **文本帧**：`cJSON_ParseWithLength` 解析，`hello` 消息本地处理（含服务器采样率等），其余调 `on_incoming_json_`。

### 第 2 站：声音包入解码队列（只在 Speaking 状态）

[application.cc:553-557](main/application.cc#L553-L557)：

```cpp
protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
    if (GetDeviceState() == kDeviceStateSpeaking) {
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    }
});
```

入队 [audio_service.cc:609-629](main/audio/audio_service.cc#L609-L629)：满了默认直接返回 `false`（不等），保证网络任务不被拖住；同时记录"播放尚未排空"。

### 第 3 站：OpusCodecTask 解码（背压：播放队列满就先不解）

[audio_service.cc:384-464](main/audio/audio_service.cc#L384-L464)。任务睡眠等待条件值得注意 [L384-388](main/audio/audio_service.cc#L384-L388)：

```cpp
audio_queue_cv_.wait(lock, [this]() {
    return ... || !audio_encode_queue_.empty() ||
           (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE); // 背压闸门
});
```

解码流程：

1. `SetDecodeSampleRate()`：若服务器发来的采样率/帧长与当前解码器不同，**动态重建解码器**（[audio_service.cc:532-567](main/audio/audio_service.cc#L532-L567)），并按需创建输出重采样器（例如服务器 16k、喇叭 24k）。
2. `esp_opus_dec_decode()` 解压成 PCM。
3. 采样率不匹配时经 `output_resampler_` 转换。
4. 重新拿锁，确认期间没有发生 Stop/重置（`playback_generation_` 代数校验），才推入播放队列。

### 第 4 站：输出任务写 I²S

[audio_service.cc:326-376](main/audio/audio_service.cc#L326-L376)：

1. 睡眠等播放队列非空。
2. 取出一帧；若喇叭未使能，先 `EnableOutput(true)`。
3. `codec_->OutputData(task.pcm)` → 3.4 节底层 `Write()`（16→32bit、音量）→ I²S DMA → 喇叭。
4. 服务端 AEC 模式下记录时间戳。
5. 写完检查"是否全部排空"，若是则触发 `on_playback_drained` 回调。

### 第 5 站：排空通知，决定下一步状态

回调 → `MAIN_EVENT_PLAYBACK_DRAINED`，主任务处理 [application.cc:214-225](main/application.cc#L214-L225)：通知 `NotifyPlayer`；若是"自动模式下等待排空再开始听音"，此刻才启动 `StartListeningAudio()`。

**下行全链路一句话：**

```
云端 → WebSocket OnData → decode_queue(20) → Opus解码(+重采样) → playback_queue(2)
       → audio_output → I²S DMA → 喇叭；排空 → PLAYBACK_DRAINED
```

> 提示音（welcome/popup/低电量等）走同一条解码链：`PlaySound()` 用 `OggDemuxer` 解出 Opus 包，以 `wait=true` 阻塞式推入解码队列，见 [audio_service.cc:764-786](main/audio/audio_service.cc#L764-L786)。

---

## 9. 核心：AFE 音频引擎内部机制

AFE（Audio Front-End，音频前端）来自乐鑫 **ESP-SR** 组件，是唤醒词、VAD、AEC 的运行框架。

### 9.1 AFE 的创建与管线配置

[afe_audio_engine.cc:141-204](main/audio/engines/afe_audio_engine.cc#L141-L204) 关键项：

```cpp
afe_config_t* afe_config =
    afe_config_init(input_format.c_str(), models_, AFE_TYPE_FD, AFE_MODE_LOW_COST);
afe_config->aec_init  = codec_->input_reference();  // 有播放参考声道才开 AEC
afe_config->ns_init   = false;                     // 降噪关闭（本配置）
afe_config->vad_init  = kUseAfeForVoiceProcessing; // 语音处理时启用 VAD
afe_config->wakenet_init = (检测器 == WakeNet);
afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM; // 尽量用 PSRAM
```

- `input_format`：麦克风写 `'M'`、参考声道（播放回采）写 `'R'`，见 [L132-139](main/audio/engines/afe_audio_engine.cc#L132-L139)。本板麦克风无参考声道，所以 AEC 默认不可用（需要硬件回采设计的板子才行）。
- `AFE_TYPE_FD`：频域（Frequency Domain）管线，效果好但算力需求高。
- 模型（WakeNet/VAD）在 [afe_audio_engine.cc:66-81](main/audio/engines/afe_audio_engine.cc#L66-L81) 初始化：从 Flash 的 `model` 分区加载。

### 9.2 唤醒词检测：WakeNet 与自定义 MultiNet

两种检测器（枚举 `WakeDetector`，[afe_audio_engine.cc:50-54](main/audio/engines/afe_audio_engine.cc#L50-L54)）：

- **WakeNet**：神经网络唤醒模型，离线本地运行。检测命中时 `result->wakeup_state == WAKENET_DETECTED`，处理见 [afe_audio_engine.cc:444-471](main/audio/engines/afe_audio_engine.cc#L444-L471)。
- **MultiNet**：命令词/自定义唤醒词，走 `CustomWakeWord` 类（`main/audio/wake_words/`）。

命中时的动作（[L463-470](main/audio/engines/afe_audio_engine.cc#L463-L470)）：记录唤醒词 → 清除唤醒使能位（检测到后自动停一次，避免重复触发）→ 更新 AFE 活跃状态 → 调 `wake_word_detected_callback_`。

### 9.3 VAD：判断"说话/停顿"

`HandleVoiceResult` 里根据 `result->vad_state`（`VAD_SPEECH` / `VAD_SILENCE`）边沿触发回调 [afe_audio_engine.cc:477-485](main/audio/engines/afe_audio_engine.cc#L477-L485)。AudioService 回调中更新 `voice_detected_` 并上报 `on_vad_change`，见 [audio_service.cc:90-95](main/audio/audio_service.cc#L90-L95)。主任务在 Listening 状态收到后刷新 LED，见 [application.cc:257-262](main/application.cc#L257-L262)。

VAD 的另一个用途是让服务器知道用户什么时候说完（配合 `tts/stop` 等消息完成自动轮次切换）。

### 9.4 AEC：回声消除

喇叭播放时，麦克风会收到喇叭的声音。AEC 拿"正在播放的参考信号"去抵消麦克风里的回声，是**全双工（边放边听）和播放中唤醒词打断**的前提。

- 硬件 AEC：板子的声卡必须提供播放参考声道（`input_reference()`）。AFE 的 AEC 使能/关闭由 ProcessingTask 在 fetch 前统一切换，见 [afe_audio_engine.cc:375-393](main/audio/engines/afe_audio_engine.cc#L375-L393)。
- 服务端 AEC：设备上报播放时间戳（`timestamp_queue_`），服务器自己做对齐消除。

### 9.5 为什么控制操作都"延迟"到 ProcessingTask 执行

代码里有大量"deferred（延迟处理）"标记：`afe_control_dirty_`、`reset_pending_`、`output_reset_pending_`、`control_generation_`。原因注释写得很清楚：

- WakeNet/AEC 的开关、AFE buffer 复位，**与并发的 `fetch()` 同时进行会损坏环形缓冲状态**。
- 所以别的任务（如主任务）只置标记位，由拥有 fetch 侧的 ProcessingTask 在每轮循环开头统一执行（[L409-417](main/audio/engines/afe_audio_engine.cc#L409-L417)）。
- `control_generation_` 用来丢弃"停用/重开期间那次迟到的旧 fetch 结果"（[L418-426](main/audio/engines/afe_audio_engine.cc#L418-L426)）。

---

## 10. 完整时序：唤醒词 → 屏幕状态改变

这是本文的重点：从声音进麦克风，到 OLED 上文字/表情变化，每一步跨了哪个任务、走了什么机制。

```
【音频任务链】                                    【主任务链】                    【屏幕】

Idle 待机：
 audio_input 睡眠(WAKE_WORD_RUNNING)
   ↓ 每10ms: ReadAudioData → Feed
 AFE ProcessingTask: fetch
   ↓ WakeNet 持续本地推理
 用户说"小智小智" → WAKENET_DETECTED
   ↓ HandleWakeWordResult
   ├─ 清唤醒使能位 / UpdateActiveState
   └─ wake_word_detected_callback_
        ↓ (AudioService 回调)
      on_wake_word_detected
        ↓ MAIN_EVENT_WAKE_WORD_DETECTED  ──────►  主任务醒来
                                                   HandleWakeWordDetectedEvent()
                                                   BeginWakeWordInvoke()
                                                   ↓
                                                  SetDeviceState(Connecting)
                                                   ↓ 状态机校验+通知监听器
                                                  MAIN_EVENT_STATE_CHANGED ──► (见下)
                                                   ↓ Schedule
                                                  ContinueWakeWordInvoke()
                                                   ├─ OpenAudioChannel() (WebSocket)
                                                   ├─ 发送唤醒包/SendWakeWordDetected
                                                   └─ SetListeningMode()
                                                        ↓
                                                       SetDeviceState(Listening)
                                                        ↓ STATE_CHANGED
MAIN_EVENT_STATE_CHANGED 处理:
 HandleStateChangedEvent() [application.cc:990]
   ├─ LED.OnStateChanged()
   ├─ Connecting分支: SetStatus("连接中…") SetEmotion("neutral") SetChatMessage("")
   └─ Listening分支:
        SetStatus("聆听中…") SetEmotion("neutral")
        StartListeningAudio():
          SendStartListening + EnableVoiceProcessing(true)
          (可能先 PlaySound "叮")
              ↓
        屏幕显示"聆听中…"，麦克风开始上行
```

**状态切换 → 屏幕更新的精确路径**（以 Connecting 为例）：

1. `state_machine_.TransitionTo(kDeviceStateConnecting)`：合法性校验（[device_state_machine.h:36](main/device_state_machine.h#L36)），成功后通知所有监听者。
2. Application 注册的监听器 [application.cc:96-98](main/application.cc#L96-L98) 置 `MAIN_EVENT_STATE_CHANGED`。
3. 主任务下一轮循环执行 `HandleStateChangedEvent()` [application.cc:990-1064](main/application.cc#L990-L1064)。
4. switch 命中对应状态分支，调用 `display->SetStatus(...)` / `SetEmotion(...)` / `SetChatMessage(...)`。
5. 这些 Display 方法获取 LVGL 锁、修改 LVGL 对象（详见第 11 节）。
6. LVGL 任务把变化的像素刷给 SSD1306，经 I²C 发到 OLED。

各状态在屏幕上做什么（[application.cc:1002-1063](main/application.cc#L1002-L1063)）：

| 进入状态        | 屏幕动作                          | 音频侧动作                                                   |
| --------------- | --------------------------------- | ------------------------------------------------------------ |
| Idle            | 状态="待命"、清消息、表情=neutral | 关语音处理、**开唤醒检测**                                   |
| Connecting      | "连接中…"、表情 neutral、清字幕   | —                                                            |
| Listening       | "聆听中…"、表情 neutral           | 开语音处理；唤醒词按 Kconfig 配置                            |
| Speaking        | "说话中…"                         | 关语音处理（非实时模式）；AFE 唤醒保留以便打断；ResetDecoder |
| Notifying       | "说话中…"                         | 同 Speaking                                                  |
| WifiConfiguring | —                                 | 关唤醒、关语音处理                                           |

**云端消息驱动的屏幕变化路径**（例如 AI 回答的字幕）：

```
网络任务 OnData(JSON)
  → on_incoming_json_  [application.cc:578]
  → 按 type 分发(tts/stt/llm/…)
  → 不在原地改屏幕！而是 Schedule([...]{ display->SetXxx(...) })   ← 关键
  → MAIN_EVENT_SCHEDULE
  → 主任务执行函数 → Display → LVGL → 屏幕
```

例如：`tts/sentence_start` → `SetChatMessage("assistant", 文本)`（[application.cc:638-652](main/application.cc#L638-L652)）；`stt` → 显示用户语音识别结果（[L654-668](main/application.cc#L654-L668)）；`llm` 带 emotion → `SetEmotion`（[L669-675](main/application.cc#L669-L675)）；`tts/start` → 状态转 Speaking（[L623-627](main/application.cc#L623-L627)）。

**状态栏图标（静音/WiFi/电量/时钟）的刷新路径：**

```
clock_timer (esp_timer, 每1秒) → MAIN_EVENT_CLOCK_TICK
  → 主任务 → display->UpdateStatusBar()
  → LvglDisplay::UpdateStatusBar [lvgl_display.cc:193]
      ├─ 静音图标(音量==0)
      ├─ Idle 时显示系统时钟 HH:MM
      ├─ 电池图标(充电/电量档位/低电量弹窗)
      └─ 网络图标(每10秒)
```

定时器启动见 [application.cc:101](main/application.cc#L101)，tick 处理见 [application.cc:273-284](main/application.cc#L273-L284)。

---

## 11. 屏幕显示侧：Display → LVGL → SSD1306

### 11.1 三层继承与各自职责

```
Display            (抽象接口，main/display/display.h)
 └─ LvglDisplay    (LVGL 通用逻辑：状态栏/通知栏/锁，lvgl_display/)
     └─ OledDisplay (OLED 专用：UI布局、表情、字幕，display/oled_display.cc)
```

- `Display` 定义统一动作：`SetStatus / SetEmotion / SetChatMessage / ShowNotification / UpdateStatusBar`（[display.h:43-56](main/display/display.h#L43-L56)）。基类默认实现基本是空/打日志（`NoDisplay` 无屏时用）。
- `LvglDisplay` 实现了状态文字、通知、状态栏的通用逻辑。
- `OledDisplay` 只实现 OLED 特有的布局与表情/字幕。

### 11.2 SetStatus：改状态文字

[lvgl_display.cc:145-164](main/display/lvgl_display/lvgl_display.cc#L145-L164)：

```cpp
DisplayLockGuard lock(this);                    // 拿 LVGL 全局锁
lv_label_set_text(status_label_, status);
lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN); // 通知栏让位
```

### 11.3 ShowNotification：临时弹窗，自动恢复

[lvgl_display.cc:170-191](main/display/lvgl_display/lvgl_display.cc#L170-L191)：写通知标签、隐藏状态标签、启动一次性定时器 `notification_timer_`，到时自动恢复（网络连接提示等用它）。

### 11.4 SetEmotion：名字 → 字形 → 标签

[oled_display.cc:395-414](main/display/oled_display.cc#L395-L414)：

```cpp
const char* utf8 = noto_emoji_get_utf8(emotion);   // 先查彩色 emoji 表
if (utf8 == nullptr) {
    utf8 = material_symbols_get_utf8(emotion);     // 再查 Material 图标
    emotion_font = large_icon_font;
}
DisplayLockGuard lock(this);
lv_obj_set_style_text_font(emotion_label_, emotion_font, 0);
lv_label_set_text(emotion_label_, utf8);          // 找不到就显示默认 neutral
```

单色 OLED 用 Material 图标的 30px 大字显示"表情"。

### 11.5 SetChatMessage：字幕跑马灯

[oled_display.cc:149-170](main/display/oled_display.cc#L149-L170)：换行替换为空格 → 设置标签文本（128×32 布局里该标签配了 `LV_LABEL_LONG_SCROLL_CIRCULAR` 循环滚动，见 [oled_display.cc:379-392](main/display/oled_display.cc#L379-L392)）。

### 11.6 128×32 的屏幕布局（你的屏幕）

[oled_display.cc:307-393](main/display/oled_display.cc#L307-L393)，横向分两块：

```
┌────────┬──────────────────────────────────────────┐
│        │ 状态栏(16px高): 状态文字 静音 WiFi 电量    │
│ 表情    ├──────────────────────────────────────────┤
│ 32×32  │ 字幕行(16px): AI/用户消息循环滚动          │
└────────┴──────────────────────────────────────────┘
```

### 11.7 LVGL 锁：多任务安全操作 UI

`DisplayLockGuard`（[display.h:104-126](main/display/display.h#L104-L126)）构造时调 `display_->Lock(30000)`（最多等 30 秒），析构解锁。OLED 的实现就是 esp_lvgl_port 的递归互斥锁：[oled_display.cc:145-147](main/display/oled_display.cc#L145-L147)：

```cpp
bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void OledDisplay::Unlock()             { lvgl_port_unlock(); }
```

任何任务改 LVGL 对象前都必须拿锁——这保证 LVGL 任务刷新时不会撞上半修改的对象。

### 11.8 像素如何到屏幕

```
改 LVGL 对象(脏区域)
  → LVGL 任务(周期触发) 计算脏区域像素，1bpp 单色
  → esp_lvgl_port 调 esp_lcd 绘制
  → SSD1306 面板驱动 (managed component)
  → I²C (SDA=41/SCL=42, 400kHz, 地址0x3C) → OLED
```

显示缓冲与面板注册见 [oled_display.cc:53-80](main/display/oled_display.cc#L53-L80)；面板/I²C 初始化见 [compact_wifi_board.cc:51-101](main/boards/bread-compact-wifi/compact_wifi_board.cc#L51-L101)。

---

## 12. 线程安全协作规则（为什么代码长这样）

这是读懂整个项目最重要的"规矩"，来自工程的 `AGENTS.md`：

1. **回调可能跑在别的任务上**（音频回调在 audio_afe、网络回调在网络任务、按键回调在按键处理路径）。
2. **回调里绝不直接修改应用状态**（状态、协议对象、共享数据），而是二选一：
   - `xEventGroupSetBits()` 按铃，让主任务在 `Run()` 循环中处理；
   - `Application::Schedule(func)`，把函数塞进队列（`MAIN_EVENT_SCHEDULE`），在主任务执行。
3. **运行状态只能经 `Application::SetDeviceState()` 改变**，由状态机校验合法性；不允许旁路直接赋值。
4. **LVGL 对象操作必须持 `DisplayLockGuard`**；队列操作必须持 `audio_queue_mutex_`。
5. **不能阻塞主事件循环和音频任务**：所以打开音频通道等慢操作通过 `Schedule()` 延后执行（先让 STATE_CHANGED 完成 UI 更新），见 [application.cc:803-807](main/application.cc#L803-L807)。

典型例子对照：

| 触发源       | 运行任务   | 正确做法                             | 代码                                                              |
| ------------ | ---------- | ------------------------------------ | ----------------------------------------------------------------- |
| 唤醒词命中   | audio_afe  | 回调 → MAIN_EVENT_WAKE_WORD_DETECTED | [audio_service.cc:96-101](main/audio/audio_service.cc#L96-L101)   |
| 云端 JSON    | 网络任务   | Schedule() 到主任务改屏幕            | [application.cc:647-651](main/application.cc#L647-L651)           |
| 按键单击     | 按键回调   | MAIN_EVENT_TOGGLE_CHAT               | [application.cc:769](main/application.cc#L769)                    |
| 发送队列有货 | opus_codec | MAIN_EVENT_SEND_AUDIO                | [audio_service.cc:502-504](main/audio/audio_service.cc#L502-L504) |

**为什么要单线程决策？** 把所有状态修改收敛到主任务一处执行，就天然避免了多线程竞态，且行为可按事件顺序复现、好调试。

---

## 13. 背压、丢帧与延迟控制

音频是**实时**数据，"旧"数据没有价值，因此全线采用"丢旧保新"而不是"排队等"：

| 位置                    | 满了怎么办                 | 原因（代码注释）                               |
| ----------------------- | -------------------------- | ---------------------------------------------- |
| encode_queue（容量2）   | 丢最旧 PCM                 | 阻塞会卡住 AFE fetch，进而死锁整条输入链       |
| send_queue（容量40）    | 丢最旧 Opus                | 网络拥塞时，迟到的实时音频对服务器无用         |
| decode_queue（容量20）  | 非阻塞入队直接失败         | 不拖网络任务；`PlaySound` 提示音才用 wait=true |
| playback_queue（容量2） | codec 任务暂停解码（背压） | 等输出端消费，避免 PCM 堆积增延迟              |

- 丢帧日志做了**每秒最多一次**的限流，避免 UART 输出本身拖慢任务，见 [audio_service.cc:597-606](main/audio/audio_service.cc#L597-L606)。
- 发送失败时主任务**清空整个发送队列**：否则 codec 任务等队列空位、而主任务又不再发，形成死锁，见 [application.cc:241-249](main/application.cc#L239-L251)。
- `Stop()` / `ResetDecoder()` 通过递增 `playback_generation_` 代数，让"在途旧包/旧解码结果"作废，防止重置后还播旧声音，见 [audio_service.cc:173-193](main/audio/audio_service.cc#L173-L193)。

---

## 14. 省电：音频输入输出的自动开关

- `audio_power_timer_`：默认每 1 秒检查一次（有数据传输期间），见 [audio_service.cc:833-853](main/audio/audio_service.cc#L833-L853)。
- 输入/输出各自记录最后活动时间，超过 **15 秒**无活动：
  - 输入：置 `AS_EVENT_AUDIO_INPUT_STOP_REQUEST`，由输入任务自己关 I²S（ADC 连续模式要求"谁开谁关"，且必须同任务）。
  - 输出：直接 `EnableOutput(false)`（双工且输入在跑时保留 TX 时钟，防止某些板子 RX 停摆）。
- 首次读/写时才重新使能外设（见第 7/8 章），实现"用时开、闲时关"。

---

## 15. 关键参数速查表

| 参数                | 值                                      | 位置                                                                 |
| ------------------- | --------------------------------------- | -------------------------------------------------------------------- |
| 麦克风采样率 / 声道 | 16000 Hz / 单声道                       | [config.h:6](main/boards/bread-compact-wifi/config.h#L6)             |
| 喇叭采样率 / 声道   | 24000 Hz / 单声道                       | [config.h:7](main/boards/bread-compact-wifi/config.h#L7)             |
| I²S 物理位宽        | 32 bit（读出右移12、写入扩32）          | [no_audio_codec.cc:107](main/audio/codecs/no_audio_codec.cc#L107)    |
| Opus 帧长           | 60 ms                                   | [audio_service.h:40](main/audio/audio_service.h#L40)                 |
| 输入任务读取粒度    | 160 采样 = 10 ms                        | [audio_service.cc:311](main/audio/audio_service.cc#L311)             |
| 上行帧大小          | 960 采样 = 60 ms                        | [afe_audio_engine.cc:63](main/audio/engines/afe_audio_engine.cc#L63) |
| DMA                 | 6 描述符 × 240 帧                       | [audio_codec.h:15-16](main/audio/audio_codec.h#L15-L16)              |
| 读超时 / 队列等待   | 200 ms / 30 s（显示锁）                 | [no_audio_codec.cc:243](main/audio/codecs/no_audio_codec.cc#L243)    |
| 音频空闲断电超时    | 15 s                                    | [audio_service.h:48](main/audio/audio_service.h#L48)                 |
| OLED                | SSD1306 128×32，I²C 0x3C，SDA=41/SCL=42 | [config.h:37-53](main/boards/bread-compact-wifi/config.h#L37-L53)    |
| 状态时钟            | 1 Hz（esp_timer）                       | [application.cc:101](main/application.cc#L101)                       |

---

## 16. 动手调试切入点

按从易到难的顺序，建议自己打断点/加日志验证本文：

1. **看任务是否都活着**：`idf.py monitor` 开机日志中有 AFE 管线打印（`print_pipeline`）；`SystemInfo::PrintTaskList()`（主循环中被注释，打开即可，[application.cc:281](main/application.cc#L281)）。
2. **追上行**：在 [audio_service.cc:313](main/audio/audio_service.cc#L313) `ReadAudioData` 后打印 `data.size()`；在 [L488](main/audio/audio_service.cc#L488) 编码成功处打印压缩后字节数。
3. **追下行**：在 [audio_service.cc:351](main/audio/audio_service.cc#L351) `OutputData` 前后加日志，观察播放节奏。
4. **追状态→屏幕**：在 [application.cc:990](main/application.cc#L990) `HandleStateChangedEvent` 入口打印新状态名（状态机提供 `GetStateName()`）。
5. **看丢帧**：搜索日志 `dropping oldest frame`，出现即说明某处队列曾被顶满（网络或 CPU 跟不上）。
6. **开音频调试器**：`CONFIG_USE_AUDIO_DEBUGGER` 打开后，原始录音会经 `AudioDebugger` 送出（[audio_service.cc:231-237](main/audio/audio_service.cc#L231-L237)），可在服务器端回听。

---

## 附：整体调用链速记（背下来这一段就够了）

**上行：**

```
mic → I²S DMA → AudioInputTask(10ms) → AFE.Feed(32ms块) → AFE 内部(WakeNet/VAD/AEC)
    → fetch → 攒60ms → encode_queue(2) → OpusCodecTask 编码 → send_queue(40)
    → MAIN_EVENT_SEND_AUDIO → 主任务 → Protocol.SendAudio → 网络
```

**下行：**

```
网络 OnData → 二进制: decode_queue(20) → OpusCodecTask 解码(+重采样)
           → playback_queue(2) → AudioOutputTask → I²S DMA → 喇叭 → PLAYBACK_DRAINED
           JSON: Schedule → 主任务 → SetStatus/SetEmotion/SetChatMessage
```

**到屏幕：**

```
状态机 TransitionTo → STATE_CHANGED → 主任务 HandleStateChangedEvent
  → Display 方法(持 LVGL 锁) → 改 lv_label → LVGL 任务 → I²C → SSD1306
```
