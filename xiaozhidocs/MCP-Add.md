# 银发经济 MCP 工具清单

本项目在 XiaoZhi 固件基础上，为 ESP32-S3-CAM（bread-compact-wifi-s3cam）添加了一套完整的银发经济智能语音助手工具链。以下整理所有 MCP 工具、后台任务和硬件功能。

## 一、官方内置工具（mcp_server.cc，所有板子通用）

| # | 工具名 | 功能 | 启用条件 |
|---|--------|------|---------|
| 1 | `self.get_device_status` | 查询设备状态（MAC、IP、版本等） | 默认启用 |
| 2 | `self.audio_speaker.set_volume` | 设置音量 | 默认启用 |
| 3 | `self.screen.set_brightness` | 设置屏幕亮度 | 有显示屏时 |
| 4 | `self.screen.set_theme` | 切换屏幕主题 | 有显示屏时 |
| 5 | `self.camera.take_photo` | 拍照 | 有摄像头时 |

## 二、银发经济工具 — 学习版（mcp_server.cc，Kconfig 开关）

注册位置：`main/mcp_server.cc`，受 `CONFIG_ENABLE_SILVER_ECONOMY_DEMO` 控制。

正式版在板级代码注册同名工具后，框架会自动跳过学习版（日志输出 "Tool xxx already added"），属预期行为。

| # | 工具名 | 功能 | 参数 | 存储 |
|---|--------|------|------|------|
| 6 | `self.medication_reminder` | 服药提醒增删查 | add / remove / list | NVS `medication` |
| 7 | `self.fall_detection` | 拍照+视觉分析是否跌倒 | 无 | 无状态 |
| 8 | `self.family_voice_board` | 家属留言板（文字） | add / list | NVS `voice_board` |

## 三、银发经济工具 — 正式版（板级代码）

注册位置：`main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc` 的 `InitializeTools()` 中。

| # | 工具名 | 功能 | 参数 | 存储 | 说明 |
|---|--------|------|------|------|------|
| 9 | `self.medication_reminder` | 服药提醒增删查 | add / remove / list | NVS `medication` | 正式版，与学习版同名 |
| 10 | `self.fall_detection` | 拍照+视觉分析是否跌倒 | 无 | 无状态 | 返回 `{fell, confidence, description}` |
| 11 | `self.family_voice_board` | 家属留言板（文字） | add / list | NVS `voice_board` | |
| 12 | `self.medication_log` | 用药打卡记录 | checkin / status / list_today | NVS `medication` | 与服药提醒配合，形成闭环 |
| 13 | `self.schedule_reminder` | 通用日程提醒 | add / remove / list | NVS `reminders` | 到点自动播报（需开 Kconfig） |
| 14 | `self.emergency_contact` | 紧急联系人管理 | add / remove / list | NVS `emergency_contacts` | SOS 触发时自动显示 |
| 15 | `self.find_item` | 找东西（拍照+视觉） | item_name | 无状态 | 受 VisionGuard 互斥锁保护 |
| 16 | `self.door_identification` | 门口来人识别 | 无 | 无状态 | 防诈骗，同上互斥锁 |
| 17 | `self.weather_query` | 天气查询 | city（可选，默认自动定位） | 无状态 | wttr.in API，IP 自动定位 |

### 语音触发示例

| 工具 | 对设备说 |
|------|---------|
| `medication_log` | "我吃过降压药了" / "今天药吃了没" |
| `schedule_reminder` | "下午3点提醒我去医院" |
| `emergency_contact` | "存一下我儿子的电话，13800138000" |
| `find_item` | "帮我找找眼镜" |
| `door_identification` | "看看门口是谁" |
| `weather_query` | "今天天气怎么样" |

## 四、后台定时任务（板级 esp_timer）

受 Kconfig 开关控制，位于 `main/Kconfig.projbuild` 的 "Silver Economy Demo" 菜单下。均依赖 `BOARD_TYPE_BREAD_COMPACT_WIFI_CAM`。

| # | Kconfig 开关 | 功能 | 默认间隔 | 依赖 |
|---|-------------|------|---------|------|
| A | `ENABLE_BOARD_FALL_DETECTION` | 周期跌倒检测 | 60 秒 | 摄像头 + 云端视觉 |
| B | `ENABLE_SCHEDULE_REMINDER` | 日程到点播报 | 60 秒检查 | NVS `reminders` |
| C | `ENABLE_SEDENTARY_REMINDER` | 久坐提醒 | 30 分钟 | 摄像头（可选） |
| D | `ENABLE_BED_EXIT_DETECTION` | 夜间离床告警 | 2 分钟检查 / 10 分钟超时 | 摄像头 + 云端视觉 |

### Kconfig 可调参数

| 配置项 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `BOARD_FALL_DETECTION_PERIOD_MS` | int | 60000 | 30000~3600000 | 跌倒检测间隔（毫秒） |
| `SEDENTARY_REMINDER_PERIOD_MS` | int | 1800000 | 600000~7200000 | 久坐提醒间隔（毫秒） |
| `BED_EXIT_CHECK_PERIOD_MS` | int | 120000 | 30000~600000 | 离床检测拍照间隔（毫秒） |
| `BED_EXIT_EMPTY_TIMEOUT_S` | int | 600 | 60~3600 | 床上连续无人超时（秒） |

## 五、SOS 硬件按键

| 属性 | 值 |
|------|-----|
| GPIO | `GPIO_NUM_3`（config.h 中 `SOS_BUTTON_GPIO`） |
| 接线 | 按键一端接 GPIO3，另一端接 GND（内部上拉，无需外部电阻） |
| 触发方式 | 长按 3 秒（`long_press_time = 3000`） |
| 短按 | 不触发（误按保护） |

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

| # | 工具名 | 功能 | 数据源 |
|---|--------|------|--------|
| 18 | `get_medication_report` | 服药周报告（依从率+漏服明细） | `medication_log.json` |
| 19 | `add_family_message` | 家属写留言 | `family_messages.json` |
| 20 | `get_family_messages` | 老人听取留言 | `family_messages.json` |

## 七、板级关键设计

### VisionGuard 互斥锁

6 个视觉调用点（3 个 MCP 工具 + 3 个后台任务）共享摄像头，通过 RAII 互斥锁防止并发冲突：
- MCP 工具拿不到锁：立即返回"摄像头正忙，请几秒后再试"
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

## 九、相关文件索引

| 文件 | 说明 |
|------|------|
| `main/mcp_server.cc` | 官方工具 + 银发经济学习版工具（#1-#8） |
| `main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc` | 板级正式版工具 + SOS + 后台任务（#9-#17, A-D） |
| `main/boards/bread-compact-wifi-s3cam/config.h` | 引脚定义（SOS_BUTTON_GPIO = GPIO3） |
| `main/Kconfig.projbuild` | 银发经济功能开关和可调参数 |
| `docs/knowledge_base/` | 6 份知识库文档（上传到 xiaozhi.me） |
| `docs/team-guide.md` | 团队协作指南 |
| `F:\All_Code\ESP32\mcp-calculator\family_service.py` | 云端 MCP 工具（#18-#20） |
