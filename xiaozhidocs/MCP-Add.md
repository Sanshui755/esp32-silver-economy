# 银发经济 MCP 工具清单

本项目在 XiaoZhi 固件基础上，为 ESP32-S3-CAM（bread-compact-wifi-s3cam）添加了一套完整的银发经济智能语音助手工具链。以下整理所有 MCP 工具、后台任务和硬件功能。

## 一、官方内置工具（mcp_server.cc，所有板子通用）

| #   | 工具名                          | 功能                            | 启用条件   |
| --- | ------------------------------- | ------------------------------- | ---------- |
| 1   | `self.get_device_status`        | 查询设备状态（MAC、IP、版本等） | 默认启用   |
| 2   | `self.audio_speaker.set_volume` | 设置音量                        | 默认启用   |
| 3   | `self.screen.set_brightness`    | 设置屏幕亮度                    | 有显示屏时 |
| 4   | `self.screen.set_theme`         | 切换屏幕主题                    | 有显示屏时 |
| 5   | `self.camera.take_photo`        | 拍照                            | 有摄像头时 |

## 二、银发经济工具（板级代码）

注册位置：`main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc` 的 `InitializeTools()` 中。所有工具按 AGENTS.md 规范在板级注册，不放在 mcp_server.cc。

| #   | 工具名                     | 功能                  | 参数                                             | 存储                     | 说明                                                                                                |
| --- | -------------------------- | --------------------- | ------------------------------------------------ | ------------------------ | --------------------------------------------------------------------------------------------------- |
| 6   | `self.medication_reminder` | 服药提醒增删查        | add / remove / list                              | NVS `medication`         |                                                                                                     |
| 7   | `self.fall_detection`      | 拍照+视觉分析是否跌倒 | 无                                               | 无状态                   | 返回 `{fell, confidence, description}`                                                              |
| 8   | `self.family_voice_board`  | 家属留言板（文字）    | add / list                                       | NVS `voice_board`        | 留言默认保留 3 天自动过期（`VOICE_BOARD_MSG_TTL_DAYS` 可调 1-30 天）                                  |
| 9   | `self.medication_log`      | 用药打卡+依从性记录   | checkin / status / list_today / report / history | NVS `medication`         | 打卡记 `{medicine,time}`；report 返医嘱执行情况(taken/late/missed/pending+delay)；history 查近 7 天 |
| 10  | `self.schedule_reminder`   | 通用日程提醒          | add / remove / list                              | NVS `reminders`          | 到点自动播报（需开 Kconfig）                                                                        |
| 11  | `self.emergency_contact`   | 紧急联系人管理        | add / remove / list                              | NVS `emergency_contacts` | SOS 触发时自动显示                                                                                  |
| 12  | `self.find_item`           | 找东西（拍照+视觉）   | item_name                                        | 无状态                   | 受 CameraLockGuard 摄像头互斥锁保护                                                                 |
| 13  | `self.door_identification` | 门口来人识别          | 无                                               | 无状态                   | 防诈骗，同上互斥锁                                                                                  |
| 14  | `self.weather_query`       | 天气查询              | city（可选，默认自动定位）                       | 无状态                   | wttr.in API，IP 自动定位                                                                            |

### 语音触发示例

| 工具                  | 对设备说                          |
| --------------------- | --------------------------------- |
| `medication_log`      | "我吃过降压药了" / "今天药吃了没" |
| `schedule_reminder`   | "下午3点提醒我去医院"             |
| `emergency_contact`   | "存一下我儿子的电话，13800138000" |
| `find_item`           | "帮我找找眼镜"                    |
| `door_identification` | "看看门口是谁"                    |
| `weather_query`       | "今天天气怎么样"                  |

## 四、后台定时任务（板级 esp_timer）

受 Kconfig 开关控制，位于 `main/Kconfig.projbuild` 的 "Silver Economy Demo" 菜单下。均依赖 `BOARD_TYPE_BREAD_COMPACT_WIFI_CAM`。

| #   | Kconfig 开关                      | 功能         | 默认间隔/规则                       | 依赖              |
| --- | --------------------------------- | ------------ | ----------------------------------- | ----------------- |
| A   | `ENABLE_BOARD_FALL_DETECTION`     | 周期跌倒检测 | 60 秒                               | 摄像头 + 云端视觉 |
| B   | `ENABLE_SCHEDULE_REMINDER`        | 日程到点播报 | 60 秒检查                           | NVS `reminders`   |
| C   | `ENABLE_SEDENTARY_REMINDER`       | 久坐提醒     | 30 分钟                             | 摄像头（可选）    |
| D   | `ENABLE_BED_EXIT_DETECTION`       | 夜间离床告警 | 2 分钟检查 / 10 分钟超时            | 摄像头 + 云端视觉 |
| E   | `ENABLE_MEDICATION_REMINDER_TASK` | 服药提醒闭环 | 60 秒检查；5 分钟×3 次；30 分钟漏服 | 不依赖摄像头      |

### 服药提醒闭环（任务 E）

完整链路：医嘱计划（`self.medication_reminder` 增删查）→ 后台任务到点弹窗+三声提示音（同时向云端发 `medication_due` 通知，服务器配自动化可触发语音询问）→ 未打卡每 5 分钟再提醒，最多 3 次 → 超 30 分钟未打卡判 `missed` 漏服并弹漏服告警（发 `medication_missed`）→ 老人说"我吃过XX了"打卡（漏服后补卡记为 `late` 迟服）→ report/history 查询。

NVS `medication` 命名空间：
- `reminders`：医嘱计划 `[{time, medicine}]`
- `logs`：打卡事件流 `{"YYYY-MM-DD":[{medicine, time}]}`
- `dYYYYMMDD`：当日剂量状态数组 `[{medicine, planned, status, reminds, actual, delay}]`，自动清理 7 天前数据

### Kconfig 可调参数

| 配置项                             | 类型 | 默认值  | 范围           | 说明                     |
| ---------------------------------- | ---- | ------- | -------------- | ------------------------ |
| `BOARD_FALL_DETECTION_PERIOD_MS`   | int  | 60000   | 30000~3600000  | 跌倒检测间隔（毫秒）     |
| `SEDENTARY_REMINDER_PERIOD_MS`     | int  | 1800000 | 600000~7200000 | 久坐提醒间隔（毫秒）     |
| `BED_EXIT_CHECK_PERIOD_MS`         | int  | 120000  | 30000~600000   | 离床检测拍照间隔（毫秒） |
| `BED_EXIT_EMPTY_TIMEOUT_S`         | int  | 600     | 60~3600        | 床上连续无人超时（秒）   |
| `MEDICATION_REMIND_REPEAT_MINUTES` | int  | 5       | 1~60           | 服药重复提醒间隔（分钟） |
| `MEDICATION_REMIND_MAX_TIMES`      | int  | 3       | 1~10           | 每条计划最多提醒次数     |
| `MEDICATION_MISSED_TIMEOUT_MIN`    | int  | 30      | 5~240          | 漏服判定时限（分钟）     |
| `MEDICATION_LOG_KEEP_DAYS`         | int  | 7       | 1~90           | 剂量记录保留天数         |

## 五、SOS 硬件按键

| 属性     | 值                                                       |
| -------- | -------------------------------------------------------- |
| GPIO     | `GPIO_NUM_3`（config.h 中 `SOS_BUTTON_GPIO`）            |
| 接线     | 按键一端接 GPIO3，另一端接 GND（内部上拉，无需外部电阻） |
| 触发方式 | 长按 3 秒（`long_press_time = 3000`）                    |
| 短按     | 不触发（误按保护）                                       |

### 触发后行为

1. 屏幕显示"紧急求助" + 紧急联系人信息
2. 播放告警提示音（三声连响）
3. 向家属端推送告警通知

### GPIO 选型说明

ESP32-S3-N16R8 上大部分 GPIO 已被摄像头 DVP、显示屏 SPI、I2S 音频、LED 占用。可用引脚经排除后：
- GPIO3：唯一空闲的常规 GPIO（strapping 脚，上电默认高，不影响启动）
- GPIO33/34：备选（N16R8 上可用）

## 六、云端 MCP 工具（family_service.py）

位置：`F:\All_Code\ESP32\mcp-calculator\family_service.py`

需本地运行 `python mcp_pipe.py family_service.py` 并通过 `.env` 中的 `MCP_ENDPOINT` 连接 xiaozhi.me 接入点。电脑关机时这三个工具不可用，但不影响设备端工具和知识库。

| #   | 工具名                  | 功能                          | 数据源                 |
| --- | ----------------------- | ----------------------------- | ---------------------- |
| 18  | `get_medication_report` | 服药周报告（依从率+漏服明细） | `medication_log.json`  |
| 19  | `add_family_message`    | 家属写留言                    | `family_messages.json` |
| 20  | `get_family_messages`   | 老人听取留言                  | `family_messages.json` |

## 七、板级关键设计

### CameraLockGuard 摄像头互斥锁

7 个视觉调用点（内置 take_photo + 3 个板级 MCP 工具 + 3 个后台任务）共享摄像头，通过 Camera 基类的 TryLock/Unlock + RAII 守卫防止并发冲突（摄像头仅 1 个帧缓冲区，并发会导致 cam_hal 超时和硬件卡死）：
- MCP 工具（含内置 take_photo）拿不到锁：立即返回"摄像头正忙，请几秒后再试"
- 后台任务拿不到锁：静默跳过本轮，日志 `camera busy, skip this round`

### 提醒音三声连响

所有告警/提醒音改为三声连响（间隔 2 秒），防止老人忽略短促的单声提示。涉及：SOS、跌倒警报、日程提醒、久坐提醒、离床告警。

### 429 限流友好处理

云端视觉 API 返回 429 时，错误消息改为"服务器繁忙，请稍等再试"，AI 会转述给老人而非干巴巴报错。建议把跌倒检测间隔调大到 120000ms 以上以减少限流。

## 八、架构关系图

```
老人说话 → S3 录音 → 云端 AI 决策
                         │
          ┌──────────────┼──────────────────┐
          ▼              ▼                   ▼
    设备端工具        云端接入点工具        平台知识库
   (S3 执行)       (你的电脑执行)       (AI 内部检索)
  #9-#17 共9个     #18-#20 共3个        6份md文档
  24h在线          电脑开机时可用        24h在线
          │
          ▼
    后台定时任务 (A-D)
    无需说话自动运行
```

## 九、代码位置索引

### MCP 工具注册位置

| 工具范围         | 文件路径                                                           | 函数                                       | 行号范围 |
| ---------------- | ------------------------------------------------------------------ | ------------------------------------------ | -------- |
| #1-#5 官方内置   | `main/mcp_server.cc`                                               | `McpServer::RegisterBuiltinTools()`        | ~39-116  |
| #6-#14 板级工具  | `main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc` | `CompactWifiBoardS3Cam::InitializeTools()` | ~270-830 |
| #15-#17 云端工具 | `family_service.py`                                                | 模块顶层 `@mcp.tool()` 装饰器              | 全文件   |

### 后台任务注册位置

| 任务       | 文件路径                      | 函数                            | 行号范围       |
| ---------- | ----------------------------- | ------------------------------- | -------------- |
| A 跌倒检测 | `compact_wifi_board_s3cam.cc` | `StartFallDetectionTimer()`     | esp_timer 回调 |
| B 日程播报 | `compact_wifi_board_s3cam.cc` | `StartScheduleReminderTimer()`  | esp_timer 回调 |
| C 久坐提醒 | `compact_wifi_board_s3cam.cc` | `StartSedentaryReminderTimer()` | esp_timer 回调 |
| D 离床检测 | `compact_wifi_board_s3cam.cc` | `StartBedExitDetectionTimer()`  | esp_timer 回调 |

### 其他关键文件

| 文件                                                 | 说明                                        |
| ---------------------------------------------------- | ------------------------------------------- |
| `main/boards/bread-compact-wifi-s3cam/config.h`      | 引脚定义（SOS_BUTTON_GPIO = GPIO3）         |
| `main/Kconfig.projbuild`                             | 银发经济功能开关和可调参数（~1140-1231 行） |
| `xiaozhidocs/knowledge_base/`                        | 6 份知识库文档（上传到 xiaozhi.me）         |
| `xiaozhidocs/`                                       | 全部项目文档                                |
| `docs/team-guide.md`                                 | 团队协作指南                                |
| `F:\All_Code\ESP32\mcp-calculator\family_service.py` | 云端 MCP 工具（#18-#20）                    |
| `F:\All_Code\ESP32\mcp-calculator\mcp_pipe.py`       | WebSocket 桥接程序（官方提供，不用改）      |
