# AGENTS.md（中文译本）

> 本文件是 [AGENTS.md](file:///f:/All_Code/ESP32/esp32-xiaozhi-chat/AGENTS.md) 的中文翻译，内容与原文保持一致；如两者有歧义，以英文原文为准。

## 项目简介

小智（XiaoZhi）是一个基于 ESP-IDF 的 C/C++ 语音助手固件，支持众多芯片、开发板、显示屏、音频设备和网络传输方式。一次构建只会选定一个板级实现。

尽量使用 ESP-IDF v6.1。支持的最低 SDK 版本为 ESP-IDF v6.0.1。不支持 IDF 5.x。

## 架构

- `main/application.*`：主事件循环、协议生命周期和高层行为。
- `main/device_state_machine.*`：运行时合法状态转换的规则。
- `main/boards/common/`：板级接口与可复用的硬件/网络辅助工具。
- `main/boards/**/`：各板子专用的引脚、初始化和构建变体。
- `main/audio/`：编解码器、音频任务、处理引擎、唤醒词和队列。
- `main/protocols/`：与传输方式无关的统一 API，以及 WebSocket 和 MQTT/UDP 两种实现。
- `main/display/` 和 `main/led/`：可复用的界面（UI）实现。
- `main/mcp_server.*`：设备端通用 MCP 工具及其调度。
- `main/Kconfig.projbuild`：板型与功能的配置菜单。
- `main/CMakeLists.txt`：源文件、板型、语言、字体与资源的选择。
- `scripts/build.py`：板型/变体构建的标准入口。

新增实现之前，先阅读与之最接近的现有实现。优先选择职责范围最小的归属层；不要把板子专用的行为放进核心模块。

## 必须遵守的规则

- 保留工作区（worktree）中与本次任务无关的改动，保持补丁聚焦。
- 一次构建必须通过 `DECLARE_BOARD(...)` 恰好导出一个板级工厂。
- 绝不为支持不同硬件而修改已有板子的引脚。应新增一个唯一命名的板子或发布变体；板型身份会影响 OTA 兼容性。
- 核心代码只依赖 `Board` 接口，绝不依赖某个具体板子类或板子的 `config.h`。
- 把摄像头、背光、显示屏、LED、电池等类似能力视为可选能力。
- 运行时状态的改变必须通过 `Application::SetDeviceState()` 和状态机进行。
- 回调可能运行在主任务之外。涉及应用状态修改时，要用 `Application::Schedule()` 或事件位来安排。
- 不要阻塞主事件循环或音频任务。在音频路径中避免使用无界队列，避免反复进行大块内存分配。
- 共享的消息语义放在 `Protocol` 中；修改其约定时，两种传输方式都要验证。
- 对网络输入做校验，并注意保持 `cJSON` 的内存所有权。NVS 键名属于持久化 API，变更时需要做迁移。
- 用 Kconfig/组件规则对目标平台专属的功能加以保护。不要假设每个目标平台都有 PSRAM 或 S3/P4 那样的资源。
- 不要手动编辑生成的或第三方的产物：`build/`、`releases/`、`managed_components/`、`components/`、`sdkconfig*`、`main/assets/lang_config.h`，以及生成的 mmap 头文件。
- 只用仓库的 `.clang-format` 格式化自己改动过的 C/C++ 文件；避免对无关文件做大规模格式化。

## 板子与配置

板型选择是一条相互关联的链条：

`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → 板级源码与 `config.h`。

新增板子或变体时，要更新链条中每一个相关环节。包括：唯一的板型身份、正确的芯片目标、Flash/分区设置、恰好一个 `DECLARE_BOARD`，以及板子文档。遵循 `docs/custom-board.md`。

## 常用命令

先 source（加载）目标 ESP-IDF 环境：

```sh
source /path/to/esp-idf/export.sh
idf.py --version
```

```sh
# 查看准确的板型与变体名称
python3 scripts/build.py --list-boards

# 标准变体构建
python3 scripts/build.py <板型目录> --name <变体名>

# 主机侧构建测试
python3 -m unittest discover -s scripts/tests -v

# 格式化/检查改动过的文件
clang-format -i <文件>
clang-format --dry-run -Werror <文件>
```

构建脚本会改变本地的 `sdkconfig` 和构建状态。不要想当然地认为构建目录仍对应上一次的目标平台。

## 验证

- 仅改动板级代码：构建受影响的变体，并对改动涉及的硬件做冒烟测试。
- 改动核心、公共板级、音频、协议、显示、依赖、Kconfig 或 CMake：运行主机测试，并构建有代表性的、受影响的芯片/网络路径。
- 改动协议：当共享行为发生变化时，WebSocket 和 MQTT/UDP 都要验证。
- 改动音频：验证采集、播放、唤醒/VAD、打断、重连，以及适用的 AEC（回声消除）模式。
- 改动 UI/资源：验证无屏/OLED/LVGL 各路径以及分区大小。
- 始终报告"测了什么"和"还有什么必须靠真实硬件验证"。构建成功不等于硬件验证通过。

## 权威文档

- 项目概览与 SDK 策略：`README.md`
- 板子接入指南：`docs/custom-board.md`
- 音频设计：`main/audio/README.md`
- 代码风格：`docs/code_style.md`
- 协议说明：`docs/websocket.md`、`docs/mqtt-udp.md`、`docs/mcp-protocol.md`
- CI 构建矩阵：`.github/workflows/build.yml`

详细或变化快的信息应放在上述文件中，而不是这里。只有当某个子系统需要专门说明时，才在其目录下新增一层嵌套的 `AGENTS.md`。
