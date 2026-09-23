# 团队协作指南

本指南面向加入 esp32-silver-economy 项目的团队成员，涵盖环境搭建、日常开发、代码同步全流程。

## 一、首次加入

### 1. 获取仓库权限

仓库管理员（Sanshui755）需要在 GitHub 上添加你为协作者：

1. 打开 https://github.com/Sanshui755/esp32-silver-economy/settings/access
2. 点击 "Add a new collaborator"
3. 输入你的 GitHub 用户名，添加

你会收到邮件邀请，点击接受即可。

### 2. 克隆仓库

```powershell
# 选一个你喜欢的目录
cd F:\All_Code\ESP32

# 克隆仓库
git clone https://github.com/Sanshui755/esp32-silver-economy.git
cd esp32-silver-economy
```

### 3. 配置你的 Git 身份

```powershell
git config user.name "你的名字"
git config user.email "你的邮箱"
```

### 4. 搭建 ESP-IDF 开发环境

参见仓库根目录的 `README.md`，核心要点：

- 安装 ESP-IDF v6.1（最低支持 v6.0.1，不支持 5.x）
- 目标板子：`bread-compact-wifi-s3cam`（ESP32-S3-N16R8 + 摄像头 + LCD）
- 构建：`python scripts/build.py bread-compact-wifi-s3cam --name default`

Windows 环境推荐用 EIM 安装：

```powershell
# 参考 https://docs.espressif.com/eim/
# 安装后激活环境
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
idf.py --version
```

### 5. 验证构建

```powershell
# 激活 ESP-IDF 环境后
cd F:\All_Code\ESP32\esp32-silver-economy
idf.py set-target esp32s3
idf.py build
```

构建成功说明环境就绪。

---

## 二、日常开发流程

### 1. 开工前：先拉取最新代码

```powershell
git pull origin main
```

**每次开始写代码前都先拉**，避免和别人的提交冲突。

### 2. 创建功能分支（推荐）

不要直接在 main 上写代码，为每个功能开一个分支：

```powershell
# 从最新的 main 创建分支
git checkout -b feature/你的功能名

# 示例
git checkout -b feature/add-voice-message
git checkout -b fix/fall-alert-dedup
git checkout -b docs/update-knowledge-base
```

### 3. 写代码

正常开发，期间随时提交：

```powershell
# 查看改了什么
git status
git diff

# 提交
git add <你改的文件>          # 只加你改的，不要 git add -A
git commit -m "feat: 简短描述你做了什么"
```

### 4. 提交信息规范

| 前缀 | 用途 | 示例 |
|------|------|------|
| `feat:` | 新功能 | `feat: 添加语音留言播放功能` |
| `fix:` | 修 bug | `fix: 跌倒告警重复触发问题` |
| `docs:` | 文档改动 | `docs: 更新用药安全指南` |
| `refactor:` | 重构（不改行为） | `refactor: 提取视觉锁为公共方法` |
| `chore:` | 杂项（配置等） | `chore: 更新 .gitignore` |

### 5. 推送到 GitHub

```powershell
# 第一次推送新分支
git push -u origin feature/你的功能名

# 之后的推送
git push
```

### 6. 创建 Pull Request

1. 打开 https://github.com/Sanshui755/esp32-silver-economy/pulls
2. 点击 "New Pull Request"
3. 选择你的分支 → main
4. 填写标题和说明
5. 请队员或管理员 review 后合并

---

## 三、同步代码

### 1. 拉取团队最新代码

```powershell
# 确保在 main 分支
git checkout main

# 拉取并合并
git pull origin main
```

### 2. 同步官方上游更新（小智官方仓库）

本项目 fork 自 78/xiaozhi-esp32，官方会持续更新。同步方法：

```powershell
# 添加官方远程（只需一次）
git remote add upstream https://github.com/78/xiaozhi-esp32.git

# 拉取官方更新
git fetch upstream

# 在 main 上合并官方更新
git checkout main
git merge upstream/main

# 如有冲突，解决后：
git add .
git commit
git push origin main
```

**注意**：官方更新可能改动你修改过的文件（如 `mcp_server.cc`、板子代码），合并时注意解决冲突，不要直接覆盖官方的安全修复。

### 3. 查看远程仓库配置

```powershell
git remote -v
```

预期输出：
```
origin    https://github.com/Sanshui755/esp32-silver-economy.git (fetch)
origin    https://github.com/Sanshui755/esp32-silver-economy.git (push)
upstream  https://github.com/78/xiaozhi-esp32.git (fetch)
upstream  https://github.com/78/xiaozhi-esp32.git (push)
```

---

## 四、项目结构速览

```
esp32-silver-economy/
├── main/
│   ├── application.*              # 主事件循环、协议生命周期
│   ├── device_state_machine.*     # 运行状态机
│   ├── mcp_server.cc              # MCP 工具注册（银发经济工具在这里）
│   ├── Kconfig.projbuild          # 板子选择 + 银发经济功能开关
│   ├── boards/
│   │   └── bread-compact-wifi-s3cam/
│   │       ├── compact_wifi_board_s3cam.cc  # 板级工具实现（9个MCP + SOS + 后台任务）
│   │       └── config.h           # 引脚定义（SOS按钮在GPIO3）
│   ├── audio/                     # 音频编解码、唤醒词
│   ├── protocols/                 # WebSocket / MQTT 传输
│   └── display/                   # 显示驱动
├── docs/
│   ├── knowledge_base/            # 知识库文档（6份，上传到xiaozhi.me）
│   ├── custom-board.md            # 自定义板子指南
│   ├── mcp-protocol.md            # MCP 协议文档
│   └── websocket.md               # WebSocket 协议文档
├── scripts/
│   └── build.py                   # 构建入口
├── README.md                       # 项目概览和SDK策略
└── AGENTS.md                       # AI 协作规范
```

### 银发经济相关文件（改动最频繁）

| 文件 | 内容 | 改动注意 |
|------|------|---------|
| `main/mcp_server.cc` | 9 个 MCP 工具注册 | 工具描述写清楚，影响 AI 调用决策 |
| `main/Kconfig.projbuild` | 功能开关（ENABLE_SILVER_ECONOMY_DEMO 等） | 改 Kconfig 需重新 menuconfig |
| `main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc` | 工具实现 + SOS + 后台任务 | 这是板级代码，不要往核心层塞板子逻辑 |
| `main/boards/bread-compact-wifi-s3cam/config.h` | 引脚定义 | 不要改已有引脚，新增引脚先查占用 |
| `docs/knowledge_base/` | 知识库文档 | 内容必须真实，医疗建议以医嘱为准 |

---

## 五、构建与烧录

### 1. 构建前配置

```powershell
# 激活 ESP-IDF
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"

# 设置目标芯片
cd F:\All_Code\ESP32\esp32-silver-economy
idf.py set-target esp32s3

# 配置（首次或改了 Kconfig 后必须）
idf.py menuconfig
# → Xiaozhi Assistant → 选择板子 bread-compact-wifi-s3cam
# → Xiaozhi Assistant → Silver Economy Demo → 按需开关功能
```

### 2. 编译烧录

```powershell
# 编译
idf.py build

# 烧录 + 监视器（COM 端口按实际改）
idf.py -p COM21 flash monitor

# 退出监视器：Ctrl + ]
```

### 3. 常用操作

```powershell
# 只擦除 NVS（清空服药记录等，不动固件）
idf.py -p COM21 erase-otadata

# 全擦（清空所有数据，包括 NVS）
idf.py -p COM21 erase-flash

# 只编译不烧录（快速检查代码能否编过）
idf.py build
```

---

## 六、测试清单

改完代码后，按影响范围测试：

| 改动范围 | 必测项 |
|---------|--------|
| MCP 工具（mcp_server.cc / 板级 cc） | 烧录后看 9 条 `Add tool` 日志，语音触发各工具 |
| Kconfig（功能开关） | menuconfig 确认选项出现，开关后构建验证 |
| config.h（引脚） | 编译通过 + 硬件功能正常 |
| 知识库文档 | 上传 xiaozhi.me，语音提问验证 AI 检索 |
| 音频/协议核心代码 | 编译 + 烧录 + 对话测试 + 唤醒测试 + 重连测试 |

---

## 七、常见问题

### Q：git push 报权限错误
- 确认管理员已添加你为 collaborator
- 确认你用的是 GitHub 账号的 HTTPS 或 SSH 方式
- `git remote -v` 检查地址是否正确

### Q：合并冲突怎么办
```powershell
git pull origin main
# 冲突时 git 会提示哪些文件冲突
# 打开冲突文件，找 <<<<<<< 标记，手动选择保留哪部分
# 解决后：
git add <冲突文件>
git commit
git push
```

### Q：构建报错 " IDF_PATH not set"
```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
```
每次开新的 PowerShell 窗口都要重新激活。

### Q：menuconfig 看不到银发经济选项
- 确认选了正确的板子：`bread-compact-wifi-s3cam`
- 确认 Kconfig.projbuild 语法正确（改过的话）
- 删 build 目录重新构建：`Remove-Item build -Recurse -Force; idf.py reconfigure`

### Q：改了 Kconfig 后构建报错
```powershell
# 清理后重新配置
idf.py fullclean
idf.py reconfigure
idf.py build
```

### Q：烧录后设备不启动
- 检查串口日志：`idf.py -p COM21 monitor`
- 常见原因：分区表不够大、PSRAM 配置不对、引脚冲突
- 用 `idf.py erase-flash` 全擦后重新烧录

---

## 八、代码规范

遵循仓库的 `AGENTS.md` 和 `docs/code_style.md`，核心要点：

- 一个构建只导出一个 `DECLARE_BOARD`
- 不要修改已有板子的引脚定义
- 核心代码依赖 `Board` 接口，不依赖具体板子类
- 状态变更走 `Application::SetDeviceState()`
- 回调里修改 UI 用 `Application::Schedule()`
- 不阻塞主事件循环和音频任务
- 用仓库的 `.clang-format` 格式化你改的文件
- 只格式化你动过的文件，不要全量格式化

```powershell
# 格式化改过的文件
clang-format -i main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc
```
