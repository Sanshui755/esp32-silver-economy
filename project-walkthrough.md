# 小智项目完整导读：总纲 + 实战走读 + 三大子系统深挖

> 适用硬件：`bread-compact-wifi`（ESP32-S3）+ I²S 麦克风/喇叭 + 0.91 寸 SSD1306 128×32 OLED
> SDK：ESP-IDF v6.x（FreeRTOS）
>
> 本文档是三篇连续讲解的合集：
> 1. **项目总览**——七层架构地图，从硬件到云端
> 2. **实战走读**——按 boot 键 → 开始听音的完整调用链（每步行号都经过核实）
> 3. **深挖三连**——MCP 服务器 / OTA 升级 / MQTT+UDP 协议
>
> 配套文档：
> - [audio-pipeline-walkthrough.md](audio-pipeline-walkthrough.md)（音频管线 4 任务 5 队列的逐站细节，本文不重复展开）
> - [embedded-concepts.md](embedded-concepts.md)（六大基础概念小课堂：Flash / NVS / Bootloader / DMA / 采样率位深 / LVGL）

---

## 目录

- [第一部分：项目总览（七层地图）](#第一部分项目总览七层地图)
- [第二部分：实战走读——按 boot 键 → 开始听音](#第二部分实战走读按-boot-键--开始听音)
- [第三部分：深挖 1/3——MCP 服务器](#第三部分深挖-13mcp-服务器)
- [第四部分：深挖 2/3——OTA 升级机制](#第四部分深挖-23ota-升级机制)
- [第五部分：深挖 3/3——MQTT+UDP 协议](#第五部分深挖-33mqttudp-协议)
- [第六部分：设计母题总结](#第六部分设计母题总结)

---

# 第一部分：项目总览（七层地图）

## 1.1 一句话理解

小智（XiaoZhi）= 跑在 ESP32 上的 **AI 语音助手固件**。工作循环：

> 听（麦克风）→ 发给云端（大模型）→ 说（喇叭），顺便用屏幕/灯/按键做界面，并通过 MCP 协议控制设备。

所有代码都在伺候这几件事。分层地图（从下往上）：

```
┌─────────────────────────────────────────────────────────────┐
│  第7层 云端：xiaozhi.me 服务器（ASR语音识别 + LLM大模型 + TTS）│
├─────────────────────────────────────────────────────────────┤
│  第6层 应用核心  Application（事件循环 + 设备状态机 + 大管家）  │
│        main.cc / application.* / device_state_machine.*       │
├──────────────┬──────────────────────┬───────────────────────┤
│ 第5层 协议层  │  Protocol 抽象接口                           │
│  网络通信     │  WebSocket / MQTT+UDP（两种传输，同一套语义）  │
├──────────────┴──────────────────────┴───────────────────────┤
│  第4层 音频管线 AudioService（输入/编解码/输出任务+队列）       │
│        AudioEngine（唤醒词ESP-SR/VAD/AEC）+ Opus 编解码        │
├──────────────┬──────────────────────┬───────────────────────┤
│ 第3层 能力驱动 │ AudioCodec 声卡       │ Display 屏幕          │
│  (可插拔)     │ Led 灯 / Button 按键  │ Camera / 电池 / 背光  │
├──────────────┴──────────────────────┴───────────────────────┤
│  第2层 板级抽象 Board（一块板子一个实现）                       │
│        boards/common/board.h + boards/bread-compact-wifi/     │
├─────────────────────────────────────────────────────────────┤
│  第1层 地基：ESP-IDF v6（FreeRTOS / I²S / I²C / WiFi /        │
│        NVS / OTA）+ LVGL 图形库 + managed_components          │
├─────────────────────────────────────────────────────────────┤
│  第0层 硬件：ESP32-S3 + 麦克风 + 喇叭 + 128x32 OLED           │
└─────────────────────────────────────────────────────────────┘
```

核心设计思想：**下层不知道上层，上层只认识接口不认识具体板子**——所以同一份固件支持 138 种板子。

## 1.2 第 0 层：硬件

引脚映射见 [main/boards/bread-compact-wifi/config.h](main/boards/bread-compact-wifi/config.h)：

| 硬件     | 引脚                      | 总线           | 干嘛的         |
| -------- | ------------------------- | -------------- | -------------- |
| 麦克风   | WS=4, SCK=5, DIN=6        | I²S（控制器1） | 采集声音       |
| 喇叭功放 | DOUT=7, BCLK=15, LRCK=16  | I²S（控制器0） | 播放回答       |
| OLED     | SDA=41, SCL=42，地址 0x3C | I²C            | 表情/状态/字幕 |
| BOOT 键  | GPIO0                     | GPIO           | 开聊/打断      |
| 触摸键   | GPIO47                    | GPIO           | 按住说话       |
| 音量 ±   | GPIO40 / GPIO39           | GPIO           | 调音量         |
| LED      | GPIO48                    | GPIO           | 状态指示       |

四个必懂硬件名词：**GPIO**（普通引脚）、**I²C**（两线慢速总线，控制屏）、**I²S**（音频高速总线）、**NVS**（Flash 里的键值存储，存 WiFi 密码等）。另有两个隐形角色：**DMA**（外设↔内存自动搬运工）、**采样率/位深**（16kHz×16bit → 原始声音每秒 32KB，这就是必须压缩的原因）。

## 1.3 第 1 层：ESP-IDF + FreeRTOS

- **FreeRTOS 多任务**：本项目同时跑主任务（决策）、audio_input（录音）、opus_codec（压缩解压）、audio_output（放音）、audio_afe（唤醒/VAD/AEC）、LVGL 任务（刷屏）。
- **任务间通信只有两种姿势**：
  - **队列 = 传送带**：传数据（音频帧）。
  - **事件组 = 铃铛**：传"发生了某件事"，睡着的任务被唤醒处理。
- **ESP-IDF**：乐鑫官方 SDK，把 I²C/屏幕/存储/WiFi/OTA 封装成函数。`managed_components/` 是自动下载的第三方组件（SSD1306 驱动、LVGL、Opus、ESP-SR），**不要手改**。

## 1.4 构建系统：配置传导链（详细版）

**要解决的唯一问题**：同一份代码，怎么编译出 138 种不同的设备？小智的办法是**把"选配置"和"写代码"彻底分开**——你不碰核心代码，只动配置文件，配置像水一样一站一站往下传，最终流进 C 代码。

### 5 个角色（点餐类比）

把编译一次固件想象成**去餐厅点一份定制套餐**：

| 角色                                 | 现实类比                         | 项目里的文件                                                                                            |
| ------------------------------------ | -------------------------------- | ------------------------------------------------------------------------------------------------------- |
| **菜单模板**（印着可选的菜）         | "本店有：米饭/面条，可乐/雪碧……" | [Kconfig.projbuild](main/Kconfig.projbuild)                                                             |
| **点菜单**（你勾好的那一张）         | "米饭✓ 可乐✓"                    | `sdkconfig`（项目根目录，**生成物**）                                                                   |
| **服务员**（帮你把套餐默认口味勾上） | "面包板套餐默认加辣"             | [scripts/build.py](scripts/build.py) + 板子的 [config.json](main/boards/bread-compact-wifi/config.json) |
| **厨房**（按菜单决定做哪些菜）       | 看到勾了米饭就煮饭               | [main/CMakeLists.txt](main/CMakeLists.txt)                                                              |
| **菜的调料开关**（代码里的 if）      | "勾了加辣→放辣椒粉"              | 板子的 [config.h](main/boards/bread-compact-wifi/config.h)                                              |

**关键认知**：配置不是一次到位，而是**每一站读到的都是上一站的产物**。

### 逐个看真实文件

**① 板子的"名片"：config.json**（只被 build.py 读，编译器看不见）：

```json
{
    "type": "bread-compact-wifi",
    "target": "esp32s3",              ← 芯片目标
    "builds": [                        ← 两个"口味变体"
        {
            "name": "bread-compact-wifi",              ← 变体1：默认
            "sdkconfig_append": [ "CONFIG_OLED_SSD1306_128X32=y" ]
        },
        {
            "name": "bread-compact-wifi-128x64",       ← 变体2：同一块板，64 行屏
            "sdkconfig_append": [ "CONFIG_OLED_SSD1306_128X64=y" ]
        }
    ]
}
```

作用一句话：**"我这块板子有哪些口味，每种口味要在点菜单上追加哪几笔。"**

**② 菜单模板：Kconfig.projbuild**（乐鑫统一语法，定义所有可选开关）：

```kconfig
# 板型选择（choice = 单选框）
config BOARD_TYPE_BREAD_COMPACT_WIFI
    bool "Bread Compact Wi-Fi (面包板)"
    depends on IDF_TARGET_ESP32S3          ← 只有芯片目标是 S3 时这个选项才出现

# 你的板子才有的子菜单：OLED 型号二选一
choice DISPLAY_OLED_TYPE
    depends on BOARD_TYPE_BREAD_COMPACT_WIFI || ...
    prompt "OLED Type"
    default OLED_SSD1306_128X32            ← 不追加配置时的默认值恰好也是 32
    config OLED_SSD1306_128X32
        bool "SSD1306 128*32"
    config OLED_SSD1306_128X64
        bool "SSD1306 128*64"
```

它不写逻辑、不定数值，只回答：**"这个项目允许用户选择什么？"**

**③ 点菜单：sdkconfig**（构建系统处理 Kconfig 后写出的最终事实，别的文件都以它为准）：

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y       ← 板型选了面包板
CONFIG_OLED_SSD1306_128X32=y                 ← 屏幕选了 32 行
CONFIG_OLED_SSD1306_128X64=                  ← 没选
```

**④ 厨房：CMakeLists.txt**（[L92-97](main/CMakeLists.txt#L92-L97)）读点菜单，决定把哪个板子目录的源码喂给编译器：

```cmake
if(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)
    set(BOARD_DIR "bread-compact-wifi")              ← 只编这个目录！
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1) ← 顺带决定字体
elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ML307)
    set(BOARD_DIR "bread-compact-ml307")
...
```

所以"一次编译只有一个 DECLARE_BOARD"不是靠运气——**是厨房只拿了一个板子目录的食材**。

**⑤ 调料开关：config.h**（[L41-50](main/boards/bread-compact-wifi/config.h#L41-L50)）。C 代码不能直接读 sdkconfig，但编译时 sdkconfig 会被转成 C 宏（`CONFIG_XXX`）：

```c
#if CONFIG_OLED_SSD1306_128X32      ← 宏为真 → 下面这行生效
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#else
#error "OLED display type is not selected"   ← 一个都没选？编译直接报错
#endif
```

**这就是"传导"的终点**：上游一路传下来的一个 `=y`，最终变成代码里的 `DISPLAY_HEIGHT 32`，进而决定 OLED 用 32 行布局。

### 完整走一遍（以你的板子为例）

```
python scripts/build.py bread-compact-wifi --name bread-compact-wifi
```

```
① build.py 读 config.json → 芯片目标=esp32s3，找到变体"bread-compact-wifi"
   → 把 sdkconfig_append 写进点菜单 sdkconfig

② 构建系统处理 sdkconfig + Kconfig.projbuild
   → Kconfig 有 "default BOARD_TYPE_BREAD_COMPACT_WIFI if IDF_TARGET_ESP32S3"
   → 芯片目标是 S3 → 板型自动选成面包板
   → sdkconfig 固化为 CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y + CONFIG_OLED_SSD1306_128X32=y

③ CMakeLists.txt 读到 CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y
   → BOARD_DIR = "bread-compact-wifi"，只编译该目录 + 核心源码

④ 编译器编译 config.h
   → CONFIG_OLED_SSD1306_128X32 为真 → DISPLAY_HEIGHT = 32
   → oled_display.cc 的 SetupUI() 看到 32 → 走 SetupUI_128x32 布局
```

**四个文件，四次接力，一个开关值从"json 里的一行"变成了"屏幕上的 32 行像素"。**

### 最容易懵的三个点

1. **`config.json` 和 `config.h` 名字像双胞胎，其实毫无关系**：前者给 Python 脚本看的"名片"（声明有哪些变体）；后者给 C 编译器看的"硬件参数表"（引脚、由 CONFIG_ 宏推出的尺寸）。
2. **`sdkconfig` 为什么不要手改**：它是生成物，`build.py` 每次构建都可能重写，手改会被冲掉。想改配置永远走上游：改 `config.json` 的 `sdkconfig_append`，或 `idf.py menuconfig`（图形菜单，本质就是可视化编辑点菜单）。
3. **"变体 variant"变的是什么**：同一块物理板子的不同**口味**。硬件不用动，但可配出 32 行屏/64 行屏两种固件；每个变体一个名字（OTA 时服务器按名字区分），`sdkconfig_append` 就是"这个口味在点菜单上多勾的几笔"。

> 生成物清单（不进 git、别手改）：`build/`、`sdkconfig`、`managed_components/`。

## 1.5 第 2 层：板级抽象（详细版，全项目最重要的设计）

**问题**：138 种板子，核心代码怎么做到"不知道板子是谁"？如果不用抽象，核心代码就得写 138 个 if 判断板型、各塞一套引脚——每加一块板子全项目都要改。

**解法**：核心代码永远只说抽象的话——"给我一块屏幕"，至于是 OLED 还是 AMOLED，**编译那一刻才由配置决定**（依赖接口，不依赖实现）。

### C++ 三件套预备知识

1. **类 = 图纸，对象 = 实物**：`class Dog` 是图纸，`Dog d` 是按图纸造出的实物。
2. **继承 = 在别人图纸上加东西**：`class Dog : public Animal` 自动拥有 Animal 的能力，再加自己的。
3. **虚函数 = 合同条款**（全部核心）：

```cpp
class Animal {
    virtual void 叫() = 0;    // 纯虚函数 = "必填题"：我不管你怎么叫，但你必须有"叫"
    virtual void 睡() { ... } // 普通虚函数 = "选做题"：我给默认做法，你可以改写
};
Animal* a = new Dog();
a->叫();   // 多态：变量类型是"动物"，实际执行的是 Dog 的"汪汪"
```

调用方只说"动物，叫一声"，执行的是子类版本——**调用方和实现方就此解耦**。

### 逐行看 board.h（真·合同书）

[board.h](main/boards/common/board.h) 共 92 行，骨架分四部分：

**第 1 部分：防复制保险（L49-52）**

```cpp
class Board {
private:
    Board(const Board&) = delete;              // 禁用拷贝构造
    Board& operator=(const Board&) = delete;   // 禁用赋值
```

Board 是单例（全世界只能有一块"当前板子"），`= delete` 把复制粘贴的路焊死。

**第 2 部分：合同条款（L68-84）——必填题与选做题**

```cpp
virtual std::string GetBoardType() = 0;    // 必填：报上板子名
virtual AudioCodec* GetAudioCodec() = 0;   // 必填：交出声卡（语音设备必有）
virtual NetworkInterface* GetNetwork() = 0;// 必填：交出网络
virtual void StartNetwork() = 0;           // 必填：开始联网

virtual Display* GetDisplay();             // 选做：基类默认返回 nullptr（无屏板子）
virtual Led* GetLed();                     // 选做：灯
virtual Camera* GetCamera();               // 选做：相机
virtual Backlight* GetBacklight() { return nullptr; }  // 选做：默认"没有背光"
```

**关键**：`= 0` 结尾是必填题（每块板必须实现）；没有 `= 0` 是选做题（基类给默认答案"没有"，有硬件可以改写）。**这就是"屏幕/灯/相机是可选能力、声卡是必有能力"的代码载体**——AGENTS.md 里"Treat camera, backlight, display… as optional"这条规则的出处。

**第 3 部分：单例 + 神秘钩子（L46, 62-65）**

```cpp
void* create_board();        // ← 只"声明"，没人定义它！？

static Board& GetInstance() {
    static Board* instance = static_cast<Board*>(create_board());  // 首次调用时创建
    return *instance;
}
```

头文件只承诺"会有一个叫 create_board 的函数"，**定义权留给板子**——一个预留插槽：

```
board.h：       "这里有个插槽，叫 create_board"
某板子的 .cc：  把插头插上 → "create_board 就是造我"
编译时：        只编一个板子的 .cc → 全工程只有这一个插头 → 插槽必被它占
```

**第 4 部分：DECLARE_BOARD 宏（L87-90）——插头本体**

```cpp
#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}
```

在 [compact_wifi_board.cc L188](main/boards/bread-compact-wifi/compact_wifi_board.cc#L188) 写 `DECLARE_BOARD(CompactWifiBoard);`，预处理展开成：

```cpp
void* create_board() { return new CompactWifiBoard(); }
```

因为 CMake 只编译你的板子目录（1.4 的"厨房只拿一个板子目录的食材"），**链接器在全工程只会找到这一个 create_board 定义**。换板子编译，插头自动换——"一次构建必须恰好一个 DECLARE_BOARD"的原因。

### 继承链：通用逻辑一层层下沉

光有合同不够——138 块 WiFi 板的"连 WiFi/开配网热点"逻辑全一样，不能每块抄一遍。于是三层各干各的事：

```
Board            （合同：只有条款，没有实现）
 └─ WifiBoard    （半成品：把 WiFi 派板子的通用逻辑全实现掉）
     └─ CompactWifiBoard （成品：只填引脚和器件，一行网络代码不用写）
```

[wifi_board.h](main/boards/common/wifi_board.h) 把合同一大半必填题都答了：

```cpp
class WifiBoard : public Board {                  // "我是 Board 的 WiFi 派"
    void StartWifiConfigMode();                   // 配网热点：写好了
    void TryWifiConnect();                        // 连 WiFi：写好了
    virtual void StartNetwork() override;         // 合同必填题 → WiFi 版答案
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
};
```

`override` = "**我来回答合同里的这道题**"。你的 [compact_wifi_board.cc](main/boards/bread-compact-wifi/compact_wifi_board.cc) 里 `class CompactWifiBoard : public WifiBoard`，连 StartNetwork 都不用写（继承现成答案），只实现"我的麦克风接哪、我的屏是啥"。

**类比**：Board 是国家法律（必须有什么）；WifiBoard 是省条例（WiFi 省统一细则）；CompactWifiBoard 是村规民约（只补本村细节）。核心代码只引用国家法律，永远不往下引用。

### 核心代码怎么用（抽象的最终证明）

[application.cc](main/application.cc) 里 `Board::GetInstance()` 出现 15+ 处，典型如 [L110](main/application.cc#L110)：

```cpp
auto display = Board::GetInstance().GetDisplay();
display->SetStatus("正在连接…");
```

实验性证明：在 application.cc 里 **grep "CompactWifiBoard" 一个结果都没有**。核心代码从头到尾不知道你的板子叫什么，换 AMOLED 板、4G 板重编译，application.cc 一个字不用改。整条链：

```
application.cc:  Board::GetInstance().GetDisplay()->SetStatus("正在连接…")
                        │ 多态分发
                        ▼
create_board()   ←── compact_wifi_board.cc 的 DECLARE_BOARD（编译期钉死）
                        │
                        ▼
CompactWifiBoard::GetDisplay() 返回 OledDisplay（已按 config.h 引脚初始化好）
                        │
                        ▼
OledDisplay::SetStatus() → LVGL → I²C → 128x32 屏幕亮出字
```

### 五个概念深问：防复制 / virtual / 钩子 / 插槽 / override

**① 防复制保险 `= delete`**（board.h L51-52）

`Board(const Board&) = delete;` = "拷贝 Board 这个操作，编译器禁止"。没有它：`Board b2 = b1;` 合法，程序里出现多个"板子"对象——但物理设备只有一块，板子对象握着 I²C 句柄、声卡指针这些真实硬件资源，两个对象都认为"麦克风归我管"就会重复初始化、互相打架。类比：身份证不能复印。**更深的用意：把错误从运行时挪到编译时**——没保险时灵异 bug 查几天，有保险时编译当场报错。

**② `virtual` —— 多态的开关**

`virtual` 标在函数上 = "调用时别看变量类型，看对象实际类型"。10 行实验：

```cpp
class Animal {
public:
    void 睡()  { printf("呼呼"); }          // 普通函数
    virtual void 叫() { printf("……"); }     // virtual 函数
};
class Dog : public Animal {
public:
    void 叫() { printf("汪！"); }           // 改写了"叫"
};
Animal* a = new Dog();    // 变量类型是 Animal*，装的是狗
a->睡();                  // "呼呼"——没悬念
a->叫();                  // "汪！" ← 魔法在这
```

- **没有 virtual**：编译器只看变量类型（Animal*）→ 调 Animal 版本 → 输出"……"，子类的"汪"被无视
- **有 virtual**：每个对象偷偷带一张表（虚函数表），写着"我实际用哪个版本"，调用时查表 → 找到 Dog 的版本

项目意义：`board->GetDisplay()` 没有 virtual 的话，永远执行 Board 默认版（返回空）→ 屏幕永远不亮。**没有 virtual，合同式抽象整栋楼塌掉。**

**③ 钩子（hook）—— 框架定时机，你填内容**

钩子 = 框架预留的空位。类比 USB 口：电脑（框架）规定"这个口通电、有协议、会被系统调用"，插鼠标还是 U 盘你说了算。本质是控制反转：平时"你调框架的函数"，有钩子后"框架在合适时机调你填的代码"。项目里已见过三个：

| 钩子                     | 谁定时机                          | 谁填内容                   |
| ------------------------ | --------------------------------- | -------------------------- |
| 按键 `OnClick(...)`      | 按键组件（检测到单击）            | 板子的 lambda（开聊/配网） |
| MCP `AddTool(..., 回调)` | MCP 框架（AI 调工具时）           | 板子的"拉高 GPIO 开灯"     |
| `create_board()`         | `Board::GetInstance()` 首次取板子 | 板子的 `DECLARE_BOARD` 宏  |

**④ 预留插槽 —— create_board 的声明/定义分离**

先补 C++ 基础：**声明 vs 定义**。

```cpp
void* create_board();                       // 声明：只说"有这么个函数"（名片）
void* create_board() { return new X(); }    // 定义：函数真身（本人到场）
```

规则：编译某文件时见到**声明**就敢调用；**链接**（拼装所有编译产物）时必须找到**恰好一个定义**。board.h 的花样：只给声明（[L46](main/boards/common/board.h#L46)），定义留白——插槽。为什么"恰好一个"能成立？1.4 的厨房只喂一个板子目录：

- 插头 0 个（忘写 DECLARE_BOARD）→ 链接报错 `undefined reference` —— 当场抓住
- 插头 2 个（编了两个板子目录）→ 链接报错 `multiple definition` —— 当场抓住
- 恰好 1 个 → `GetInstance()` 首次调用时造出且只造出这块板

AGENTS.md"一次构建必须恰好一个 DECLARE_BOARD"不是道德要求，是**链接器的数学**。

**⑤ `override` —— 答题确认章**

写在子类函数上 = "我在回答父类合同里的某道虚函数题"。真正价值是**编译器验证**：父类必须真的存在签名一致的虚函数，否则报错。经典坑：

```cpp
class Animal { virtual void 叫(int 次数); };
class Dog : public Animal {
    void 叫();            // 忘传参数、没写 override → 编译器视为全新函数，不报错！
};
Animal* a = new Dog();
a->叫(3);                 // 执行的是 Animal 默认版本——Dog 以为自己答题了，实际答错卷子
                          // → 静默 bug，极难查
```

加 `override`：`void 叫() override;` → 编译器立刻报错"父类没有无参的叫()"。类比：答题卡必须涂题号，写错位置直接被撕卷打回。项目里 [wifi_board.h](main/boards/common/wifi_board.h) 的 `virtual void StartNetwork() override;` = "StartNetwork 必填题，我 WiFi 派来答"。

**五个词合起来讲一个故事：C++ 把大量本该"运行时才发现"的错误搬到"编译时"就地枪决**（delete / override / 链接器查重），同时用 virtual + 钩子把"写死"变成"可插拔"。

| 词汇       | 一句话                       | 防的坑                       |
| ---------- | ---------------------------- | ---------------------------- |
| `= delete` | 禁止拷贝                     | 多个"板子"对象抢硬件         |
| `virtual`  | 运行时按对象真实类型分发     | 核心代码永远拿到基类默认行为 |
| 钩子       | 框架定时机、你填内容         | 框架被迫写死具体实现         |
| 插槽       | 头文件留声明，板子给唯一定义 | 多板子打架 / 忘写板子        |
| `override` | "我在答题"，编译器验证       | 签名笔误静默答错题           |

### 自测三题（答案都在本节）

1. `GetAudioCodec()` 和 `GetDisplay()` 的声明差在哪？为什么声卡必填、屏幕选做？
2. `create_board()` 在 board.h 只有声明没有定义，链接时怎么不报错？
3. 新建一块 ESP32-C3 板需要动 application.cc 吗？动哪几个文件？

## 1.6 第 3 层：能力驱动（详细版，同一套思想的复读机）

1.6 的本质用一句话说：**1.5 学的"合同式抽象"思想，在能力层被复制了 N 遍**——每类器官都是一份新合同 + 多家答卷。

### 总起：同一套思想的复读机

```
抽象基类（合同，含 = 0 必填题 + 默认实现的选做题）
 └─ 多个实现类（各自 override 答题）
      └─ 板子构造函数里 new 出自己那份，通过 GetXxx() 交出去
```

屏幕、声卡、灯、按键、电池、相机——六份合同，一个套路。区别只是：有的合同必填题多，有的全是选做题（可选能力）。

### 屏幕 Display：最值得细看的器官

**合同原文**（[display.h L43-58](main/display/display.h#L43-L58)）：

```cpp
virtual void SetStatus(const char* status);      // 状态栏文字："正在连接…"
virtual void SetEmotion(const char* emotion);    // 表情："neutral" "happy"…
virtual void SetChatMessage(const char* role, const char* content);  // 对话字幕
virtual void ShowNotification(...);              // 弹出通知
virtual void UpdateStatusBar(...);               // 刷 WiFi/电量图标
virtual void SetupUI();                          // 摆家具（建 UI 对象）
```

合同里全是"选做题"气质（都有默认实现）——因为没有屏幕的板子也必须活着。无屏板怎么交卷？[display.h L128-132](main/display/display.h#L128-L132)：

```cpp
class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }   // "锁"直接给过
    virtual void Unlock() override {}                                  // "解锁"啥也不干
};
```

一份**空答卷**：核心代码照常喊 `SetStatus("聆听中…")`，喊给空气听，程序不崩。这就是"可选能力"在代码里的样子——用空实现代替 if 判断。

**你板子的答卷：OledDisplay 构造函数**（[oled_display.cc L23-88](main/display/oled_display.cc#L23-L88)）干四件事：

1. **挂字体**（L29-42）：把编译时选好的中文/图标/emoji 字体注册成 "dark" 主题——字体是 1.4 构建链里 CMakeLists 按板子定的
2. **启动 LVGL 移植层**（L44-51）：`lvgl_port_init`，开一个 LVGL 任务（优先级 1，双核芯片绑到核 1）——装修公司进场上工
3. **交房**（L54-80）：把 SSD1306 面板句柄交给 `lvgl_port_add_disp`，关键参数：`.monochrome = true`（单色屏 1bit/像素）、`.mirror_x/mirror_y`（config.h 里那两个镜像开关）、`.buff_dma = 1`（渲染缓冲用 DMA 内存）
4. 构造完**故意不建 UI**（L86-87 注释）：等 `Application::Initialize()` 统一调 `SetupUI()`——时机由管家定

**SetupUI 分发**（[L90-103](main/display/oled_display.cc#L90-L103)）：防重复调用（`setup_ui_called_` 标记）→ 按 `height_` 选布局：64 行走上下布局，**32 行走 `SetupUI_128x32()`（你的屏）**。`height_` 从 config.h 的 `DISPLAY_HEIGHT 32` 一路传来——1.4 配置链的终点站。

**重点新概念：DisplayLockGuard = RAII 锁**（[display.h L104-126](main/display/display.h#L104-L126)）

为什么需要锁：LVGL 不是线程安全的——LVGL 任务在渲染、主任务在 SetStatus、定时器在 UpdateStatusBar，同时改同一个 label = 布局损坏/崩溃。

RAII（Resource Acquisition Is Initialization）：**"拿锁"写进构造函数，"放锁"写进析构函数**：

```cpp
DisplayLockGuard lock(display);   // 构造 = 拿锁（最多等 30 秒）
display->SetStatus("聆听中…");     // 干活
}                                  // ← 变量出作用域，析构自动放锁！
```

好处：中间哪怕 return、抛异常、写 8 个出口，锁都**必然**释放——C++ 保证局部变量离开作用域时析构一定执行。类比：扫地机器人离开房间时自动关灯。传统"拿锁→干活→放锁"写法一中间 return 就死锁，RAII 把"记得放锁"这个人类弱点从代码里消灭了。

再留意一个呼应：[L118-119](main/display/display.h#L118-L119) `DisplayLockGuard(const DisplayLockGuard&) = delete;`——**锁本身也禁止复制**，否则复制出来的锁提前析构会把别人的锁放掉。1.5 的 `= delete` 在这里第二次上岗。

**屏幕数据流**：Application 改文字 → LVGL 改对象 → LVGL 任务渲染像素 → esp_lcd → I²C → SSD1306。

### 声卡 AudioCodec：小心名字陷阱

**合同原文**（[audio_codec.h](main/audio/audio_codec.h)）：

```cpp
// 公开的"服务台"方法（给音频管线用的）：
virtual void SetOutputVolume(int volume);             // L32 调音量
virtual void EnableInput(bool enable);                // L34 开/关麦克风通道
virtual void EnableOutput(bool enable);               // L35 开/关喇叭通道
virtual void OutputData(std::vector<int16_t>& data);  // L37 播：把 PCM 塞给喇叭
virtual bool InputData(std::vector<int16_t>& data);   // L38 录：从麦克风取 PCM

// 私有的纯虚函数（L67-68）——必填题，每份答卷必须实现：
virtual int Read(int16_t* dest, int samples) = 0;
virtual int Write(const int16_t* data, int samples) = 0;
```

名字陷阱：这里的 codec = **硬件声卡芯片**（ES8311 那种 I²C 芯片）的驱动；Opus 软件压缩是音频管线（1.7）的事。两码事。

**单工 vs 双工**（你板子是单工）：

- 双工版 [NoAudioCodecDuplex L33](main/audio/codecs/no_audio_codec.cc#L33)：`i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_)` —— 一条 I²S 总线（控制器0）同时创建收+发，麦克风喇叭共用总线
- 单工版（你的板子，[L79-94](main/audio/codecs/no_audio_codec.cc#L79-L94)）：`i2s_new_channel(&chan_cfg, &tx_handle_, nullptr)` 控制器0只给喇叭，再用另一条配置创建 rx（控制器1，专给麦克风）

对应 config.h 的 `AUDIO_I2S_METHOD_SIMPLEX` 和两组引脚（喇叭 7/15/16、麦克风 4/5/6）。面包板方案选单工：接线和时序简单，收发互不干扰；代价是全双工对话更难做，对 AEC 要求更苛刻。

老朋友出现：`dma_desc_num=6, dma_frame_num=240`（[L15-16](main/audio/audio_codec.h#L15-L16)）——embedded-concepts.md 讲的 DMA 环形缓冲参数；`output_volume_ = 70`（[L64](main/audio/audio_codec.h#L64)）——出厂默认音量，MCP 调音量工具最终改的就是它。

### 灯、按键、电池/相机（快讲）

- **LED**：合同核心就一个方法 `OnStateChanged(设备状态)`——状态机每次切状态都通知它，灯按状态变色（Idle 呼吸绿、Listening 蓝、Speaking 红…）。你的板子用 `SingleLed`（GPIO48 单色灯）；豪华板用 `CircularStrip`（WS2812 彩色灯环，能跑流光动画）
- **按键 Button**：自己不碰硬件，包装乐鑫 `iot_button` 组件（管 GPIO 中断、消抖、区分单击/双击/长按），对外提供 `OnClick/OnPress/OnRelease/OnLongPress` 四个"填空位"——1.5 讲的钩子。你板子在 InitializeButtons 里填了四个
- **电池/相机**：纯选做题。电池 = ADC 周期测电压换算百分比（或 AXP2101 电源芯片）；相机 = 给视觉模型拍照用。没有它们的板子交空答卷（GetBatteryLevel 默认返回 false）

### 本节收束：七份合同一览

| 器官   | 合同（基类）           | 必填题                  | 你的板子的答卷                                               |
| ------ | ---------------------- | ----------------------- | ------------------------------------------------------------ |
| 快递柜 | Board                  | —（纯虚接口层）         | CompactWifiBoard 的 GetXxx() 按需 new 各器官                 |
| 屏幕   | Display                | Lock/Unlock（私有纯虚） | OledDisplay（你的 128x32 OLED）/ NoDisplay（无屏板的空答卷） |
| 声卡   | AudioCodec             | Read/Write（私有纯虚）  | NoAudioCodecSimplex（无芯片直驱 I²S）                        |
| 灯     | Led                    | OnStateChanged          | SingleLed                                                    |
| 按键   | （组合在板子里）       | —                       | 四个 OnClick/OnPress 钩子                                    |
| 电池   | Board::GetBatteryLevel | —（选做）               | 未实现 → 默认"没有"                                          |
| 相机   | Board::GetCamera       | —（选做）               | 未实现 → 默认"没有"                                          |

一个发现：屏幕和声卡的"必填题"（Lock/Unlock、Read/Write）都是 **private 纯虚函数**——对外只暴露安全的高层方法，底层细节藏进私有区。合同设计的高级手法：**"你必须会做，但不许直接做，必须按我规定的方式做"**（Read/Write 只被 OutputData/InputData 调用，那里有缓冲和格式统一逻辑）。"快递柜"行就是抽象框架自身（1.5 的 Board）——它本身不干活，只负责把六个器官按需交付给核心代码。

## 1.7 第 4 层：音频管线（详细版，全项目最硬的一节）

前置知识全部就位：I²S/DMA（embedded-concepts.md 4/5 节）、采样率（6 节）、合同式抽象（1.5）、按铃模式。更细的逐站行号走读见 [audio-pipeline-walkthrough.md](audio-pipeline-walkthrough.md)，本节给完整骨架。

### 一、为什么需要"流水线"这个设计

先算账（概念文档 6 节的账）：16kHz × 16bit = **每秒 32KB 原始声音**。网络实时传不动，所以用 Opus 压缩；而压缩/解压是重计算，麦克风/喇叭又按毫秒节拍不停供货——"生产者"和"消费者"速度完全不同步。

**解法是工厂流水线**：把"采集 → 压缩 → 发送"拆成独立工位，工位之间用传送带（队列）衔接。每个工位只干一件事，忙不过来的活先在传送带上排队。

### 二、设计图就写在文件头注释里（[audio_service.h L28-38](main/audio/audio_service.h#L28-L38)，真实原文）

```cpp
/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use dedicated tasks for input, output, and Opus encoding/decoding.
 */
```

翻译：**上行**（你说话）和**下行**（AI 回答）两条流水线，共用一套任务。`[方括号]` = 干活的任务，`{花括号}` = 传送带（队列）。

### 三、三个任务 + 一个幕后大脑

| 任务                                                         | 干什么                                                                      | 类比     |
| ------------------------------------------------------------ | --------------------------------------------------------------------------- | -------- |
| `AudioInputTask`（[L214](main/audio/audio_service.h#L214)）  | 从麦克风（I²S+DMA）定时取 PCM，喂给音频引擎和编码队列                       | 收料员   |
| `OpusCodecTask`（[L216](main/audio/audio_service.h#L216)）   | **一个任务两份工**：编码队列的 PCM 压缩→发送队列；解码队列的包解压→播放队列 | 压缩车间 |
| `AudioOutputTask`（[L215](main/audio/audio_service.h#L215)） | 从播放队列取 PCM 写给喇叭（I²S+DMA）                                        | 发货员   |
| AFE 任务（藏在 AudioEngine 里）                              | 唤醒词检测 / VAD / AEC，跑在乐鑫 ESP-SR 框架自己的任务上                    | 安检员   |

### 四、五条队列——每个容量都是精心算的

真实声明（[L187-192](main/audio/audio_service.h#L187-L192)，`FixedQueue<类型, 容量>`）+ 宏（[L40-44](main/audio/audio_service.h#L40-L44)）：

| 队列     | 容量     | 怎么算的                        | 为什么是这个数                                    |
| -------- | -------- | ------------------------------- | ------------------------------------------------- |
| 编码队列 | **2**    | `MAX_ENCODE_TASKS_IN_QUEUE 2`   | PCM 大（60ms=1920字节），堆多了吃内存             |
| 发送队列 | **40**   | `2400 ÷ 60`                     | 攒 2.4 秒 Opus 包——网络抖一下不断粮，再多延迟超标 |
| 解码队列 | **20**   | `1200 ÷ 60`                     | 攒 1.2 秒下行——允许缓冲但不能攒太多               |
| 播放队列 | **2**    | `MAX_PLAYBACK_TASKS_IN_QUEUE 2` | 直接喂喇叭，满了就丢——宁丢不延                    |
| 测试队列 | **~166** | `10000 ÷ 60`                    | 音频测试模式整段录 10 秒再放，唯一"大肚"队列      |

**全项目最重要的取舍**：所有队列**有界**，满了**丢最旧的**。打电话偶尔丢一个字无所谓，但延迟 3 秒就没法对话——**实时性优先于完整性**。

### 五、上行走一遍（你说话）

```
① 麦克风 → I²S 总线（硬件节拍器）→ DMA 自动搬进内存
② AudioInputTask 每 10ms 取 160 个采样 → 喂给 AFE 引擎
③ AFE：降噪 + 判断有没有人说话（VAD）+ 监听"小智小智"（唤醒词）
④ 编码队列（容量2）→ OpusCodecTask 压成 60ms 一帧
⑤ 发送队列（容量40）
⑥ 队列有货 → 回调 on_send_queue_available 按铃（MAIN_EVENT_SEND_AUDIO）
⑦ 主任务醒来 → protocol->SendAudio() → 网络 → 服务器
```

注意 ⑥：音频任务**不自己发网络**——只按铃，发送永远由主任务做（1.5 的单线程决策原则）。

### 六、下行走一遍（AI 说话）

```
① 网络收包 → Protocol 回调 on_incoming_audio → 解码队列（容量20）
② OpusCodecTask 解压回 PCM
③ 播放队列（容量2）→ AudioOutputTask → I²S+DMA → 喇叭
④ 全部播完（队列排空）→ on_playback_drained 回调 → 主任务：状态 Speaking → Idle
```

细节：服务器 TTS 采样率可能不是 16k，解码器发现不一致会动态重建（`SetDecodeSampleRate`，[L219](main/audio/audio_service.h#L219)）——概念文档 6 节"两端照片尺寸不同，先裁剪再播放"的落点。

### 七、AFE 引擎：音频的"安检员"

`AudioEngine` 接口两个实现：AFE 引擎（全功能，基于乐鑫 ESP-SR）和 Lite 引擎（C3 等小芯片用）。AFE 在**本地（不联网）**持续做三件事：

- **唤醒词**：听到"小智小智" → `on_wake_word_detected` 按铃 → 主任务走 Idle→Connecting→Listening 流程
- **VAD**：判断你何时说完 → `on_vad_change` → 自动停止收音
- **AEC**：喇叭放音时消除麦克风里的回声——播放中能打断的根本原因

回调清单（[L82-90](main/audio/audio_service.h#L82-L90)，真实原文）——每个都是 `std::function`，**全是钩子**（1.5 的概念第三次上岗）：

```cpp
struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;              // 发送队列有货
    std::function<void(const std::string&)> on_wake_word_detected;  // 听到唤醒词
    std::function<void(bool)> on_vad_change;                        // 有人说话/说完了
    std::function<void(void)> on_playback_drained;                  // 下行播完了
    ...
};
```

Application 初始化时往这些钩子里填"按铃"代码——音频任务在任何线程触发它们都不会直接改应用状态。

### 八、和状态机的接线（详细版：函数级联动机制）

#### 8.1 先明确"谁认识谁"：单向依赖

音频引擎（AFE）里**没有一行代码引用状态机**（在 afe_audio_engine.cc 中 grep 不到 device_state）。它只会埋头处理声音、有事通过回调通知。真正同时认识两边的是 **Application（主任务）**，它在中间传话：

```
音频引擎（AFE）          Application/主任务           状态机
     │  "我检测到唤醒词了"      │                        │
     │ ────回调按铃──────────▶ │                        │
     │                        │ ──SetDeviceState──────▶│ 守门 + 记账
     │                        │ ◀────"换好了"──────────│
     │ ◀──EnableVoiceProcessing / EnableWakeWordDetection（主任务主动下命令）
```

这就是"音频管线开关由状态机驱动"的真正含义：**不是音频引擎去观察状态，而是主任务拿着新模式，显式地对音频服务下命令。**

#### 8.2 状态机自己只做两件事

设备任何时刻只能处于一种模式。状态机（[device_state_machine.cc](main/device_state_machine.cc)）的职责被严格限定为：

1. **记账**：原子变量 `current_state_` 记住当前模式（[L129](main/device_state_machine.cc#L129) `.store()`）
2. **守门**：`IsValidTransition()` 内置合法路线表（[L35-107](main/device_state_machine.cc#L35-L107)），非法跳转直接拒绝。核心路线：

```cpp
case kDeviceStateIdle:       // Idle → Connecting/Listening/Speaking/Notifying/Upgrading/WifiConfiguring
case kDeviceStateConnecting: // Connecting → Idle(失败) / Listening(成功)
case kDeviceStateListening:  // Listening → Speaking / Idle
case kDeviceStateSpeaking:   // Speaking → Listening(被打断) / Idle
```

#### 8.3 完整函数级调用链（唤醒词 → 进入听音，逐站看）

**第 1 站：音频世界按铃。** AFE 在自己的任务上检测到"小智小智" → 触发回调。Application 开机时往该回调里填的代码只有一句：

```cpp
xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);  // 只按铃，模式没变
```

**第 2 站：主任务要求换模式。** `Run()` 检测到铃 → 调 `SetDeviceState(kDeviceStateConnecting)`，进入 [application.cc L58](main/application.cc#L58)：

```cpp
bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);   // 原样委托给状态机
}
```

**第 3 站：状态机守门 + 记账**（[TransitionTo L113-136](main/device_state_machine.cc#L113-L136)）：

```cpp
if (!IsValidTransition(old_state, new_state)) return false;  // ① 守门：非法只警告、不改变
current_state_.store(new_state);                             // ② 记账：模式此刻才真正改变
NotifyStateChange(old_state, new_state);                     // ③ 通知所有监听者
```

**第 4 站：监听者仍然只按铃。** Application 在 [Initialize L96-97](main/application.cc#L96-L97) 注册的监听者：

```cpp
state_machine_.AddStateChangeListener([this](DeviceState, DeviceState) {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);   // 不直接动硬件
});
```

**第 5 站：主任务执行"新模式该干什么"**（[HandleStateChangedEvent L990-1064](main/application.cc#L990-L1064)）——真正的动作全部集中在这一个 switch 里：

```cpp
auto led = Board::GetInstance().GetLed();
led->OnStateChanged();                          // 灯先变色
switch (new_state) {
  case kDeviceStateIdle:
      display->SetStatus(Lang::Strings::STANDBY);
      audio_service_.EnableVoiceProcessing(false);      // 关语音处理
      audio_service_.EnableWakeWordDetection(true);    // 唤醒词重新值守
      break;
  case kDeviceStateConnecting:
      display->SetStatus(Lang::Strings::CONNECTING);    // 只改屏幕，音频不动
      break;
  case kDeviceStateListening:
      display->SetStatus(Lang::Strings::LISTENING);
      StartListeningAudio();                             // 见第 6 站
      break;
  case kDeviceStateSpeaking:
      display->SetStatus(Lang::Strings::SPEAKING);
      audio_service_.EnableVoiceProcessing(false);      // 停止编码上传
      audio_service_.EnableWakeWordDetection(           // AFE 唤醒词保持开启 → 播放中可打断
          audio_service_.IsAfeWakeWord());
      audio_service_.ResetDecoder();
      break;
}
```

**第 6 站：命令抵达音频服务内部**（[StartListeningAudio L1066-1084](main/application.cc#L1066-L1084)）：

```cpp
protocol_->SendStartListening(listening_mode_);   // 通知服务器
audio_service_.EnableVoiceProcessing(true);       // 开语音处理
ConfigureWakeWordForListening();                  // 按 Kconfig 决定听音时唤醒词开关
```

钻进音频服务（[audio_service.cc L661-732](main/audio/audio_service.cc#L661-L732)），这两个命令除了给 AFE 下指令，还翻转**内部事件位**：

| 命令                            | 翻转的位                                                                          | AFE 动作                        |
| ------------------------------- | --------------------------------------------------------------------------------- | ------------------------------- |
| `EnableVoiceProcessing(true)`   | 置 `AS_EVENT_AUDIO_PROCESSOR_RUNNING`（[L725](main/audio/audio_service.cc#L725)） | 开启语音处理通道 + ResetDecoder |
| `EnableWakeWordDetection(true)` | 置 `AS_EVENT_WAKE_WORD_RUNNING`（[L686](main/audio/audio_service.cc#L686)）       | WakeNet 开始检测                |

#### 8.4 两组事件位的分工（别混淆）

|        | `MAIN_EVENT_*`             | `AS_EVENT_*`                                                         |
| ------ | -------------------------- | -------------------------------------------------------------------- |
| 属于   | Application 主任务事件循环 | AudioService 音频服务内部                                            |
| 含义   | "发生了某事"的通知铃       | "音频引擎哪个模块正在运行"的状态标记                                 |
| 谁设置 | 各任务/回调                | EnableVoiceProcessing / EnableWakeWordDetection                      |
| 谁消费 | 主任务 `Run()`             | **audio_input 任务**——每 10ms 读出的麦克风数据往哪送，由这两个位决定 |

audio_input 的数据路由规则：`WAKE_WORD_RUNNING` 在 → 数据喂 WakeNet（待机值守靠它）；`AUDIO_PROCESSOR_RUNNING` 在 → 数据同时送去编码上传（听音时才需要）。所以模式控制最终落到实处的完整链条是：

```
主任务下命令 → 音频服务置 AS_EVENT_* 位 → audio_input 任务按位路由麦克风数据
```

#### 8.5 为什么必须绕这一圈（而不是 SetDeviceState 后直接干活）

1. **`SetDeviceState` 可能在任何任务里被调用**，但动屏幕、操作硬件、发网络这些副作用必须全部收敛在主任务一个线程串行执行，否则多任务同时操作共享资源会崩溃。所以状态机本身只做最轻量的事（改原子变量 + 按铃），重活全部排进主任务。
2. **职责分离**：状态机只管"模式合法不合法"，不管"进入模式要做什么"——后者集中在 HandleStateChangedEvent 一处，便于审查。

#### 8.6 一次对话的完整状态-音频联动时序

```
Idle（WAKE_WORD_RUNNING=1, AUDIO_PROCESSOR_RUNNING=0）
  │ ① 唤醒词 → MAIN_EVENT_WAKE_WORD_DETECTED
  │ ② Idle→Connecting（守门通过）→ MAIN_EVENT_STATE_CHANGED → 屏幕"正在连接…"，音频不变
  │ ③ 服务器就绪：Connecting→Listening → StartListeningAudio()
  │      → AUDIO_PROCESSOR_RUNNING=1：audio_input 开始把声音编码上传
  │ ④ 收到回答：Listening→Speaking → AUDIO_PROCESSOR_RUNNING=0（停上传）
  │      → AFE 唤醒词保持开启（为可打断）
  │ ⑤ 播放中再次唤醒：Speaking→Listening（打断）→ 回 ③
  │    或播完：MAIN_EVENT_PLAYBACK_DRAINED → Speaking→Idle
  │      → EnableWakeWordDetection(true)：WakeNet 重新值守
  └─ 回到 Idle
```

这就是 1.2 说的"想加新行为，先找状态切换点"——音频管线的开关全部由状态机经主任务驱动，没有一个模式判断 if 散落在音频引擎代码里。

**一句话收束**：音频管线 = 3 个专用任务 + 5 条有界队列组成的两条流水线 + 1 个本地"安检员"（AFE），全靠"按铃"和状态机沟通，铁律是**实时优先于完整、绝不阻塞**。

## 1.8 第 5 层：协议层（详细版，设备的"邮政系统"）

协议层管三件事：**说什么语言**（消息格式）、**怎么寄**（WebSocket 或 MQTT+UDP）、**收发怎么通知**（钩子回调）。两种实现的深入对照（握手/加密/防乒乓）见本文第五部分深挖。

### 一、信件与包裹：消息的两大类

**JSON 信件**（控制指令）：靠 `type` 字段区分——`hello`（握手）、`tts`（开始播/播完，驱动字幕）、`stt`（你的话识别成文字）、`llm`（改表情"思考中/开心"）、`mcp`（工具调用）、`goodbye`（道别）。

**二进制包裹**（音频），真实结构体（[protocol.h L19-33](main/protocols/protocol.h#L19-L33)）：

```cpp
struct BinaryProtocol2 {        // 版本2：包头带时间戳
    uint16_t version;
    uint16_t type;          // 0: OPUS, 1: JSON
    uint32_t reserved;
    uint32_t timestamp;     // 毫秒时间戳（用于服务器端 AEC）
    uint32_t payload_size;
    uint8_t payload[];
} __attribute__((packed));      // ← 禁止编译器加填充字节，逐字节对齐网络格式

struct BinaryProtocol3 {        // 版本3：精简头（1+1+2 字节），省带宽
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));
```

两个新词：`packed` = "别在字段间塞凑整的字节"——网络格式必须逐字节精确，多一个填充字节服务器就解析错位；`payload[]` = 柔性数组，包头后紧跟数据本体，整包一次分配。版本 2 的 `timestamp` 给**服务器端 AEC** 用。

### 二、合同本身：三层进阶手法（本节最大看点）

真实声明（[protocol.h L59-68](main/protocols/protocol.h#L59-L68)）：

```cpp
// 第一层：纯虚（=0）——"必填题，子类必须答"
virtual bool Start() = 0;
virtual bool OpenAudioChannel() = 0;
virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;

// 第二层：有默认实现——"选做题，基类替你答好了"
virtual void SendWakeWordDetected(const std::string& wake_word);
virtual void SendStartListening(ListeningMode mode);
virtual void SendStopListening();
virtual void SendAbortSpeaking(AbortReason reason);
virtual void SendMcpMessage(const std::string& message);

// 第三层（藏在 L85）：protected 纯虚——"最后一公里，你们自己想办法"
virtual bool SendText(const std::string& text) = 0;
```

**妙在哪**：5 个"选做"发送方法，基类已写好实现（protocol.cc）——**只负责把参数组装成正确的 JSON**，组装完调 `SendText()` 寄出。而 `SendText` 是纯虚——**怎么寄（WebSocket 还是 MQTT）留给子类**。于是 `SendAbortSpeaking()` 的 JSON 格式全项目只有一份代码，两种传输自动共享、永不写岔。合同设计进阶三部曲：

```
1.5 手法：必填题（=0）+ 选做题（默认实现）
1.6 手法：private 纯虚——"必须会做但不许直接做"
1.8 手法：公共逻辑上提基类，只把"传输差异"抽成一个小纯虚（SendText）
          —— JSON 组装代码写一遍，两种传输白享
```

和 1.5 继承链（WifiBoard 沉淀通用逻辑）是同一思想在协议层的投影。

### 三、接收端：钩子的第四五六次上岗

[protocol.h L51-57](main/protocols/protocol.h#L51-L57) 全是钩子注册器：`OnIncomingAudio`（收到音频包）、`OnIncomingJson`（收到信件）、`OnAudioChannelOpened/Closed`、`OnNetworkError`、`OnConnected/Disconnected`。子类收到东西调 `on_incoming_json_(root)`——Application 填的按铃代码执行。条件反射：**"组件定时机，应用填内容"**。

### 四、几个值得注意的成员

- `server_sample_rate_ = 24000`（[L79](main/protocols/protocol.h#L79)）：**默认按服务器 24k 准备**——握手时服务器告知真实采样率，即 1.7 的 `SetDecodeSampleRate` 动态重建的来源
- `last_incoming_time_` + `IsTimeout()`（[L83/L88](main/protocols/protocol.h#L83)）：通道健康检查——"最近还有数据吗？"，僵尸通道自动判死
- `AudioStreamPacket`（[L10-17](main/protocols/protocol.h#L10-L17)）：音频包裹在代码里的样子——除 payload 外带 `sample_rate/playback_id/media_position_ms`，下行播放进度上报靠后两个
- `ListeningMode`（[L37-41](main/protocols/protocol.h#L37-L41)）三种：`AutoStop`（VAD 自动判断说完）、`ManualStop`（松开按键才算说完）、`Realtime`（全双工随时插话，**注释写明"需要 AEC 支持"**——你的单工麦克风方案用不了这个模式）

### 五、两种实现 + 怎么选

```
OTA 检测（每次开机）→ 服务器下发配置写进 NVS
   ├─ 有 mqtt 配置   → MqttProtocol（4G 板 / 配了 MQTT 的 WiFi 板）
   └─ 否则 websocket → WebsocketProtocol（WiFi 板默认）
```

选择逻辑在 [application.cc L536-538](main/application.cc#L536-L538)，**MQTT 优先**。两种实现的对照（握手/加密/防乒乓）见第五部分深挖。

**AGENTS.md 铁律"改 Protocol 语义必须同时验证两种传输"的原理**：JSON 组装代码在基类共享，测两种传输 = 测"同一段组包代码的两个下游"。

## 1.9 第 6 层：应用核心（详细版）

整个 Application 类 = 一个单例 + 一个口袋 + 十四口铃 + 一个永不返回的循环。

### 一、为什么必须是单例

全设备只有一块屏、一个声卡、一个主循环——"当前会话"天然唯一。两个 Application 实例 = 两套协议、两个状态机抢硬件。真实实现（[application.h L49-55](main/application.h#L49-L55)）：

```cpp
static Application& GetInstance() {
    static Application instance;   // Meyers 单例：首次调用时构造，只构造一次
    return instance;
}
Application(const Application&) = delete;             // 1.5 防复制保险二次上岗
Application& operator=(const Application&) = delete;
```

Meyers 单例：C++11 保证多任务首次并发调用时初始化只执行一次（语言承诺，不用加锁）。构造函数 private（[L130](main/application.h#L130)）——外界连 `new` 都做不到。

### 二、口袋里装着什么（[application.h L133-155](main/application.h#L133-L155)）

| 成员             | 是什么                     | 对应前文     |
| ---------------- | -------------------------- | ------------ |
| `state_machine_` | 状态机对象                 | 1.7 第八部分 |
| `audio_service_` | 音频服务（3 任务 5 队列）  | 1.7          |
| `protocol_`      | unique_ptr，当前协议       | 1.8          |
| `ota_`           | OTA 对象                   | 第四部分深挖 |
| `event_group_`   | **十四口铃**（事件组）     | 本节主角     |
| `main_tasks_`    | deque，Schedule 的待办口袋 | 本节主角     |
| `notify_player_` | 通知播放器                 | 1.10         |

`protocol_` 用 `std::unique_ptr`——独占所有权，换协议时旧对象自动销毁。

### 三、十四口铃全表（真实宏 [L25-38](main/application.h#L25-L38)）

```cpp
#define MAIN_EVENT_SCHEDULE             (1 << 0)   // 有插队任务要执行
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)   // 发送队列有音频
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)   // 听到唤醒词
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)   // 有人开始/停止说话
#define MAIN_EVENT_ERROR                (1 << 4)   // 出错了
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)   // 激活完成
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)   // 1 秒滴答
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)   // 网络连上
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)   // 网络断了
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)   // 按键：开聊/结束
#define MAIN_EVENT_START_LISTENING      (1 << 10)  // 按键：开始听
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)  // 按键：停止听
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)  // 状态机换模式
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)  // 下行播完
```

来源四路：音频世界（1/2/3/13）、网络世界（7/8）、状态机（12）、按键+定时器（9/10/11/6）、其他（4/5/0）。**全项目所有异步事件最终汇聚到这十四个位。**

### 四、入口 app_main（[main.cc L14-29](main/main.cc#L14-L29) 真实原文）

```cpp
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();                 // 初始化 NVS（WiFi 配置存这）
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());           // 损坏就擦掉重来
        ret = nvs_flash_init();
    }
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // 永不返回
}
```

`extern "C"`：ESP-IDF 是 C 框架，入口必须以 C 方式命名，否则 C++ 名字修饰会让 bootloader 找不到。

### 五、Run()：永不返回的心跳（[application.cc L176-286](main/application.cc#L176-L286)）

**开场自抬身份**：

```cpp
vTaskPrioritySet(nullptr, 10);   // 主任务优先级 10，高于 audio_input(8)/output(4)/opus(2)
```

**骑在铃上睡觉**：

```cpp
while (true) {
    auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS,
                                    pdTRUE,   // 返回时清零等到的位（"看完销毁"）
                                    pdFALSE,  // 任意一位被置就醒
                                    portMAX_DELAY);  // 没事件就永久睡
```

三个参数正是"事件驱动"的精髓：**没事件 0% CPU 睡着；任意铃响即醒；干完铃自动复位**。不轮询、不费电。

**十四个 if 串行分发**（一次醒来可处理多个事件）：

```cpp
if (bits & MAIN_EVENT_ERROR)              { ...SetDeviceState(Idle); Alert(...); }
if (bits & MAIN_EVENT_NETWORK_CONNECTED)  { HandleNetworkConnectedEvent(); }
if (bits & MAIN_EVENT_STATE_CHANGED)      { HandleStateChangedEvent(); }
if (bits & MAIN_EVENT_SEND_AUDIO)         { ... }
if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) { HandleWakeWordDetectedEvent(); }
if (bits & MAIN_EVENT_CLOCK_TICK)         { ... }
// 共 14 个，顺序有讲究：错误最先，状态变化靠前，时钟滴答最后
```

**两个值得驻足的分支**：

`SEND_AUDIO`（[L239-251](main/application.cc#L239-L251)）——教科书级注释：

```cpp
while (auto packet = audio_service_.PopPacketFromSendQueue()) {
    if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
        // 发送失败：剩余包全丢！注释写明：留着会让 opus_codec 任务等队列空间，
        // 死锁整个音频输入管线，SEND_AUDIO 铃再也不会响
        while (audio_service_.PopPacketFromSendQueue());
        break;
    }
}
```

"实时优先于完整"在主任务的体现：网络断了**宁可全丢也不能堵**。

`PLAYBACK_DRAINED`（[L214-225](main/application.cc#L214-L225)）——一口铃两个用途：

```cpp
notify_player_.OnPlaybackDrained();      // 用途1：通知播放器收尾
if (pending_listening_start_ && ...IsPlaybackIdle()) {
    StartListeningAudio();               // 用途2：自动模式下等喇叭放完再开麦（防切字）
}
```

### 六、Schedule：带行李的铃（[application.cc L1162-1168](main/application.cc#L1162-L1168) 真实原文）

```cpp
void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);           // ① RAII 锁（第三次上岗）
        main_tasks_.push_back(std::move(callback));         // ② 函数塞进口袋
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);  // ③ 按铃
}
```

主任务对应处理（[L264-271](main/application.cc#L264-L271)）：

```cpp
if (bits & MAIN_EVENT_SCHEDULE) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto tasks = std::move(main_tasks_);   // 整袋搬走（顺便清空）
    lock.unlock();                          // 先放锁再执行！
    for (auto& task : tasks) task();
}
```

**按铃 vs Schedule**：按铃只喊"有事了"（主任务去状态里查）；Schedule 把**现成的函数**塞进口袋。使用者：MCP 工具执行、MCP 消息发送（[L1316-1326](main/application.cc#L1316-L1326) 注释 "Always schedule to run in main task for thread safety"）、按键/定时器回调。

**执行前先 `unlock()` 的原因**：任务内部可能再调 Schedule（很常见），拿着锁执行 → 自己等自己的锁 → 死锁。"锁只保护口袋，不保护执行"。

### 七、对外接口全是线程安全包装

[application.h L94-109](main/application.h#L94-L109)：`ToggleChatState()/StartListening()/StopListening()` 每个只按对应铃，注释写明 event-based, thread-safe——任何任务任何回调都能放心调，逻辑在主任务的 `HandleXxxEvent()` 里串行执行。

### 八、1 秒滴答（[L273-284](main/application.cc#L273-L284)）

```cpp
clock_ticks_++;
display->UpdateStatusBar();            // 每秒刷新 WiFi/电量图标
if (clock_ticks_ % 10 == 0) {
    SystemInfo::PrintHeapStats();      // 每 10 秒打堆内存统计（串口日志的来源）
}
```

### 九、附赠小工具 TaskPriorityReset（[application.h L192-204](main/application.h#L192-L204)）

```cpp
TaskPriorityReset(BaseType_t priority) {
    original_priority_ = uxTaskPriorityGet(NULL);   // 构造：记住当前优先级并临时改
    vTaskPrioritySet(NULL, priority);
}
~TaskPriorityReset() { vTaskPrioritySet(NULL, original_priority_); }  // 析构自动恢复
```

RAII 第四次上岗（同 DisplayLockGuard）：MCP 拍照工具"干活降优先级、干完自动还原"用的就是它。

### 自测三问

1. `xEventGroupWaitBits` 的 `pdTRUE` 改成 `pdFALSE` 会怎样？（位不复位 → 下轮重复处理同一事件 → 空转死循环）
2. 按键回调里直接调 `SetDeviceState(Listening)` 安全吗？（状态机本身原子安全；进状态的动作等 STATE_CHANGED 铃在主任务执行——这正是设计好的路径）
3. `Schedule` 执行任务前为何先 `unlock`？（防任务内部再调 Schedule 自己等自己的锁）

**一句话收束**：Application = Meyers 单例（防复制）+ 14 口事件铃（所有异步事件汇聚点）+ 一个"永不返回、睡在铃上、醒来串行分发"的 Run 循环 + Schedule 口袋（带行李的铃）。全设备所有决策只发生在这一个线程——这是整个固件稳定性的根基。

## 1.10 旁路子系统

| 模块       | 文件                                                    | 作用                                               |
| ---------- | ------------------------------------------------------- | -------------------------------------------------- |
| MCP 服务端 | [mcp_server.cc](main/mcp_server.cc)                     | 设备能力注册成"工具"，AI 远程调用（见第三部分）    |
| OTA        | [ota.cc](main/ota.cc)                                   | 双分区自升级（见第四部分）                         |
| 设置       | [settings.cc](main/settings.cc)                         | NVS 的 C++ 封装；键名是持久化 API，改名要迁移      |
| 通知播放   | [notify/notify_player.cc](main/notify/notify_player.cc) | 服务器推送的音频通知独立通道                       |
| 多语言资源 | [assets/](main/assets) + assets.cc                      | 39 语言提示音/字体；`lang_config.h` 是生成物别手改 |

## 1.11 终极串联："你说你好"的完整时间线

```
1 开机   app_main → NVS → Initialize 装配 → Run 事件循环（Starting）
2 联网   WiFi 读 NVS 连路由器（无密码→按键进 WifiConfiguring 配网）
3 连云   主任务建 WebSocket，握手报板型/UUID/能力；首次显示激活码（Activating）
4 待机   Idle：唤醒词模型本地持续跑；屏幕机器人表情；LED 呼吸
5 唤醒   "小智小智"→ WakeNet → 按铃 → Connecting → OpenAudioChannel → Listening
6 收音   麦克风 PCM → AFE → 60ms 帧 → Opus → send_queue → 主任务 SendAudio → 网络
7 断句   本地 VAD 检测停顿 → SendStopListening
8 云端   ASR→LLM→TTS；先 JSON（表情/字幕）后流式 Opus
9 播放   Speaking：收包→解码→播放队列→喇叭；字幕滚动
10 打断  播放中再喊唤醒词（AEC 消除喇叭回声）→ SendAbortSpeaking → 清播放队列 → Listening
11 结束  播放排空 → PLAYBACK_DRAINED → Idle
12 控设备 AI 决定调工具 → MCP JSON → 设备执行 → 结果回传（见第三部分）
```

## 1.12 建议读码路线（由易到难）

1. [main.cc](main/main.cc)（30 行入口）→ [device_state.h](main/device_state.h)（17 行状态）
2. [compact_wifi_board.cc](main/boards/bread-compact-wifi/compact_wifi_board.cc) + [config.h](main/boards/bread-compact-wifi/config.h)（硬件装配）→ [board.h](main/boards/common/board.h)（合同）
3. [oled_display.cc](main/display/oled_display.cc) 的 `SetupUI_128x32`（你的屏幕布局）
4. [application.cc](main/application.cc) 的 `Initialize()` → `Run()` 分发骨架 → 各 HandleXxx
5. [audio-pipeline-walkthrough.md](audio-pipeline-walkthrough.md) + [audio_service.h](main/audio/audio_service.h) 顶部注释
6. [protocol.h](main/protocols/protocol.h) + [websocket_protocol.cc](main/protocols/websocket_protocol.cc)
7. 官方文档：[main/audio/README.md](main/audio/README.md)、[docs/websocket.md](docs/websocket.md)、[docs/mcp-protocol.md](docs/mcp-protocol.md)

---

# 第二部分：实战走读——按 boot 键 → 开始听音

一句话总览：**按键本身只干一件事——按铃；剩下全是主任务串起来。**

```
[开机·埋线]  ① Board 构造 → InitializeButtons 注册回调
[触发·按键]  ② 按下 GPIO0 → iot_button 消抖识别"单击" → 执行回调
             ③ 回调调 ToggleChatState() → 只按一个铃 MAIN_EVENT_TOGGLE_CHAT
[主任务·决策] ④ Run() 醒来 → HandleToggleChatEvent() 按状态分流
             ⑤ Idle 分支 → SetDeviceState(Connecting) → 状态机校验 → 按 STATE_CHANGED 铃
             ⑥ HandleStateChangedEvent → LED 变色 + 屏幕显示"正在连接…"
             ⑦ Schedule 的 ContinueOpenAudioChannel → WebSocket 握手（带身份头）
             ⑧ 成功 → SetListeningMode → SetDeviceState(Listening) → 屏幕"聆听中…"
             ⑨ StartListeningAudio → EnableVoiceProcessing → 按下麦克风总闸事件位
                → audio_input 任务被唤醒 → 声音开始流向服务器
```

## 2.1 阶段 A：开机时按键怎么"埋线"

板子构造（[compact_wifi_board.cc L156-165](main/boards/bread-compact-wifi/compact_wifi_board.cc#L156-L165)）时 `boot_button_(BOOT_BUTTON_GPIO)` 创建于 GPIO0。[InitializeButtons L103-111](main/boards/bread-compact-wifi/compact_wifi_board.cc#L103-L111) 注册单击回调：

```cpp
boot_button_.OnClick([this]() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateStarting) {
        EnterWifiConfigMode();   // 开机阶段按 = 进配网
        return;
    }
    app.ToggleChatState();       // 平时按 = 开始/打断对话
});
```

`Button::OnClick`（[button.cc L83-94](main/boards/common/button.cc#L83-L94)）转交乐鑫 `iot_button` 组件：它负责 GPIO 中断、消抖、区分单击/双击/长按。此刻硬件中断已在等你的手指，没有任何轮询。

## 2.2 阶段 B：按下 → 铃响

`iot_button` 确认有效单击后执行回调——**注意回调运行在按键组件自己的上下文，不是主任务**。回调调 `ToggleChatState()`，全部实现一行（[application.cc L769](main/application.cc#L769)）：

```cpp
void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }
```

> **铁律**：别的任务/回调绝不直接改状态，只按铃。触摸键的 `StartListening/StopListening`（[L771-773](main/application.cc#L771-L773)）同理。

## 2.3 阶段 C：主任务决策

主任务睡在 [Run L187](main/application.cc#L187)，铃响醒来，[L227-229](main/application.cc#L227-L229) 命中 → `HandleToggleChatEvent()`（[L775-815](main/application.cc#L775-L815)）——**同一个键在不同状态含义不同**：

| 当前状态           | 按 boot =                | 位置                                      |
| ------------------ | ------------------------ | ----------------------------------------- |
| Notifying          | 停通知                   | [L778-781](main/application.cc#L778-L781) |
| Activating         | 回 Idle                  | [L783-785](main/application.cc#L783-L785) |
| WifiConfiguring    | 开录音自检（能听到自己） | [L786-789](main/application.cc#L786-L789) |
| **Idle（本场景）** | **开始对话**             | [L801-808](main/application.cc#L801-L808) |
| Speaking           | 打断 AI（AbortSpeaking） | [L810-811](main/application.cc#L810-L811) |
| Listening          | 结束对话                 | [L812-814](main/application.cc#L812-L814) |

Idle 分支核心代码：

```cpp
if (state == kDeviceStateIdle) {
    ListeningMode mode = GetDefaultListeningMode();   // AEC关 → AutoStop 自动断句
    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);       // ① 先切状态
        // Schedule to let the state change be processed first (UI update)
        Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });  // ② 再排队连接
        return;
    }
    SetListeningMode(mode);   // 通道已开就直接听
}
```

`SetDeviceState`（[L58](main/application.cc#L58)）→ 状态机校验 Idle→Connecting 合法 → 监听器按 `STATE_CHANGED` 铃 → 主循环 [L210-212](main/application.cc#L210-L212) → `HandleStateChangedEvent()`（[L989](main/application.cc#L989)）：

- [L999](main/application.cc#L999) `led->OnStateChanged()` LED 变色
- Connecting 分支（[L1016-1020](main/application.cc#L1016-L1020)）：屏幕"正在连接…" + neutral 表情 + 清字幕 → LVGL → I²C → OLED

> **Schedule 的精妙**（[L805 注释](main/application.cc#L805)）：WebSocket 握手阻塞几百毫秒。若原地连接，"正在连接…"要等握完手才显示。先 Schedule 让 UI 事件插队跑完，下一轮循环才真连——"不阻塞主循环"的教科书示范。

## 2.4 阶段 D：真正连接服务器

下一轮循环处理 `MAIN_EVENT_SCHEDULE`（[L264-271](main/application.cc#L264-L271)）→ 执行 [ContinueOpenAudioChannel L817-837](main/application.cc#L817-L837)：

```cpp
if (GetDeviceState() != kDeviceStateConnecting) return;  // 防竞态：排队期间状态可能变了
board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);    // 切高性能，降延迟
if (!protocol_->OpenAudioChannel()) {
    SetDeviceState(kDeviceStateIdle);                    // 失败回待命，不卡死
    return;
}
SetListeningMode(mode);                                  // 成功 → Listening
```

`WebsocketProtocol::OpenAudioChannel()`（[websocket_protocol.cc L79](main/protocols/websocket_protocol.cc#L79)）：从 NVS 读 URL/token/版本（[L80-86](main/protocols/websocket_protocol.cc#L80-L86)）→ 创建连接（[L91](main/protocols/websocket_protocol.cc#L91)）→ 带**身份三件套**请求头（[L97-106](main/protocols/websocket_protocol.cc#L97-L106)）：`Authorization: Bearer <token>`、`Device-Id: <MAC>`、`Client-Id: <UUID>` → 注册 `OnData`（[L108](main/protocols/websocket_protocol.cc#L108)，将来 AI 的回答从这里进）→ hello 握手上报能力。

## 2.5 阶段 E：进入听音（"开始听音"的真正开关）

`SetListeningMode`（[L1178-1181](main/application.cc#L1178-L1181)）→ `SetDeviceState(Listening)` → STATE_CHANGED 铃 → Listening 分支（[L1021-1039](main/application.cc#L1021-L1039)）：

- 屏幕"聆听中…"
- 处理器没在跑 → `StartListeningAudio()`（[L1034](main/application.cc#L1034)）；特例：auto 模式下播放队列还有存货 → 挂起 `pending_listening_start_` 等 `PLAYBACK_DRAINED` 再启动（[L1031-1032](main/application.cc#L1031-L1032)），防止 AI 最后一句被截断

[StartListeningAudio L1065-1083](main/application.cc#L1065-L1083) 三连：

```cpp
protocol_->SendStartListening(listening_mode_);  // 告诉服务器："我开始说了"
audio_service_.EnableVoiceProcessing(true);      // 打开本地上行链路
ConfigureWakeWordForListening();                 // 按 Kconfig 决定听音时是否保留唤醒检测
```

`AudioService::EnableVoiceProcessing(true)`（[audio_service.cc L709-732](main/audio/audio_service.cc#L709-L732)）最关键一行是 [L725](main/audio/audio_service.cc#L725)：

```cpp
xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);  // ← 麦克风的"总闸"
```

audio_input 任务睡在 [L243-250](main/audio/audio_service.cc#L243-L250) 等这个位；此刻被唤醒进入采集循环（[L309-317](main/audio/audio_service.cc#L309-L317)）：每 10ms 读 160 采样 → AFE → 60ms 帧 → Opus → send_queue → `MAIN_EVENT_SEND_AUDIO` 铃 → 主任务 `SendAudio` → 服务器听到你了。屏幕"聆听中…"与 LED 颜色早已就位。**链路闭环。**

## 2.6 走读后的四条"为什么"

| 设计                                             | 为什么                                             |
| ------------------------------------------------ | -------------------------------------------------- |
| 按键回调只按铃（L769 一行）                      | 回调在按键组件上下文，改状态必须留给主任务串行做   |
| 切状态后先 Schedule 再连接（L805）               | 握手阻塞几百 ms，不能挡住 STATE_CHANGED 的 UI 更新 |
| ContinueOpenAudioChannel 开头再验状态（L819）    | 排队期间世界可能变了——"先检查再执行"防竞态         |
| EnableVoiceProcessing 靠事件位开关麦克风（L725） | 输入任务平时深度睡眠，用"闸门"控制省电             |

**课后练习**：自己追一遍"播放中再按 boot 键"→ [L810-811](main/application.cc#L810-L811) → `AbortSpeaking`（[L1170](main/application.cc#L1170)）→ `SendAbortSpeaking` → 服务器回 JSON 让设备回 Listening，与本文链路几乎对称。

---

# 第三部分：深挖 1/3——MCP 服务器

## 3.1 MCP 是什么

MCP（Model Context Protocol）= "AI 如何调用外部工具"的公开标准，基于 **JSON-RPC 2.0**：

```json
{"jsonrpc":"2.0", "id":1, "method":"tools/call", "params":{"name":"xxx", "arguments":{}}}
```

**小智的反转**：按 MCP 术语，**设备（ESP32）= MCP 服务器**（提供工具），**云端大模型 = MCP 客户端**（决定何时调用）。你说"开灯"，AI 理解后远程遥控你的板子。

## 3.2 一个"工具"的三要素

[McpTool（mcp_server.h L260-277）](main/mcp_server.h#L260-L277)：

| 要素           | 作用                                                                                                          | 给谁看                            |
| -------------- | ------------------------------------------------------------------------------------------------------------- | --------------------------------- |
| `name`         | 唯一名字，如 `self.lamp.turn_on`                                                                              | 程序按名分发                      |
| `description`  | 自然语言描述"干什么、何时用"                                                                                  | **大模型**（AI 全靠它决定用不用） |
| `PropertyList` | 参数类型/默认值/范围 → 自动转 JSON Schema（[to_json L279-347](main/mcp_server.h#L279-L347) 的 `inputSchema`） | 大模型                            |
| `callback_`    | 真正干活的 C++ 函数                                                                                           | 设备本地                          |

返回值 `ReturnValue` 是变体（[L57](main/mcp_server.h#L57)）：`bool/int/string/cJSON*/ImageContent*`，统一包装成 MCP `content` 返回。摄像头工具能返回 base64 图片（[ImageContent L19-52](main/mcp_server.h#L19-L52)），让 AI 真"看照片"。

## 3.3 工具注册的三类来源

挂在单例 [McpServer::GetInstance（L423-429）](main/mcp_server.h#L423-L429)：

**① 通用工具**（[AddCommonTools mcp_server.cc L27-121](main/mcp_server.cc#L27-L121)）：

- `self.get_device_status` 查状态（[L39-49](main/mcp_server.cc#L39-L49)）
- `self.audio_speaker.set_volume` 调音量，参数 0~100（[L54](main/mcp_server.cc#L54)）
- `self.screen.set_brightness`——**只有板子有背光才注册**（[L61-71](main/mcp_server.cc#L61-L71) 的 `if (backlight)`，你的 OLED 没有）
- `self.screen.set_theme` / `self.camera.take_photo` 同样按硬件条件注册

> 常用工具放列表最前（[L28-30 注释](main/mcp_server.cc#L28-L30)）：利用大模型 prompt cache 提升响应速度。

**② 板级工具**——你的开灯工具。板子 [InitializeTools L150-153](main/boards/bread-compact-wifi/compact_wifi_board.cc#L150-L153)：

```cpp
void InitializeTools() {
    static LampController lamp(LAMP_GPIO);   // static：构造一次，注册永久
}
```

[LampController（lamp_controller.h L13-44）](main/boards/common/lamp_controller.h#L13-L44) 注册 3 个工具（get_state / turn_on / turn_off）：

```cpp
mcp_server.AddTool("self.lamp.turn_on", "Turn on the lamp", PropertyList(),
    [this](const PropertyList& properties) -> ReturnValue {
        power_ = true;
        gpio_set_level(gpio_num_, 1);   // ← 真正拉高 GPIO，灯亮
        return true;
    });
```

> **加一个 AI 可控的新功能只需三步**：写个类、构造里 `AddTool`、板子 `InitializeTools` 里 `static` 一个实例。核心代码零改动。

**③ 仅用户可见工具**（[AddUserOnlyTools L123](main/mcp_server.cc#L123)，如查系统信息）：打上 `annotations.audience=["user"]`（[mcp_server.h L325-343](main/mcp_server.h#L325-L343)），**AI 看不见**，只出现在 App/网页——防 AI 乱调危险工具。

## 3.4 设备如何声明 MCP 能力

WebSocket 握手 hello 消息里带（[websocket_protocol.cc L202-209](main/protocols/websocket_protocol.cc#L202-L209)）：

```cpp
cJSON_AddBoolToObject(features, "mcp", true);   // ← 声明：我是 MCP 服务器
```

服务器随后可发 `initialize` / `tools/list` 拿完整工具清单，把描述塞进大模型系统提示词——**AI 从此知道这台设备能做什么**。

## 3.5 终极串联：你说"把灯打开"的完整旅程

```
你："把灯打开"
 ↓ 麦克风→Opus→WebSocket（上行链路）
云端：ASR→LLM 思考→决定调用 self.lamp.turn_on
 ↓ 服务器发 {"type":"mcp","payload":{jsonrpc:2.0,id:1,method:"tools/call",params:{...}}}
设备网络任务收到 → OnIncomingJson 的 "mcp" 分支
   [application.cc L676-680] → McpServer::ParseMessage(payload)
 ↓
ParseMessage 校验 JSON-RPC 版本/method/id [mcp_server.cc L357-390]
   → 命中 "tools/call" 分支 [L420-439]
 ↓
DoToolCall [L540-601]：
  ① 按名找工具，找不到 → ReplyError(-32602) [L546-550]
  ② 逐个校验参数：类型/范围（Property::set_value [mcp_server.h L163-171]，
     AI 传 150 的音量会被直接拒绝）
  ③ 缺必填参数 → 报错 [L582-587]
  ④ 通过 → app.Schedule(把 tool->Call 排进主任务) [L592-601]
 ↓
主任务执行 LampController 回调 → gpio_set_level(GPIO,1) → 💡灯亮
 ↓
ReplyResult [L454-461] → SendResponse [L446-452]
   → Application::SendMcpMessage [application.cc L1316-1326]（再 Schedule 一次保证线程安全）
   → protocol_->SendMcpMessage
 ↓
云端收到结果 → TTS 回答"好的，灯已经打开啦" → 下行音频链路 → 喇叭
```

**两个线程安全细节**：工具回调 `Schedule` 进主任务执行（[L590-601](main/mcp_server.cc#L590-L601)），因为要操作 GPIO/声卡等共享资源；摄像头工具例外地降优先级 `TaskPriorityReset priority_reset(1)`（[L103](main/mcp_server.cc#L103)）——拍照计算重，别卡主任务。

**错误码**遵循 JSON-RPC 标准：`-32601` 方法不存在（[L442](main/mcp_server.cc#L442)）、`-32602` 参数非法、`-32603` 内部错误。

---

# 第四部分：深挖 2/3——OTA 升级机制

## 4.1 解决什么问题

OTA = 不插线，设备自己下载安装新固件。核心难题：**不能往正在运行的固件身上写**。解法 = **双分区**：

> Flash 里两块一样大的地盘（ota_0/ota_1），A 区运行时新固件写进 B 区，写完改"下次启动去 B 区"的标记，重启切换。旧 A 区完好——新固件有问题就退回。

## 4.2 真实的 Flash 分区地图

16MB Flash，分区表 [partitions/v2/16m.csv](partitions/v2/16m.csv)（由 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 指定，见 build/config/sdkconfig.h L402-404）：

```
0x9000   nvs        16KB   ← 配置仓库（WiFi/音量/OTA URL…）
0xd000   otadata     8KB   ← 关键！"下次启动从 ota_0 还是 ota_1"的指示牌
0xf000   phy_init    4KB   ← 射频校准参数
0x20000  ota_0     ~3.9MB  ← 固件 A 区
         ota_1     ~3.9MB  ← 固件 B 区（OTA 时新固件写这里）
0x800000 assets       8MB  ← 资源区（字体/提示音，独立升级通道）
```

## 4.3 阶段 1：版本检测（开机联网后自动跑）

网络连上后 `ActivationTask` 调 [CheckNewVersion（application.cc L372 → L441）](main/application.cc#L441)。

**① 带身份去问**（[Ota::CheckVersion L77-113](main/ota.cc#L77-L113)，请求头 [SetupHttp L55-72](main/ota.cc#L55-L72)）：

- `Device-Id`（MAC）、`Client-Id`（UUID）
- **`User-Agent: <板子名>/<固件版本> (<芯片型号>)`**——服务器靠它知道"哪块板子、什么版本"，决定推哪个固件包（第 4.6 节要考）

**② 响应一次带回 5 大块**（[L115-244](main/ota.cc#L115-L244)），不止查版本：

| 字段                 | 干什么                                                                                  | 代码                              |
| -------------------- | --------------------------------------------------------------------------------------- | --------------------------------- |
| `activation`         | 激活码/挑战（首次绑定）                                                                 | [L127-147](main/ota.cc#L127-L147) |
| `mqtt` / `websocket` | **协议服务器地址/密钥写进 NVS**——`InitializeProtocol` 里 `ota_->HasMqttConfig()` 的来源 | [L149-189](main/ota.cc#L149-L189) |
| `server_time`        | 校准系统时钟 settimeofday                                                               | [L192-214](main/ota.cc#L192-L214) |
| `firmware`           | 新版本号 + 下载 URL                                                                     | [L217-244](main/ota.cc#L217-L244) |

**③ 版本比较**：`"1.2.3"` 按点拆数组逐位比（[IsNewVersionAvailable L415-428](main/ota.cc#L415-L428)）；服务器可带 `force:1` 强制安装（[L237-240](main/ota.cc#L237-L240)）。

**④ 失败重试**：最多 10 次，指数退避 10s→20s→40s…（[L442-487](main/application.cc#L442-L487)）。

## 4.4 阶段 2：下载与写入

[UpgradeFirmware（application.cc L1202-1259）](main/application.cc#L1202-L1259) 先打扫战场：

```
关通知 → 关音频通道（L1214-1217）→ 屏幕弹"升级中" → 状态切 Upgrading（L1224）
→ 切 PERFORMANCE → audio_service_.Stop() 停掉 4 个音频任务（L1230）
```

核心 [Ota::Upgrade（L270-396）](main/ota.cc#L270-L396) 四步：

```cpp
① esp_ota_get_next_update_partition(NULL);   // L273 "另一个"分区 → 写 B 区不碰自己
② 循环下载：每次 4KB（L306）→ esp_ota_write（L360）
   - 边下边写，4KB 缓冲用内部 RAM（L307）
   - 每秒算进度/速度 → 回调 → Schedule → 屏幕字幕"37% 210KB/s"（L333/L1236）
   - 先校验镜像头（esp_image_header_t，L339-355）确认合法固件再写
③ esp_ota_end(update_handle);                // L378 结束并校验整包完整性
④ esp_ota_set_boot_partition(update_partition); // L388 把 otadata 指到 B 区
```

**成功** → 屏显"升级成功" → `Reboot()` → Bootloader 读 otadata → 从 B 区启动。
**失败** → 不重启！重启音频服务继续正常工作（[L1241-1250](main/application.cc#L1241-L1250)）——旧固件毫发无损。

手动入口：用户专属 MCP 工具 `self.upgrade_firmware`（[mcp_server.cc L144-163](main/mcp_server.cc#L144-L163)），App/网页点"升级"走它。

## 4.5 阶段 3：新固件"自证清白"（自动回滚保险）

新固件首启处于 `PENDING_VERIFY`。[MarkCurrentVersionValid（L250-268）](main/ota.cc#L250-L268)：

```cpp
if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();   // "我跑起来了，别回滚我"
}
```

若新固件反复崩溃、没来得及自我确认 → Bootloader 下次**自动回滚旧分区**。设备不会因一次失败升级变砖。

## 4.6 为什么"永远不许改现有板子的引脚"

AGENTS.md 这条规则的 **OTA 逻辑**根源：

- 固件 User-Agent 带板子名 → 服务器据此匹配固件包；
- 悄悄改某块现有板子的引脚但名字不变 → 服务器继续推老定位的固件 → OTA 后**用户硬件直接失聪/失明**；
- 所以加硬件变体必须**新建唯一命名的板子/变体**（改 config.json、Kconfig、CMakeLists 整条链），而不是魔改旧板子。**board identity affects OTA compatibility。**

## 4.7 assets 独立升级通道

[CheckAssetsVersion（application.cc L381-439）](main/application.cc#L381-L439)：NVS 里有资源包地址就走 `assets.Download()` 写进 `assets` 分区（8MB），不占固件双分区——新增语言提示音不用重编固件。

---

# 第五部分：深挖 3/3——MQTT+UDP 协议

## 5.1 为什么拆成两条通道

WebSocket 在 WiFi 下舒服，但 4G Cat.1 模组（ML307 类板子）不行：

- TCP 长连接在蜂窝网里脆：基站切换/信号抖动断流，重连 TCP+TLS 要好几秒
- TCP 的"可靠性"是负担：丢一个音频包，后面全部卡住等重传（队头阻塞）——对实时语音，迟到 500ms 的包等于垃圾

于是按需求拆分交通工具：

| 需求                                 | 特点                         | 通道                      | 类比   |
| ------------------------------------ | ---------------------------- | ------------------------- | ------ |
| **信令**（hello/再见/表情/字幕/MCP） | 量小、**必须可靠到达**       | **MQTT**（TCP，TLS 8883） | 挂号信 |
| **音频**（Opus 包）                  | 量大、**必须快**、**允许丢** | **UDP**（裸 UDP + AES）   | 电话   |

## 5.2 配置从哪来、选谁

OTA 响应的 `mqtt` 字段（[ota.cc L149-168](main/ota.cc#L149-L168)）下发 endpoint/client_id/username/password/publish_topic 存进 NVS。选择逻辑（[application.cc L536-543](main/application.cc#L536-L543)）：

```cpp
if (ota_->HasMqttConfig())        protocol_ = std::make_unique<MqttProtocol>();     // MQTT 优先
else if (ota_->HasWebsocketConfig()) protocol_ = std::make_unique<WebsocketProtocol>();
```

**服务器给了 MQTT 配置就用 MQTT**。选择对上层透明，Application 只认 `Protocol*`。

## 5.3 阶段 A：MQTT 客户端常驻（[StartMqttClient L72-178](main/protocols/mqtt_protocol.cc#L72-L178)）

与 WebSocket"用时才连"不同，MQTT **开机常驻**：

- 从 NVS 读五件套（[L78-84](main/protocols/mqtt_protocol.cc#L78-L84)），默认端口 **8883（TLS）**
- **断线自愈**（[L98-112](main/protocols/mqtt_protocol.cc#L98-L112)）：断线 → 60 秒一次性定时器 → 到点且设备空闲时 Schedule 重连；连上 → 停定时器
- `OnMessage`（[L114-147](main/protocols/mqtt_protocol.cc#L114-L147)）是收件箱：`hello` → 握手；`goodbye` → 关会话；**其余 JSON → `on_incoming_json_`**——"mcp 改屏幕""tts/start 切状态"等处理与 WebSocket **完全共用**，这就是 Protocol 基类抽象的价值

## 5.4 阶段 B：打开音频通道 = 申请加密 UDP 管道（[OpenAudioChannel L251-362](main/protocols/mqtt_protocol.cc#L251-L362)）

```
① MQTT 没连就先连（L252-257）
② 经 MQTT 发 hello（GetHelloMessage L364-388）：
   {"type":"hello","version":3,"transport":"udp","features":{"mcp":true},
    "audio_params":{"format":"opus","sample_rate":16000,...}}
   —— "transport":"udp" = 向服务器申请 UDP 通道
③ 等铃：xEventGroupWaitBits(SERVER_HELLO, 10 秒超时)（L269-275）
④ ParseServerHello（L390-480）拆服务器 hello：
   - session_id（本次对话凭证）
   - 服务器 audio_params（采样率可不同 → SetDecodeSampleRate 动态重建解码器）
   - udp: {server, port, key, nonce} ← 加密材料交接仪式
     · key/nonce 是 hex 字符串，DecodeHexString 解成 16 字节（L438-443）
     · AES-128 密钥导入 PSA 安全加密库拿句柄 aes_key_id_（L458-464）
       ——密钥不裸奔在普通内存，析构时 psa_destroy_key 销毁（L57-63）
⑤ 创建 UDP、注册收包回调、Connect（L277-356）→ 通道就绪
```

## 5.5 阶段 C：发音频 = 16 字节头当加密盐（[SendAudio L192-224](main/protocols/mqtt_protocol.cc#L192-L224)）

每个 UDP 包：

```
|type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|  ← 16字节明文头
| Opus 数据（AES-CTR 加密） |
```

最巧的设计（[L204-218](main/protocols/mqtt_protocol.cc#L204-L218)）：**这 16 字节头本身被当作 AES-CTR 的 nonce**——每次发包把 `payload_len/timestamp/sequence` 填进 nonce 副本再用它加密。sequence 每包递增 → **nonce 每包必然不同** → 满足 CTR 模式安全前提（nonce 绝不能重复）。安全与工程在一个结构里同时解决。

## 5.6 阶段 D：收音频 = 解密 + 序号去重（[OnMessage L279-346](main/protocols/mqtt_protocol.cc#L279-L346)）

```
校验 type/长度 → 序号检查（L309-320）：
   sequence <= remote_sequence_  → 重复/过时包直接扔（UDP 世界的日常）
   sequence != remote_sequence_+1 → 乱序警告，但照收
→ 同样的 16 字节头当 nonce 做 AES-CTR 解密（L330-334）
→ 更新 remote_sequence_ → on_incoming_audio_ 交给解码队列
```

**乱序不等待、重复不处理**——实时优先，与 TCP 哲学相反。

## 5.7 会话管理与两个工程亮点

- **goodbye 防乒乓**：设备关通道发 goodbye（[L238-244](main/protocols/mqtt_protocol.cc#L238-L244)）；收到**服务器先发的** goodbye 时 `CloseAudioChannel(false)` 不再回敬（[L137-138](main/protocols/mqtt_protocol.cc#L137-L138)）——否则两边互相道别死循环
- **通道健康判断**（[IsAudioChannelOpened L543-546](main/protocols/mqtt_protocol.cc#L543-L546)）：UDP 存在 && 没出错 && **最近有数据**（基类 `IsTimeout()` 跟踪 `last_incoming_time_`）——僵尸通道自动判死
- **对象生命周期护身符**：`alive_` 是 `shared_ptr<atomic<bool>>`（[mqtt_protocol.h L39](main/protocols/mqtt_protocol.h#L39)），定时器/回调捕获副本；析构先置 false，排队的 Schedule 回调醒来一看"已死"就什么都不做——优雅解决"对象销毁后异步回调还引用它"的经典崩溃，值得抄走

## 5.8 与 WebSocket 全面对照

|            | WebSocket                   | MQTT+UDP                         |
| ---------- | --------------------------- | -------------------------------- |
| 信令通道   | 同一条 TCP                  | MQTT（TLS 8883，常驻+自动重连）  |
| 音频通道   | 同一条 TCP                  | 独立 UDP（AES-CTR 加密）         |
| 音频丢包   | 队头阻塞，全线卡            | 丢弃继续，只损一帧               |
| 音频加密   | TLS 管全部                  | 应用层 AES-128-CTR（头即 nonce） |
| 音频完整性 | TCP 保证                    | sequence 去重/乱序检测           |
| 会话建立   | HTTP 升级 + hello           | MQTT hello → 服务器分配 UDP+密钥 |
| 断线恢复   | 整条重连                    | MQTT 自动重连；UDP 每次对话重开  |
| 适用场景   | WiFi 稳定网络               | 4G Cat.1 / 弱网 / 省流量         |
| 共享代码   | `Protocol` 基类全部消息语义 | 同左                             |

这就是 AGENTS.md 说"改 Protocol 共享语义必须同时验证两种传输"的原因：**差异只该存在于"怎么搬运"，绝不该渗入"搬什么"**。

---

# 第六部分：设计母题总结

掌握的全部核心链路：

1. **总纲**：七层架构、Board 抽象、构建链、状态机、事件驱动（第一、二部分）
2. **音频管线**：4 任务 5 队列、AFE、背压丢帧（[audio-pipeline-walkthrough.md](audio-pipeline-walkthrough.md)）
3. **按键走读**：GPIO 中断 → 麦克风开闸的 9 站链（第二部分）
4. **MCP**：设备即服务器，工具注册 + JSON-RPC + 主任务执行（第三部分）
5. **OTA**：双分区换鞋 + 自证清白 + 板子身份即兼容性（第四部分）
6. **MQTT+UDP**：信令走 MQTT、音频走加密 UDP（第五部分）

贯穿始终的**四个设计母题**：

| 母题                      | 体现                                                        |
| ------------------------- | ----------------------------------------------------------- |
| **接口与实现分离**        | Board / Display / AudioCodec / Protocol——换硬件不改核心     |
| **事件驱动 + 单线程决策** | 回调只按铃；主任务串行处理；Schedule 插队；先检查再执行     |
| **实时优先于完整**        | 有界队列、丢旧帧、乱序不等待、僵尸通道自动判死              |
| **身份即契约**            | 板子名/UUID/MAC 决定 OTA 匹配、激活、服务器匹配——永远别撒谎 |
