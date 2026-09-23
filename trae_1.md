# 2026-09-22 工作记录：银发经济 MCP 工具

> 项目：XiaoZhi ESP32 语音助手固件（学习/笔记仓库 `f:\All_Code\ESP32\esp32-xiaozhi-chat`）
> 硬件：优信 ESP32-S3-CAM（ESP32-S3-N16R8，24PIN DVP 摄像头，1.54 寸 ST7789 240×240 屏）
> 板子实现：`bread-compact-wifi-s3cam`

---

## 一、目标

为设备添加银发经济场景的 MCP 工具，并让它们能在实际硬件上被调用。

**第一批**：

1. `self.medication_reminder` —— 服药提醒管理
2. `self.fall_detection` —— 跌倒检测（拍照 + 云端视觉分析）
3. `self.family_voice_board` —— 家属留言板

**第二批（同日扩展）**：

4. `self.medication_log` —— 用药打卡记录
5. `self.schedule_reminder` —— 通用日程提醒 + 到点自动播报
6. `self.emergency_contact` —— 紧急联系人管理
7. `self.find_item` —— 找东西（复用摄像头视觉链路）
8. `self.door_identification` —— 门口是谁（防诈骗）
9. `self.weather_query` —— 天气查询（HTTP，无需额外硬件）
10. **SOS 硬件按键** —— GPIO3 长按 3 秒触发紧急告警
11. **三个后台定时任务** —— 日程到点播报 / 久坐提醒 / 夜间离床检测

---

## 二、产出概览

| 文件                                                                                                                                 | 改动量   | 内容                                                                 |
| ------------------------------------------------------------------------------------------------------------------------------------ | -------- | -------------------------------------------------------------------- |
| [main/Kconfig.projbuild](main/Kconfig.projbuild)                                                                                     | +95 / -3 | `Silver Economy Demo` 菜单，共 9 个配置项（3 个第一批 + 6 个第二批） |
| [main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc](main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc) | +1089    | 9 个 MCP 工具 + SOS 按键 + 4 个后台定时任务                          |
| [main/boards/bread-compact-wifi-s3cam/config.h](main/boards/bread-compact-wifi-s3cam/config.h)                                       | +5       | `SOS_BUTTON_GPIO = GPIO_NUM_22` 及接线注释                           |
| [main/mcp_server.cc](main/mcp_server.cc)                                                                                             | +205     | 带详细注释的学习版 3 个工具（Kconfig 开关控制，默认关闭）            |
| `main/boards/waveshare/esp32-s3-cam/esp32-s3-cam-xxxx.cc`                                                                            | 已还原   | 误改后撤回，`git diff` 已验证回到原始状态                            |

合计：`1394 insertions(+), 3 deletions(-)`（`git diff --stat`）。

设计原则：**正式版放板子，学习版放 mcp_server.cc**。按 `AGENTS.md` 与 `mcp_server.cc:36-37` 的规则，自定义工具必须在板子的 `InitializeTools()` 中注册，核心模块不应承载板级功能。

---

## 三、改动明细（第一批）

### 3.1 Kconfig 选项

在 `main/Kconfig.projbuild` 末尾（`Xiaozhi Assistant` 主菜单内）新增：

```kconfig
menu "Silver Economy Demo"
    comment "学习示例：银发经济 MCP 工具（学习用，正式版在板子 InitializeTools 中）"

    config ENABLE_SILVER_ECONOMY_DEMO
        bool "Enable Silver Economy MCP Demo Tools (Learning Only)"
        default n

    config ENABLE_BOARD_FALL_DETECTION
        bool "Enable Periodic Fall Detection (Board, Always-On)"
        default n
        depends on BOARD_TYPE_BREAD_COMPACT_WIFI_CAM

    config BOARD_FALL_DETECTION_PERIOD_MS
        int "Fall Detection Periodic Check Interval (ms)"
        default 60000
        range 30000 3600000
        depends on ENABLE_BOARD_FALL_DETECTION
endmenu
```

| 选项                                    | 默认  | 作用                                  |
| --------------------------------------- | ----- | ------------------------------------- |
| `CONFIG_ENABLE_SILVER_ECONOMY_DEMO`     | n     | 启用 `mcp_server.cc` 里的学习版工具   |
| `CONFIG_ENABLE_BOARD_FALL_DETECTION`    | n     | 启用板子代码的 esp_timer 周期跌倒检测 |
| `CONFIG_BOARD_FALL_DETECTION_PERIOD_MS` | 60000 | 周期检测间隔（30 秒 ~ 1 小时）        |

两个开关**相互独立**：同时启用会出现 `Tool xxx already added` 警告，框架跳过重复项，属预期行为。

### 3.2 板子代码：三个 MCP 工具

在 `CompactWifiBoardS3Cam` 类中新增 `InitializeTools()`，构造函数中调用。

**工具 1：`self.medication_reminder`**

- 参数：`action`（add / remove / list）、`medicine`、`time`
- 存储：NVS namespace `medication`，单 key `reminders` 存整个 JSON 数组字符串
- 原因：`Settings` 类没有遍历 namespace 的 API，只能用一个 summary key 整体读写

**工具 2：`self.fall_detection`**

- 无参数，AI 按需调用
- 流程：`TaskPriorityReset(1)` 降优先级 → `camera->Capture()` → `camera->Explain(prompt)` → 返回 JSON `{fell, confidence, description}`

**工具 3：`self.family_voice_board`**（文字版）

- 参数：`action`（add / list / play_latest）、`sender`、`message`
- 存储：NVS namespace `voice_board`，单 key `messages`
- `play_latest` 返回文本，由 AI 用 TTS 朗读
- **音频版未做**（家属端推送 Opus 音频 → 存 SPIFFS → 按键播放），需改协议层 + 文件系统 + audio_service，改动面过大，推迟

### 3.3 周期跌倒检测（`#ifdef CONFIG_ENABLE_BOARD_FALL_DETECTION`）

线程模型分四层，核心是**不在回调里做重活**：

```
esp_timer (timer service task)
    └─ FallDetectionTimerCb()          只做调度，不阻塞
         └─ TryStartFallDetection()    std::atomic<bool> 重入保护
              └─ xTaskCreate("fall_det", 8192, prio=2)
                   └─ RunFallDetection()
                        ├─ TaskPriorityReset(1)   让出 CPU 给音视频
                        ├─ camera_->Capture()            640x480 RGB565
                        ├─ camera_->Explain(prompt)      → 返回服务器信封 JSON
                        ├─ 解析两层 JSON，取 fell / description
                        └─ Application::Schedule(...)  回到主任务
                             ├─ app.Alert("跌倒警报", description, ...)
                             └─ app.SendMcpMessage({"type":"fall_alert"})
```

关键点：

- `std::atomic<bool>::compare_exchange_strong` 防止上一轮未结束又起一轮
- UI / 音频 / MCP 消息一律通过 `Application::Schedule()` 回主任务，符合 `AGENTS.md` 的"回调可能运行在主任务之外"约束
- 任务栈 8KB，优先级 2
- **`Explain()` 返回的是服务器信封 JSON，真正的模型回答嵌在 `text` 字段里，必须解析两层**（详见 3.4）

### 3.4 硬件测试中发现的 bug：嵌套 JSON 导致误判

这是今天最有价值的一处发现——**代码看起来在正常工作，实际判定完全没生效**。

#### 现象

硬件实跑日志（`CONFIG_BOARD_FALL_DETECTION_PERIOD_MS=60000`）：

```
I (94658) Esp32Camera: Explain image size=640x480, compressed size=49411, remain stack size=6284, question=观察画面中是否有人呈跌倒或瘫坐姿态。返回 JSON：{"fell": bool, "confidence": 0.0-1.0, "description": 简短描述}
{"success":true,"filename":"x12cxrnb0pvahnfssvt0cgl3k.jpg","text":"{\"fell\": true, \"confidence\": 0.6, \"description\": \"画面模糊且带有扫描线干扰，一人头部倾斜、身体呈躺卧姿态，疑似跌倒或瘫坐。\"}"}
I (94688) CompactWifiBoardS3Cam: fall_detection: no fall detected
```

**模型明确返回 `fell: true`，设备却打印 `no fall detected`，既不告警也不上报。**

#### 根因

返回值是**两层嵌套 JSON**：

| 层级               | 内容                                                    | 是否含 `fell` |
| ------------------ | ------------------------------------------------------- | ------------- |
| 外层（服务器信封） | `{"success":…, "filename":…, "text":"<模型回答>"}`      | ❌             |
| 内层（模型回答）   | `{"fell": true, "confidence": 0.6, "description": "…"}` | ✅             |

内层是**字符串**形式，藏在 `text` 字段里。原代码只解析了一层：

```cpp
cJSON* root = cJSON_Parse(result->c_str());          // 解析成功（外层是合法 JSON）
cJSON* fell_item = cJSON_GetObjectItem(root, "fell"); // 外层没这个 key → NULL
```

`fell_item == NULL` → `fell` 保持 `false` → 直接 `return`，`Alert()` 和 `SendMcpMessage()` 两行**根本没执行**。

**教训**：`cJSON_Parse` 成功 ≠ 数据结构正确。外层解析成功掩盖了字段缺失，失败被静默吞掉，只留下一句看起来正常的 `no fall detected`。

#### 修复

先取 `text` 拿到字符串，再解析一次；并用"第一个 `{` 到最后一个 `}`"兜住模型可能输出的 markdown 围栏或解释性文字：

```cpp
// 1. 解析外层信封，取出 text 字段
std::string model_text;
cJSON* envelope = cJSON_Parse(result->c_str());
if (envelope != nullptr) {
    cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
    model_text = (text_item && cJSON_IsString(text_item))
                     ? text_item->valuestring : *result;   // 无信封则退回原文
    cJSON_Delete(envelope);
} else {
    model_text = *result;
}

// 2. 截取 JSON 对象子串，解析内层
size_t begin = model_text.find('{');
size_t end   = model_text.rfind('}');
if (begin != std::string::npos && end != std::string::npos && end > begin) {
    cJSON* root = cJSON_Parse(model_text.substr(begin, end - begin + 1).c_str());
    if (root != nullptr) {
        // 取 fell → 决定是否告警；取 description → 带进屏幕/上报内容
    } else {
        ESP_LOGW(TAG, "fall_detection: failed to parse model reply: %s", model_text.c_str());
    }
}
```

同时增强了两处：

- `description`（模型对画面的描述）会显示到屏幕聊天区与日志，不再是一句固定的"请立即确认"
- 解析失败时打印模型原文，便于下次定位结构变化

### 3.5 新增头文件

板子文件补充了：`settings.h`、`assets/lang_config.h`、`<esp_timer.h>`、`<atomic>`、`<cstdio>`、`<ctime>`、`<cstring>`。

**不需要改 CMakeLists**：板子源文件通过 `boards/${BOARD_DIR}/*.cc` 通配符收集；`esp_timer`、`spi_flash`、`mbedtls` 等已在 `PRIV_REQUIRES` 中。

---

## 三B、改动明细（第二批）

### 3B.1 新增六个 MCP 工具（全部注册在板子 `InitializeTools()`）

| #   | 工具名                     | 参数                                                   | 存储 / 依赖                                                                              | 说明                                                                            |
| --- | -------------------------- | ------------------------------------------------------ | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 4   | `self.medication_log`      | `action`(checkin/status/list_today)、`medicine`        | NVS `medication`，key `logs`，按日期分组的 JSON 对象 `{ "2026-09-22": ["降压药", ...] }` | 用药打卡闭环；checkin 有去重；`time(nullptr) < 1700000000` 时日期记为 `unknown` |
| 5   | `self.schedule_reminder`   | `action`(add/remove/list)、`time`、`content`           | NVS `schedule`，key `reminders`                                                          | 通用日程 CRUD；到点播报由后台任务负责（见 3B.3），两者解耦                      |
| 6   | `self.emergency_contact`   | `action`(add/remove/list)、`name`、`relation`、`phone` | NVS `contacts`，key `list`                                                               | SOS 告警时会读取前两条联系人显示到屏幕                                          |
| 7   | `self.find_item`           | `item`                                                 | 复用摄像头 + `Explain()`                                                                 | 拍照后让视觉模型描述「物品在哪」，取信封 `text` 返回给 AI                       |
| 8   | `self.door_identification` | 无参数                                                 | 复用摄像头 + `Explain()`                                                                 | 描述门口人数/穿着/手持物，防诈骗                                                |
| 9   | `self.weather_query`       | `city`（可选）                                         | HTTP，wttr.in                                                                            | `GET https://wttr.in/{city}?format=3&lang=zh`，纯文本返回，无需 API key         |

视觉类工具（7、8）与跌倒检测共用同一套解析逻辑：**先取信封 `text`，再返回文本**（这次返回的是自然语言，不需要二次 JSON 解析）。

### 3B.2 SOS 硬件按键

**引脚选择（GPIO3）**：逐一对照 `config.h` 排除全部已占用引脚后确定。**第一版选了 GPIO22 是错的——ESP32-S3 芯片上根本不存在 GPIO22–25**（编号从 21 直接跳到 26），编译能过但运行时按键必失效。修正过程见下表。

| 引脚范围               | 状态                                                                                                              |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------- |
| 0                      | Boot 按键（占用）                                                                                                 |
| 1, 2, 42               | 麦克风 I2S（占用）                                                                                                |
| 4–13, 15–18            | 摄像头 DVP/SCCB（占用）                                                                                           |
| 14                     | 灯控 LAMP_GPIO（占用）                                                                                            |
| 19, 20, 21, 38, 45, 47 | 显示屏 SPI（占用）                                                                                                |
| 22–25                  | **芯片上不存在，禁用**（初版错误选择的引脚就在这里）                                                              |
| 26–32                  | 内部 16MB SPI Flash，禁用                                                                                         |
| 33, 34                 | N16R8 可用（**备选**；Octal PSRAM 只占 35–37）                                                                    |
| 35–37                  | **被 Octal PSRAM（R8）占用**，N16R8 禁用                                                                          |
| 39–42                  | 喇叭 I2S（占用）                                                                                                  |
| 43, 44                 | UART0 调试串口，占用后无法看日志                                                                                  |
| 46                     | strapping，仅输入受限                                                                                             |
| 48                     | 板载 LED（占用）                                                                                                  |
| **3**                  | **唯一空闲的常规 GPIO → SOS 采用**（strapping：JTAG 源选择，上电内部上拉为高，不影响启动；boot 由 GPIO0/46 决定） |

**接线**：按键一端接 GPIO3，另一端接 GND。代码里 `active_high=false` + `disable_pull=false`（内部上拉），**不需要外部上拉电阻**，按下为低电平。硬件不便时可改 `config.h` 的 `SOS_BUTTON_GPIO`（备选 33/34）。

**触发逻辑**：构造函数 `sos_button_(SOS_BUTTON_GPIO, false, 3000)` → 长按 3 秒触发 `BUTTON_LONG_PRESS_START`。回调里：

1. 读 NVS `contacts/list`，取前两个联系人拼进消息
2. `Application::Schedule()` 回主任务 → `Alert("紧急求助", ...)` + 告警音 + `SendMcpMessage({"type":"sos_alert"})`

**比语音呼救可靠的原因**：老人情急时可能喊不出唤醒词，物理按键不依赖网络对话链路的唤醒环节。

**教训**：ESP32-S3 选脚前先把"芯片引脚不存在 / Flash 占用 / PSRAM 占用 / 串口占用"四类黑名单排除，再对照板子 config 逐脚核对；编译通过不代表引脚存在（`GPIO_NUM_22` 只是数字，驱动运行时才报错）。

### 3B.3 三个后台定时任务（Kconfig 开关，默认全关）

| Kconfig 开关 + 参数                                                          | 默认值               | 任务行为                                                         |
| ---------------------------------------------------------------------------- | -------------------- | ---------------------------------------------------------------- |
| `CONFIG_ENABLE_SCHEDULE_REMINDER`                                            | n                    | 每 60 秒读 NVS `schedule/reminders`，`HH:MM` 匹配到点弹屏+提示音 |
| `CONFIG_ENABLE_SEDENTARY_REMINDER` + `_PERIOD_MS`                            | n / 30 分钟          | 周期拍照问视觉模型是否久坐，是则提醒"起来活动活动"               |
| `CONFIG_ENABLE_BED_EXIT_DETECTION` + `_CHECK_PERIOD_MS` + `_EMPTY_TIMEOUT_S` | n / 2 分钟 / 10 分钟 | 周期拍照判断床上是否有人；连续无人超阈值 → 离床告警+推送家属     |

线程模型与跌倒检测完全一致（esp_timer 回调只做调度 → `std::atomic` 重入保护 → 独立任务 8192 栈 prio=2 → `TaskPriorityReset(1)` → `Application::Schedule()` 回主任务）。

细节：

- **日程播报**用 `schedule_last_minute_` 记录上一轮处理过的分钟，避免同一分钟重复触发；系统时间未同步（`now < 1700000000`）直接跳过
- **久坐判定**：单帧问"是否长时间坐着或躺着没有明显活动"，默认 `sedentary=false`（模型不回答就不打扰）
- **离床判定**：默认 `in_bed=true`（宁可漏报不误报）；`bed_empty_count_` 连续累计，达到 `EMPTY_TIMEOUT_S / CHECK_PERIOD_MS` 次数阈值才告警，告警后清零可再次触发；人回到床上即清零

### 3B.4 离床检测的"摄像头区域识别"是什么意思

不是像素级 ROI 裁剪，而是**基于云端视觉模型的场景理解**：

1. **物理区域**：摄像头固定对准床铺，画面里的"床"就是感兴趣区域
2. **语义区域**：prompt 限定模型只回答"床上是否有人躺着"，忽略画面其余部分
3. **时间区域**：连续 N 次无人才告警，容忍正常起夜

不裁像素的原因：ESP32 上操作 PSRAM frame buffer 裁剪代码量大，而视觉模型本身能理解"床上有没有人"。若以后要升级真 ROI，在 `Capture()` 与 `Explain()` 之间加裁剪即可。

### 3B.5 新增依赖说明

- `Button` 类（`boards/common/button.h`）第一批已在用（boot_button_），SOS 只是第二个实例，无新依赖
- `weather_query` 用 `Board::GetInstance().GetNetwork()->CreateHttp()`，与 `esp32_camera.cc` 同一套抽象，无需新组件
- `find_item`/`door_identification` 复用 `TaskPriorityReset`（application.h）与 `Explain()`
- 仍不需要改 CMakeLists

---

## 四、板子选型排查（第一批的重点弯路）

### 现象

`idf.py menuconfig` 里看不到 `Enable Periodic Fall Detection`。

### 误判过程

先查 `sdkconfig`，发现选的是 `CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_CAM=y`，而 Kconfig 里我写的是 `depends on BOARD_TYPE_WAVESHARE_ESP32_S3_CAM_XXXX`，于是判断"板子选错了"，并试图改用 `waveshare/esp32-s3-cam`。

中间还发现 `waveshare/esp32-s3-cam` 只有 2 / 2.8 / 3.5 / 1.83 四个 variant，**没有 1.54**，一度打算新增 variant。

### 真相

**用户的板子就是 `bread-compact-wifi-s3cam`**（对应 Kconfig `BOARD_TYPE_BREAD_COMPACT_WIFI_CAM`，菜单名"面包板新版接线 WiFi + LCD + Camera"），板子选型从头到尾都是对的。

证据：

| 项目                                | 现象                                       |
| ----------------------------------- | ------------------------------------------ |
| menuconfig `LCD Type`               | `ST7789 240*240` —— 与 1.54 寸屏吻合       |
| `bread-compact-wifi-s3cam/config.h` | 支持 `CONFIG_LCD_ST7789_240X240` → 240×240 |

### 根因

**Kconfig 的 `depends on` 写错了板子符号**，依赖不满足 → 选项被隐藏。

修正：

```diff
  config ENABLE_BOARD_FALL_DETECTION
      bool "Enable Periodic Fall Detection (Board, Always-On)"
      default n
-     depends on BOARD_TYPE_WAVESHARE_ESP32_S3_CAM_XXXX
+     depends on BOARD_TYPE_BREAD_COMPACT_WIFI_CAM
```

### 结论 / 教训

- 排查"menuconfig 选项不显示"，**第一步就是核对 `depends on` 的板子符号和实际选中的板子是否一致**
- 跨板子的代码改动前，先确认用户真正在用的板子，不要凭硬件型号推测
- `waveshare/esp32-s3-cam` 的改动已全部撤回

---

## 五、测试方法

### 第 0 步：编译烧录 + 确认工具注册

```powershell
cd f:\All_Code\ESP32\esp32-xiaozhi-chat
idf.py build
idf.py -p COM3 flash monitor
```

启动日志（`mcp_server.cc:519` 的 `Add tool` 日志）应出现 **9 条**：

```
I (xxxx) MCP: Add tool: self.medication_reminder
I (xxxx) MCP: Add tool: self.fall_detection
I (xxxx) MCP: Add tool: self.family_voice_board
I (xxxx) MCP: Add tool: self.medication_log
I (xxxx) MCP: Add tool: self.schedule_reminder
I (xxxx) MCP: Add tool: self.emergency_contact
I (xxxx) MCP: Add tool: self.find_item
I (xxxx) MCP: Add tool: self.door_identification
I (xxxx) MCP: Add tool: self.weather_query
```

### 第 1 步：语音对话触发（9 个工具全覆盖）

设备端 MCP 工具由云端 AI 决定调用，路径是「唤醒 → 自然语言请求 → AI 调工具 → 返回 → AI 朗读」。

**第一批工具：**

| 工具                | 话术                            | 预期现象                |
| ------------------- | ------------------------------- | ----------------------- |
| medication_reminder | "帮我加个提醒，早上8点吃降压药" | AI 回"已添加"           |
|                     | "我都有哪些吃药提醒？"          | AI 念出清单             |
|                     | "把8点的提醒删掉"               | AI 回"已删除"           |
| fall_detection      | "看看我现在有没有摔倒"          | 听到拍照声，AI 描述画面 |
| family_voice_board  | "给家人留个言：今晚我早点休息"  | AI 回"留言已保存"       |
|                     | "播放最新的家人留言"            | AI 念出留言内容         |

**第二批工具（新增）：**

| 工具                | 话术                                          | 预期现象                                                                                               |
| ------------------- | --------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| medication_log      | "我吃过降压药了"                              | AI 调 `checkin`，回"已记录"（再说一遍会说"已经记过了"）                                                |
|                     | "今天药吃了没？"                              | AI 调 `status`，回答吃过/没吃过                                                                        |
| schedule_reminder   | "下午3点提醒我去医院"                         | AI 调 `add`；**到 15:00 屏幕弹"日程提醒"+提示音**（需开 `CONFIG_ENABLE_SCHEDULE_REMINDER`，见第 2 步） |
| emergency_contact   | "存一下我儿子的电话，13800138000，紧急联系人" | AI 调 `add`                                                                                            |
|                     | "我的紧急联系人都有谁"                        | AI 念出联系人列表                                                                                      |
| find_item           | "帮我找找眼镜"                                | 听到拍照声，AI 描述眼镜在画面里的位置                                                                  |
| door_identification | "看看门口是谁"                                | 听到拍照声，AI 描述门外的人（人数/穿着/拿什么）                                                        |
| weather_query       | "今天天气怎么样"                              | AI 播报 `城市: 天气 温度`；也可指定"上海天气"                                                          |

**闭环组合测试**：先 `emergency_contact` 存联系人 → 再测 SOS 按键（第 3 步），验证告警消息里带出联系人。

每个工具都要覆盖 add → list → 删除/查询，才算完整。

### 第 2 步：后台定时任务（menuconfig 开启后测）

```powershell
idf.py menuconfig   # Xiaozhi Assistant → Silver Economy Demo
idf.py build flash monitor
```

| 开关                        | 启动日志                                               | 运行时日志 / 现象                                                         |
| --------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------- |
| `ENABLE_SCHEDULE_REMINDER`  | `schedule_reminder: timer started, checking every 60s` | 到点 `firing 'xxx' at HH:MM`，屏幕弹"日程提醒"+提示音                     |
| `ENABLE_SEDENTARY_REMINDER` | `sedentary_reminder: timer started, period=1800000 ms` | 检测时 `Esp32Camera: Explain ...`；判定久坐时弹"温馨提醒"                 |
| `ENABLE_BED_EXIT_DETECTION` | `bed_exit_detection: timer started, period=120000 ms`  | `bed empty, count=N`；超阈值 `bed empty too long, alerting!` + "离床告警" |

**离床快速验证技巧**：把 `BED_EXIT_EMPTY_TIMEOUT_S` 临时调成 60（最小值）、`BED_EXIT_CHECK_PERIOD_MS` 调成 30000，镜头对着空椅子，1 分钟内就能看到告警，不用干等 10 分钟。测完改回。

### 第 3 步：SOS 按键（需要接线）

1. 按键一端接 **GPIO3**，另一端接 **GND**（内部上拉，无需电阻）
2. 先用语音存至少一个紧急联系人（见第 1 步 emergency_contact）
3. **长按 3 秒**，预期：
   - 日志：`W (xxxx) CompactWifiBoardS3Cam: SOS button long pressed!`
   - 屏幕：弹出"紧急求助"对话框，含联系人信息
   - 播放告警音（OGG_EXCLAMATION）
   - 家属端收到 `{"type":"sos_alert","source":"device"}`

误按保护：短按/长按不足 3 秒都不会触发。

### 第 4 步：看日志辅助验证

工具注册（`mcp_server.cc`，TAG = `MCP`）：

```
I (xxx) MCP: Add tool: self.medication_log
E (xxx) MCP: tools/call: Unknown tool: ...   ← 工具名写错 / 没注册
E (xxx) MCP: tools/call: <错误信息>           ← 回调返回了 unexpected
```

后台任务（TAG = `CompactWifiBoardS3Cam`，相机日志 TAG = `Esp32Camera`）：

```
I (xxx) CompactWifiBoardS3Cam: fall_detection: timer started, period=60000 ms
I (xxx) CompactWifiBoardS3Cam: schedule_reminder: timer started, checking every 60s
I (xxx) Esp32Camera: Explain image size=640x480, compressed size=..., ...
W (xxx) CompactWifiBoardS3Cam: fall_detection: FALL DETECTED! <模型描述>
W (xxx) CompactWifiBoardS3Cam: bed_exit: bed empty too long, alerting!
```

注意 `ESP_LOGD` 级别的 `previous round still running, skip` 在默认 `LOG_DEFAULT_LEVEL=INFO` 下**不会打印**，需要把日志级别调到 Debug。

### 第 5 步：静态确认固件里有没有编进去（跳过硬件）

```powershell
$s = [System.IO.File]::ReadAllText("build\xiaozhi.elf", [System.Text.Encoding]::ASCII)
foreach ($k in @("fall_detection: timer started",
                 "schedule_reminder: timer started",
                 "sedentary_reminder: timer started",
                 "bed_exit_detection: timer started",
                 "SOS button long pressed",
                 "self.medication_reminder",
                 "self.medication_log",
                 "self.schedule_reminder",
                 "self.emergency_contact",
                 "self.find_item",
                 "self.door_identification",
                 "self.weather_query")) {
    if ($s.Contains($k)) { "FOUND : $k" } else { "MISS  : $k" }
}
```

注意：查 `Add tool: self.xxx` 会 MISS——因为源码里是格式串 `Add tool: %s%s`，工具名单独存放，要分开查。后台任务的字符串只开了对应 menuconfig 开关才会 FOUND。

### 第 6 步：脱离硬件理解协议（可选）

仓库里的 [my_server_python.py](my_server_python.py) 是本地 Python MCP Server，与硬件无关，用来先摸清 `tools/list` / `tools/call`：

```powershell
pip install "mcp[cli]" fastmcp
mcp dev my_server_python.py
```

会打开网页调试器，左侧列工具、右侧点按钮调用。

---

## 六、排错对照表

| 现象                                                                   | 原因                                                                                                                                               |
| ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| menuconfig 里看不到 `Enable Periodic Fall Detection`                   | Kconfig `depends on` 的板子符号与实际选中的板子不一致（见第四节）                                                                                  |
| 启动日志没有 `Add tool`                                                | 板子选错 / `InitializeTools()` 没被调用                                                                                                            |
| AI 说"我没有这个能力"                                                  | `tools/list` 没送到服务器，或工具被分页截断（`max_payload_size = 8000`，`mcp_server.cc:688`）。**新增 6 个工具后 tools/list 变长，更容易触发分页** |
| `Unknown tool: self.xxx`                                               | 工具名拼错                                                                                                                                         |
| **模型返回 `fell: true` 但日志是 `no fall detected`**                  | **只解析了一层 JSON；真正的字段在信封的 `text` 字段里（见 3.4）**                                                                                  |
| **开机前几轮 `explain failed: Image explain URL or token is not set`** | **正常：定时器在板子构造函数里就启动了，那时还没联网/激活**                                                                                        |
| 日志看不到 `previous round still running, skip`                        | 该日志是 `ESP_LOGD`，默认 `LOG_DEFAULT_LEVEL=INFO` 不打印                                                                                          |
| 添加的提醒重启后还在                                                   | 正常，NVS 持久化；`idf.py erase-flash` 可清空                                                                                                      |
| AI 不主动调工具                                                        | 工具描述不够清楚，或用户说法太模糊                                                                                                                 |
| SOS 长按没反应                                                         | 接线错误（应接 GPIO3↔GND）/ 长按不足 3 秒 / `config.h` 引脚被改过                                                                                  |
| schedule 到点没播报                                                    | `CONFIG_ENABLE_SCHEDULE_REMINDER` 没开 / 系统时间没同步（看日志时间戳是否为真实日期）                                                              |
| weather_query 返回失败                                                 | 未联网 / wttr.in 访问慢或被限流，可重试或指定城市                                                                                                  |
| 后台任务日志一个都没有                                                 | 对应 Kconfig 开关没在 menuconfig 打开（所有后台任务默认全关）                                                                                      |

---

## 七、验证状态与待办

### 静态确认（已完成）

- [x] `git diff --stat` 确认改动范围：`Kconfig.projbuild` +95/-3、`compact_wifi_board_s3cam.cc` +1089、`config.h` +5、`mcp_server.cc` +205
- [x] IDE 语言服务对板子文件无诊断错误
- [x] `waveshare/esp32-s3-cam` 已还原干净（`git diff` 无输出）
- [x] `sdkconfig` 确认：`CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_CAM=y`、`CONFIG_ENABLE_BOARD_FALL_DETECTION=y`、`CONFIG_BOARD_FALL_DETECTION_PERIOD_MS=60000`
- [x] `build/xiaozhi.elf` 字符串确认周期检测与三个工具都已编入固件
- [x] **发现并修正 SOS 引脚错误**：初版选的 GPIO22 在 ESP32-S3 上不存在（22–25 编号被跳过）；全量核对 `config.h` + 芯片引脚黑名单后改为 GPIO3（唯一空闲常规 GPIO）

### 硬件上已观察到的（第一批真实日志）

- [x] 编译通过并成功烧录运行
- [x] 周期检测确实在跑：定时器已创建并周期触发
- [x] 拍照 → 编码 → HTTP 上传 → 云端视觉分析链路完全打通：
      `Explain image size=640x480, compressed size=49411, remain stack size=6284`
- [x] **发现并修复 bug**：模型返回 `fell: true` 却被判定为 `no fall detected`（嵌套 JSON 只解析了一层，见 3.4）

### 待验证（第二批全部待烧录验证）

- [ ] **第一批遗留**：修复嵌套 JSON bug 后重新编译烧录，确认日志出现 `FALL DETECTED!` 且屏幕弹出"跌倒警报"、播放告警音
- [ ] 9 个工具的语音触发（按第五节话术表）
- [ ] **SOS 按键**：GPIO22 接线后长按 3 秒，验证告警 + 联系人显示 + 推送
- [ ] **schedule 到点播报**：开 `CONFIG_ENABLE_SCHEDULE_REMINDER`，加一条 2 分钟后的提醒等触发
- [ ] **sedentary / bed_exit 后台任务**：按第五节第 2 步的快速验证技巧测
- [ ] weather_query 在真机上确认 wttr.in 可达、HTTPS 握手正常

### 待做

- [ ] **跌倒告警去重**：人持续躺在地上时每 60 秒重复告警一次，应加"已告警则静默"状态（当前最影响体验的问题）
- [ ] `family_voice_board` 音频版（家属推 Opus → SPIFFS → 按键播放）
- [ ] 老人按键直接播最新留言（不经过 AI 对话）
- [ ] `medication_reminder` 的本地定时播报（目前只存提醒，不会主动响；`schedule_reminder` 已有播报链路，可复用）
- [ ] `medication_log` 历史日志清理策略（目前按日期累积，NVS 单 value 有 4KB 上限，长期要滚动清理旧日期）
- [ ] SOS 触发后的取消机制（老人误按后如何撤销告警）
- [ ] 跌倒后主动语音询问（本次决定暂不做；可选路径 A `WakeWordInvoke` 触发 AI 对话 / 路径 B 本地预置 OGG）

### 固有限制（非 bug，设计取舍）

- **判定在云端**：断网即完全失效，无本地模型兜底（fall/sedentary/bed_exit 三个视觉类任务均如此）
- **单帧判定**：无多帧时序确认，弯腰捡东西可能误报、缓慢跌倒可能漏报
- **持续占用成本**：每 60 秒一次拍照 + 云端视觉调用；sedentary/bed_exit 再开会更加频繁
- **weather_query 依赖第三方**：wttr.in 免费服务，无 SLA，可能限流

---

## 八、涉及文件清单

| 文件                                                                                                                                 | 状态                     |
| ------------------------------------------------------------------------------------------------------------------------------------ | ------------------------ |
| [main/Kconfig.projbuild](main/Kconfig.projbuild)                                                                                     | 已改（两批）             |
| [main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc](main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc) | 已改（两批）             |
| [main/boards/bread-compact-wifi-s3cam/config.h](main/boards/bread-compact-wifi-s3cam/config.h)                                       | 已改（第二批：SOS 引脚） |
| [main/mcp_server.cc](main/mcp_server.cc)                                                                                             | 已改（学习版）           |
| [main/settings.h](main/settings.h)                                                                                                   | 未改（参考）             |
| [main/application.h](main/application.h)                                                                                             | 未改（参考）             |
| [main/boards/common/button.h](main/boards/common/button.h)                                                                           | 未改（参考）             |
| [main/boards/common/esp32_camera.h](main/boards/common/esp32_camera.h)                                                               | 未改（参考）             |
| `main/boards/waveshare/esp32-s3-cam/esp32-s3-cam-xxxx.cc`                                                                            | 已还原                   |
| [my_server_python.py](my_server_python.py)                                                                                           | 本地 MCP 学习模板        |
| [trae_1.md](trae_1.md)                                                                                                               | 本文件                   |
