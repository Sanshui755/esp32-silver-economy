#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp32_camera.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>  // IWYU pragma: keep (条件编译的跌倒/久坐/离床周期任务启用时使用)
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
     (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C, 0x04, 0x12, 0x14,
                 0x1f},
     14, 0},
    {0xf1,
     (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D, 0x0C, 0x1A, 0x14,
                 0x1E},
     14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif

#define TAG "CompactWifiBoardS3Cam"

// ------------------------------------------------------------------
// 银发告警状态机：弹窗 / 大音量 / 补声 / 屏幕保持 一体化管理。
// 为什么需要"屏幕保持"：状态机切回 Idle/Connecting 时会清空聊天消息
// （application.cc 的 SetDeviceState 处理），告警文本可能一闪而过，
// 老人还没看清就没了。因此每次补声的同时重写屏幕，补声结束后再持续
// 重写几次；若期间开始了新对话，新消息会自然覆盖告警。
// ------------------------------------------------------------------
struct ElderAlertState {
    std::string title;
    std::string message;
    std::string emotion;
    const std::string_view* sound;
    int beeps_left;      // 还需补播的次数
    int reasserts_left;  // 补声结束后继续重写屏幕的次数
    int interval_ms;
};

static void ElderAlertFire(void* arg);

// 下一跳定时：把 state 挂到新的一次性 esp_timer 上
static void ElderAlertScheduleNext(ElderAlertState* state, int delay_ms) {
    esp_timer_create_args_t args = {};
    args.callback = ElderAlertFire;
    args.arg = state;  // esp_timer 不自动传参，漏设即 nullptr 崩溃
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "elder_alert";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
    } else {
        delete state;  // 无法继续调度时释放，避免泄漏
    }
}

// esp_timer 回调（ESP_TIMER_TASK 上下文）：UI/音频操作必须 Schedule 回主任务
static void ElderAlertFire(void* arg) {
    auto* state = static_cast<ElderAlertState*>(arg);
    bool need_more = false;

    if (state->beeps_left > 0) {
        state->beeps_left--;
        need_more = true;
        const std::string_view* sound = state->sound;
        Application::GetInstance().Schedule(
            [sound]() { Application::GetInstance().PlaySound(*sound); });
    }

    // 字符串按值拷入 lambda：最后一次触发会 delete state，避免悬垂
    Application::GetInstance().Schedule(
        [t = state->title, m = state->message, e = state->emotion]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetStatus(t.c_str());
            display->SetEmotion(e.c_str());
            display->SetChatMessage("system", m.c_str());
        });

    if (state->reasserts_left > 0) {
        state->reasserts_left--;
        need_more = true;
    }

    if (need_more) {
        int delay = state->beeps_left > 0 ? state->interval_ms : 3000;
        ElderAlertScheduleNext(state, delay);
    } else {
        // 收尾释放状态。中间过程的 esp_timer 句柄未删除（每次告警泄漏
        // 约百字节，告警频率低，可接受）。
        delete state;
    }
}

// ------------------------------------------------------------------
// 告警结束后恢复原音量：一次性 esp_timer，留 1.5 秒余量等最后一声播完。
// ------------------------------------------------------------------
static void RestoreVolumeLater(int volume, int delay_ms) {
    int* boxed = new int(volume);
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        int vol = *static_cast<int*>(arg);
        delete static_cast<int*>(arg);
        Application::GetInstance().Schedule([v = vol]() {
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec)
                codec->SetOutputVolume(v);
        });
    };
    args.arg = boxed;  // esp_timer 不自动传参，漏设即 nullptr 崩溃
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "alert_vol";
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
    } else {
        delete boxed;
    }
}

// ------------------------------------------------------------------
// 银发告警统一入口（必须在主任务上下文调用，即 Schedule 内部）：
// 1) 音量临时拉到 100——默认音量（70）在厨房/电视环境下老人容易漏听
// 2) Alert 弹窗 + 首声，随后补播 extra_beeps 声
// 3) 屏幕信息每次触发都重写，补声结束后再保持约 12 秒
// 4) 全部播完后恢复原音量
// emotion 须用固件支持的表情名（"warning" 无对应图标，映射为 angry）
// ------------------------------------------------------------------
static void ShowElderAlert(const char* title, const char* message, const char* emotion,
                           int extra_beeps = 2, int interval_ms = 2000) {
    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    int previous_volume = 0;
    if (codec) {
        previous_volume = codec->output_volume();
        codec->SetOutputVolume(100);
    }
    std::string emo =
        (emotion != nullptr && std::string_view(emotion) == "warning") ? "angry" : "neutral";
    app.Alert(title, message, emo.c_str(), Lang::Sounds::OGG_EXCLAMATION);

    auto* state = new ElderAlertState{
        title, message, emo, &Lang::Sounds::OGG_EXCLAMATION, extra_beeps, 4, interval_ms};
    ElderAlertScheduleNext(state, interval_ms);  // 首次补声在 +interval

    if (previous_volume > 0) {
        RestoreVolumeLater(previous_volume, (extra_beeps + 1) * interval_ms + 1500);
    }
}

// ------------------------------------------------------------------
// 从数组中摘除并释放节点。
// 必须用库函数摘链：本仓库的 espressif cJSON fork 为 O(1) 尾插，把数组
// 首元素的 prev 当作尾指针（指向自身），与上游 cJSON（首元素 prev==NULL）
// 不同。若手动按上游约定改链，删除唯一/首个元素时会漏掉 parent->child
// 更新，留下悬垂指针，随后 Print 返回 NULL、Delete 访问 0xfefefefe 崩溃。
// ------------------------------------------------------------------
static void UnlinkAndDeleteItem(cJSON* root, cJSON* elem) {
    cJSON* detached = cJSON_DetachItemViaPointer(root, elem);
    if (detached != nullptr) {
        cJSON_Delete(detached);
    }
}

// ------------------------------------------------------------------
// 序列化并写回 NVS。cJSON_PrintUnformatted 失败（树被破坏或内存不足）
// 时返回 false 并打 E 级日志；绝不把 nullptr 交给 std::string，
// 否则抛 logic_error 直接 abort 重启。
// ------------------------------------------------------------------
static bool StoreJson(Settings& settings, const char* what, const char* key, cJSON* root) {
    char* out = cJSON_PrintUnformatted(root);
    if (out == nullptr) {
        ESP_LOGE(TAG, "%s: serialize failed, key=%s", what, key);
        return false;
    }
    settings.SetString(key, out);
    free(out);
    return true;
}

// ------------------------------------------------------------------
// 留言板过期清理：删除 epoch 距今超过保留期（VOICE_BOARD_MSG_TTL_DAYS
// 天，默认 3）的留言；无 epoch 字段的旧格式留言一并清理。
// 系统时间未同步时跳过清理。返回删除条数。
// ------------------------------------------------------------------
static int PurgeExpiredVoiceMessages(cJSON* root) {
    time_t now = time(nullptr);
    if (now <= 1700000000) {
        return 0;  // 时间未同步，无法判断过期
    }
    const double ttl_sec = (double)CONFIG_VOICE_BOARD_MSG_TTL_DAYS * 86400.0;
    int removed = 0;
    int kept = 0;
    cJSON* elem = root->child;
    while (elem != nullptr) {
        cJSON* next = elem->next;
        cJSON* epoch = cJSON_GetObjectItem(elem, "epoch");
        double age = -1.0;
        if (cJSON_IsNumber(epoch)) {
            age = (double)now - epoch->valuedouble;
        }
        if (age < 0.0 || age >= ttl_sec) {
            cJSON_DeleteItemFromArray(root, kept);
            removed++;
        } else {
            kept++;
        }
        elem = next;
    }
    return removed;
}

class CompactWifiBoardS3Cam : public WifiBoard {
private:
    Button boot_button_;
    Button sos_button_;
    LcdDisplay* display_;
    Esp32Camera* camera_;

    // ==================================================================
    // 视觉互斥锁：通过 Camera::TryLock/Unlock 实现跨模块互斥，
    // 确保内置 take_photo、板级 MCP 工具、周期任务不会并发访问摄像头。
    // ==================================================================
    struct CameraLockGuard {
        Camera* cam;
        bool held;
        explicit CameraLockGuard(Camera* c) : cam(c) { held = c ? c->TryLock() : false; }
        ~CameraLockGuard() {
            if (held)
                cam->Unlock();
        }
        bool Held() const { return held; }
    };

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new Esp32Camera(config);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // SOS 紧急按键：长按触发紧急告警
    void InitializeSosButton() {
        sos_button_.OnLongPress([]() {
            ESP_LOGW(TAG, "SOS button long pressed!");
            // 读取紧急联系人，拼到告警消息里
            std::string contacts_info;
            Settings settings("contacts", false);
            std::string stored = settings.GetString("list", "[]");
            cJSON* root = cJSON_Parse(stored.c_str());
            if (root != nullptr && cJSON_IsArray(root)) {
                cJSON* elem = nullptr;
                int idx = 0;
                cJSON_ArrayForEach (elem, root) {
                    if (idx >= 2)
                        break;  // 最多显示前两个
                    cJSON* name = cJSON_GetObjectItem(elem, "name");
                    cJSON* rel = cJSON_GetObjectItem(elem, "relation");
                    cJSON* phone = cJSON_GetObjectItem(elem, "phone");
                    contacts_info += "联系";
                    if (rel && cJSON_IsString(rel))
                        contacts_info += rel->valuestring;
                    if (name && cJSON_IsString(name)) {
                        contacts_info += " ";
                        contacts_info += name->valuestring;
                    }
                    if (phone && cJSON_IsString(phone)) {
                        contacts_info += "：";
                        contacts_info += phone->valuestring;
                    }
                    contacts_info += "\n";
                    idx++;
                }
            }
            if (root)
                cJSON_Delete(root);

            // 关怀同步拉取到的老人姓名（无则用"老人"）
            Settings care_settings("care", false);
            std::string elder_name = care_settings.GetString("elder_name");
            std::string who = elder_name.empty() ? "老人" : elder_name;

            std::string message = "紧急求助！" + who + "按下了 SOS 键。\n" + contacts_info;
            // UI/音频/MCP 消息必须回主任务
            Application::GetInstance().Schedule([message]() {
                auto& app = Application::GetInstance();
                ShowElderAlert("紧急求助", message.c_str(), "warning");
                app.SendMcpMessage("{\"type\":\"sos_alert\",\"source\":\"device\"}");
            });
        });
    }

    // 银发经济自定义 MCP 工具
    // 按项目规则，自定义工具应通过板子的 InitializeTools 注册（不在 mcp_server.cc 中）
    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();

        // 工具 1：服药提醒管理
        mcp.AddTool(
            "self.medication_reminder",
            "管理老人的服药提醒闹钟（添加/删除/查询提醒时间点）。\n"
            "仅用于设置和查看'几点该吃什么药'的提醒计划，不回答用药咨询问题\n"
            "（如漏服处理、药物相互作用、饭前饭后等），此类问题请直接回答。\n"
            "Args:\n"
            "  action: 'add' | 'remove' | 'list'\n"
            "  medicine: 药名（add 时必填）\n"
            "  time: 'HH:MM' 24 小时制（add/remove 时必填）\n"
            "Return:\n"
            "  list 返回 [{time, medicine}, ...]；add/remove 返回更新后的列表。",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("medicine", kPropertyTypeString, std::string("")),
                Property("time", kPropertyTypeString, std::string("")),
            }),
            [](const PropertyList& properties) -> ToolResult {
                auto action = properties["action"].value<std::string>();
                auto medicine = properties["medicine"].value<std::string>();
                auto time = properties["time"].value<std::string>();

                Settings settings("medication", true);
                std::string stored = settings.GetString("reminders", "[]");

                cJSON* root = cJSON_Parse(stored.c_str());
                if (root == nullptr || !cJSON_IsArray(root)) {
                    if (root)
                        cJSON_Delete(root);
                    root = cJSON_CreateArray();
                }

                if (action == "add") {
                    if (time.empty() || medicine.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("medicine and time are required for 'add'");
                    }
                    // 重复计划直接返回现有列表（含重复时间+药名的计划）
                    cJSON* exist = nullptr;
                    cJSON_ArrayForEach (exist, root) {
                        cJSON* t = cJSON_GetObjectItem(exist, "time");
                        cJSON* m = cJSON_GetObjectItem(exist, "medicine");
                        if (cJSON_IsString(t) && cJSON_IsString(m) &&
                            strcmp(t->valuestring, time.c_str()) == 0 &&
                            strcmp(m->valuestring, medicine.c_str()) == 0) {
                            return root;
                        }
                    }
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "time", time.c_str());
                    cJSON_AddStringToObject(item, "medicine", medicine.c_str());
                    cJSON_AddItemToArray(root, item);
                // 云端同步放到后台任务，避免阻塞 MCP 响应导致 30s 超时。
                // planId 由 care_sync 周期任务兜底回填（每 5 分钟自愈）。
#ifdef CONFIG_ENABLE_CARE_SYNC
                    auto* med = new std::string(medicine);
                    auto* t = new std::string(time);
                    xTaskCreate(
                        [](void* arg) {
                            auto* pair = static_cast<std::pair<std::string*, std::string*>*>(arg);
                            std::string plan_id;
                            if (CarePushPlan(pair->first->c_str(), pair->second->c_str(),
                                             &plan_id)) {
                                ESP_LOGI(TAG, "medication_reminder: cloud synced (%s %s -> %s)",
                                         pair->second->c_str(), pair->first->c_str(),
                                         plan_id.c_str());
                            } else {
                                ESP_LOGW(TAG, "medication_reminder: cloud sync failed (%s %s)",
                                         pair->second->c_str(), pair->first->c_str());
                            }
                            delete pair->first;
                            delete pair->second;
                            delete pair;
                            vTaskDelete(NULL);
                        },
                        "care_plan_add", 4096, new std::pair<std::string*, std::string*>(med, t), 1,
                        nullptr);
#endif
                } else if (action == "remove") {
                    if (time.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("time is required for 'remove'");
                    }
                    // 原地删除匹配项；不能用 AddItemReferenceToArray+Delete(root)，
                    // 引用会悬垂导致 PrintUnformatted 访问已释放内存
                    std::vector<std::string> removed_plan_ids;
                    cJSON* elem = root->child;
                    while (elem != nullptr) {
                        cJSON* next = elem->next;
                        cJSON* t = cJSON_GetObjectItem(elem, "time");
                        if (t != nullptr && cJSON_IsString(t) &&
                            strcmp(t->valuestring, time.c_str()) == 0) {
#ifdef CONFIG_ENABLE_CARE_SYNC
                            cJSON* pid = cJSON_GetObjectItem(elem, "planId");
                            if (pid != nullptr && cJSON_IsString(pid)) {
                                removed_plan_ids.emplace_back(pid->valuestring);
                            }
#endif
                            UnlinkAndDeleteItem(root, elem);
                        }
                        elem = next;
                    }
#ifdef CONFIG_ENABLE_CARE_SYNC
                    // 云端同步删除放到后台任务执行，避免阻塞 MCP 响应导致 30s 超时。
                    // 先把 planId 记入 tombstone（NVS 持久化）：若后台删除失败
                    // （如会话结束时 Wi-Fi 省电/断连），care_sync 周期任务会重试，
                    // 防止"本地已删、云端仍在"被下一拍 sync 按"云端为准"复活。
                    for (const auto& pid : removed_plan_ids) {
                        AddPendingDelete(pid);
                    }
                    if (!removed_plan_ids.empty()) {
                        auto* ids = new std::vector<std::string>(std::move(removed_plan_ids));
                        xTaskCreate(
                            [](void* arg) {
                                auto* plan_ids = static_cast<std::vector<std::string>*>(arg);
                                for (const auto& pid : *plan_ids) {
                                    if (CareDeletePlan(pid)) {
                                        RemovePendingDelete(pid);
                                        ESP_LOGI(TAG,
                                                 "medication_reminder: cloud plan deleted (%s)",
                                                 pid.c_str());
                                    } else {
                                        ESP_LOGW(TAG,
                                                 "medication_reminder: cloud delete failed (%s)",
                                                 pid.c_str());
                                    }
                                }
                                delete plan_ids;
                                vTaskDelete(NULL);
                            },
                            "care_plan_del", 4096, ids, 1, nullptr);
                    }
#endif
                } else if (action != "list") {
                    cJSON_Delete(root);
                    return std::unexpected("Unknown action: " + action);
                }

                if (action != "list") {
                    if (!StoreJson(settings, "medication_reminder", "reminders", root)) {
                        // 序列化失败：NVS 内容或堆已异常，报错而不是重启
                        ESP_LOGE(TAG, "medication_reminder: stored=%s", stored.c_str());
                        cJSON_Delete(root);
                        return std::unexpected(
                            "internal error: reminders data corrupted, storage reset");
                    }
                }
                return root;
            });

        // 工具 2：跌倒检测（AI 按需触发拍照分析）
        mcp.AddTool(
            "self.fall_detection",
            "拍照并调用云端视觉模型判断画面中是否有人呈跌倒姿态。\n"
            "用于实时检测，不回答跌倒预防/急救知识问题（此类问题请直接回答）。\n"
            "返回 JSON {fell, confidence, description}。",
            PropertyList(), [](const PropertyList& properties) -> ToolResult {
                auto camera = Board::GetInstance().GetCamera();
                if (camera == nullptr) {
                    return std::unexpected("Camera not available on this board");
                }
                // 视觉互斥：周期跌倒检测任务可能正在用摄像头
                CameraLockGuard guard(camera);
                if (!guard.Held()) {
                    ESP_LOGW(TAG, "fall_detection tool: camera busy");
                    return std::unexpected("Camera is busy, please try again in a few seconds");
                }
                TaskPriorityReset priority_reset(1);
                if (!camera->Capture()) {
                    return std::unexpected("Failed to capture photo");
                }
                std::string prompt =
                    "观察画面中是否有人呈跌倒或瘫坐姿态。"
                    "返回 JSON：{\"fell\": bool, \"confidence\": 0.0-1.0, "
                    "\"description\": 简短描述}";
                auto result = camera->Explain(prompt);
                if (!result) {
                    // 拍照已成功且照片已在屏幕显示（Capture 内部完成）；
                    // 上传失败时返回说明而非错误，避免掩盖拍照成功的事实
                    return std::string("照片已拍摄并显示在屏幕上，但上传云端分析失败：") +
                           result.error();
                }
                return std::move(*result);
            });

        // 工具 3：家属留言板（文字版）
        mcp.AddTool("self.family_voice_board",
                    "家属留言板。家属可远程为老人添加文字留言，老人按键时设备会朗读。\n"
                    "留言默认保留 3 天（menuconfig VOICE_BOARD_MSG_TTL_DAYS 可调 1-30 天），\n"
                    "到期自动删除。\n"
                    "Args:\n"
                    "  action: 'add' | 'list' | 'play_latest'\n"
                    "  sender: 留言人姓名（add 时必填）\n"
                    "  message: 留言内容（add 时必填）\n"
                    "Return:\n"
                    "  list 返回 [{sender, message, time}, ...]；\n"
                    "  play_latest 返回最新留言文本，调用方应通过 TTS 播报。",
                    PropertyList({
                        Property("action", kPropertyTypeString),
                        Property("sender", kPropertyTypeString, std::string("")),
                        Property("message", kPropertyTypeString, std::string("")),
                    }),
                    [](const PropertyList& properties) -> ToolResult {
                        auto action = properties["action"].value<std::string>();
                        auto sender = properties["sender"].value<std::string>();
                        auto message = properties["message"].value<std::string>();

                        Settings settings("voice_board", true);
                        std::string stored = settings.GetString("messages", "[]");

                        cJSON* root = cJSON_Parse(stored.c_str());
                        if (root == nullptr || !cJSON_IsArray(root)) {
                            if (root)
                                cJSON_Delete(root);
                            root = cJSON_CreateArray();
                        }

                        // 过期留言自动清理：有删除则立即写回 NVS
                        if (PurgeExpiredVoiceMessages(root) > 0) {
                            StoreJson(settings, "family_voice_board", "messages", root);
                        }

                        if (action == "add") {
                            if (sender.empty() || message.empty()) {
                                cJSON_Delete(root);
                                return std::unexpected("sender and message are required for 'add'");
                            }
                            time_t now = time(nullptr);
                            char time_buf[32] = {0};
                            if (now > 1700000000) {
                                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M",
                                         localtime(&now));
                            } else {
                                snprintf(time_buf, sizeof(time_buf), "tick");
                            }
                            cJSON* item = cJSON_CreateObject();
                            cJSON_AddStringToObject(item, "sender", sender.c_str());
                            cJSON_AddStringToObject(item, "message", message.c_str());
                            cJSON_AddStringToObject(item, "time", time_buf);
                            cJSON_AddNumberToObject(item, "epoch", (double)now);
                            cJSON_AddItemToArray(root, item);
                            StoreJson(settings, "family_voice_board", "messages", root);
                            return root;
                        }

                        if (action == "list") {
                            return root;
                        }

                        if (action == "play_latest") {
                            int count = cJSON_GetArraySize(root);
                            if (count == 0) {
                                cJSON_Delete(root);
                                return std::unexpected("No messages yet");
                            }
                            cJSON* last = cJSON_GetArrayItem(root, count - 1);
                            cJSON* msg = cJSON_GetObjectItem(last, "message");
                            cJSON* sdr = cJSON_GetObjectItem(last, "sender");
                            std::string text = "来自 ";
                            text += (sdr && cJSON_IsString(sdr)) ? sdr->valuestring : "家人";
                            text += " 的留言：";
                            text += (msg && cJSON_IsString(msg)) ? msg->valuestring : "";
                            cJSON_Delete(root);
                            return text;
                        }

                        cJSON_Delete(root);
                        return std::unexpected("Unknown action: " + action);
                    });

        // ===== 银发经济扩展工具 =====

        // 工具 4：用药打卡记录
        mcp.AddTool(
            "self.medication_log",
            "记录和查询老人的服药情况（打卡+医嘱依从性）。\n"
            "仅用于打卡和查记录，不回答用药咨询问题（如漏服处理、药物禁忌、何时补服等），\n"
            "此类问题请直接回答，不要调用本工具。\n"
            "Args:\n"
            "  action: 'checkin' | 'status' | 'list_today' | 'report' | 'history'\n"
            "  medicine: 药名（checkin/status 时必填）\n"
            "  days: history 时可选，查询最近几天（默认7，最多7）\n"
            "Return:\n"
            "  list_today 返回今日打卡事件 [{medicine, time}, ...]（time 为打卡时刻 HH:MM）；\n"
            "  status 返回 {taken, time}，未打卡时 time 为空；\n"
            "  report 返回今日医嘱执行情况 {doses:[{medicine, planned, status, actual, delay}],\n"
            "    total, taken, late, missed, pending, unplanned}，"
            "status: taken按时/late迟服/missed漏服/pending待服，delay 为比计划晚的分钟数；\n"
            "  history 返回最近 days 天的剂量记录 [{date, doses}, ...]。\n"
            "  report/history 需开启 ENABLE_MEDICATION_REMINDER_TASK。",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("medicine", kPropertyTypeString, std::string("")),
                Property("days", kPropertyTypeInteger, 7),
            }),
            [](const PropertyList& properties) -> ToolResult {
                auto action = properties["action"].value<std::string>();
                auto medicine = properties["medicine"].value<std::string>();
                auto days = properties["days"].value<int>();

                Settings settings("medication", true);
                std::string stored = settings.GetString("logs", "{}");
                cJSON* root = cJSON_Parse(stored.c_str());
                if (root == nullptr || !cJSON_IsObject(root)) {
                    if (root)
                        cJSON_Delete(root);
                    root = cJSON_CreateObject();
                }

                time_t now = time(nullptr);
                char today[16] = {0};
                char clock[8] = {0};
                bool time_valid = now > 1700000000;
                if (time_valid) {
                    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&now));
                    strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
                } else {
                    snprintf(today, sizeof(today), "unknown");
                }

                // 兼容旧数据：从数组元素中取药名。
                // 旧格式为纯字符串 "降压药"，新格式为 {"medicine":"降压药","time":"08:30"}。
                auto elem_medicine = [](cJSON* elem) -> const char* {
                    if (cJSON_IsString(elem)) {
                        return elem->valuestring;
                    }
                    if (cJSON_IsObject(elem)) {
                        cJSON* m = cJSON_GetObjectItem(elem, "medicine");
                        if (cJSON_IsString(m)) {
                            return m->valuestring;
                        }
                    }
                    return nullptr;
                };

                if (action == "checkin") {
                    if (medicine.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("medicine is required for 'checkin'");
                    }
                    cJSON* day_arr = cJSON_GetObjectItem(root, today);
                    if (day_arr == nullptr || !cJSON_IsArray(day_arr)) {
                        day_arr = cJSON_CreateArray();
                        cJSON_AddItemToObject(root, today, day_arr);
                    }
                    // 避免重复打卡；已打过卡则保留原记录（含原打卡时间）
                    cJSON* elem = nullptr;
                    cJSON_ArrayForEach (elem, day_arr) {
                        const char* name = elem_medicine(elem);
                        if (name != nullptr && strcmp(name, medicine.c_str()) == 0) {
                            StoreJson(settings, "medication_log", "logs", root);
                            return root;
                        }
                    }
                    cJSON* record = cJSON_CreateObject();
                    cJSON_AddStringToObject(record, "medicine", medicine.c_str());
                    cJSON_AddStringToObject(record, "time", time_valid ? clock : "unknown");
                    cJSON_AddItemToArray(day_arr, record);
                    StoreJson(settings, "medication_log", "logs", root);
#ifdef CONFIG_ENABLE_MEDICATION_REMINDER_TASK
                    // 联动当日医嘱剂量状态：pending -> taken，missed -> late
                    if (time_valid) {
                        RecordMedicationCheckin(medicine, now);
                    }
#endif
#ifdef CONFIG_ENABLE_CARE_SYNC
                    // 上报后台 medication_record（msgId 幂等：每天每药一条）
                    if (time_valid) {
                        CareReportTaken(medicine, now);
                    }
#endif
                    return root;
                }

                if (action == "status") {
                    if (medicine.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("medicine is required for 'status'");
                    }
                    cJSON* day_arr = cJSON_GetObjectItem(root, today);
                    bool taken = false;
                    std::string taken_time;
                    if (day_arr != nullptr && cJSON_IsArray(day_arr)) {
                        cJSON* elem = nullptr;
                        cJSON_ArrayForEach (elem, day_arr) {
                            const char* name = elem_medicine(elem);
                            if (name != nullptr && strcmp(name, medicine.c_str()) == 0) {
                                taken = true;
                                if (cJSON_IsObject(elem)) {
                                    cJSON* t = cJSON_GetObjectItem(elem, "time");
                                    if (cJSON_IsString(t)) {
                                        taken_time = t->valuestring;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root);
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddBoolToObject(result, "taken", taken);
                    cJSON_AddStringToObject(result, "time", taken ? taken_time.c_str() : "");
                    return result;
                }

                if (action == "list_today") {
                    cJSON* day_arr = cJSON_GetObjectItem(root, today);
                    if (day_arr == nullptr) {
                        cJSON_Delete(root);
                        return cJSON_CreateArray();
                    }
                    cJSON* result = cJSON_Duplicate(day_arr, 1);
                    cJSON_Delete(root);
                    return result;
                }

#ifdef CONFIG_ENABLE_MEDICATION_REMINDER_TASK
                if (action == "report") {
                    cJSON_Delete(root);
                    return BuildMedicationReport(now);
                }

                if (action == "history") {
                    cJSON_Delete(root);
                    return BuildMedicationHistory(days, now);
                }
#else
                if (action == "report" || action == "history") {
                    cJSON_Delete(root);
                    (void)days;
                    return std::unexpected("Medication reminder task is not enabled in firmware");
                }
#endif

                cJSON_Delete(root);
                return std::unexpected("Unknown action: " + action);
            });

        // 工具 5：通用日程提醒
#if !defined(CONFIG_ENABLE_SCHEDULE_REMINDER)
#pragma message( \
    "self.schedule_reminder: ENABLE_SCHEDULE_REMINDER is OFF, reminders will be stored but NEVER fire")
#endif
        mcp.AddTool(
            "self.schedule_reminder",
            "管理老人的日程提醒（不止吃药，如看病、交水费、生日等）。\n"
#if !defined(CONFIG_ENABLE_SCHEDULE_REMINDER)
            "【警告】当前固件未开启日程播报后台任务（ENABLE_SCHEDULE_REMINDER），"
            "提醒只能保存、到点不会弹屏或响铃，必须明确告知用户这一限制。\n"
#endif
            "Args:\n"
            "  action: 'add' | 'remove' | 'list'\n"
            "  time: 'HH:MM' 24 小时制（add/remove 必填）\n"
            "  content: 提醒内容（add 必填）\n"
            "Return:\n"
            "  list 返回 [{time, content}, ...]。\n"
            "  启用 CONFIG_ENABLE_SCHEDULE_REMINDER 后，到点会自动弹屏+提示音。",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("time", kPropertyTypeString, std::string("")),
                Property("content", kPropertyTypeString, std::string("")),
            }),
            [](const PropertyList& properties) -> ToolResult {
                auto action = properties["action"].value<std::string>();
                auto time = properties["time"].value<std::string>();
                auto content = properties["content"].value<std::string>();

                Settings settings("schedule", true);
                std::string stored = settings.GetString("reminders", "[]");
                cJSON* root = cJSON_Parse(stored.c_str());
                if (root == nullptr || !cJSON_IsArray(root)) {
                    if (root)
                        cJSON_Delete(root);
                    root = cJSON_CreateArray();
                }

                if (action == "add") {
                    if (time.empty() || content.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("time and content are required for 'add'");
                    }
#if !defined(CONFIG_ENABLE_SCHEDULE_REMINDER)
                    // 播报任务未编译进固件：存了也不会响，直接报错让 AI 告知用户
                    cJSON_Delete(root);
                    return std::unexpected(
                        "日程播报后台任务未启用（menuconfig 中 ENABLE_SCHEDULE_REMINDER 未开启），"
                        "提醒到点不会弹屏或响铃。请先在 SDK Configuration Editor 勾选 "
                        "Enable Scheduled Reminder Announcement "
                        "并重新编译烧录固件，再设置日程提醒。");
#endif
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "time", time.c_str());
                    cJSON_AddStringToObject(item, "content", content.c_str());
                    cJSON_AddItemToArray(root, item);
                    StoreJson(settings, "schedule_reminder", "reminders", root);
                    return root;
                }

                if (action == "remove") {
                    if (time.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("time is required for 'remove'");
                    }
                    // 原地删除匹配项；不能用 AddItemReferenceToArray+Delete(root)，
                    // 引用会悬垂导致 PrintUnformatted 访问已释放内存
                    cJSON* elem = root->child;
                    while (elem != nullptr) {
                        cJSON* next = elem->next;
                        cJSON* t = cJSON_GetObjectItem(elem, "time");
                        if (t != nullptr && cJSON_IsString(t) &&
                            strcmp(t->valuestring, time.c_str()) == 0) {
                            UnlinkAndDeleteItem(root, elem);
                        }
                        elem = next;
                    }
                    StoreJson(settings, "schedule_reminder", "reminders", root);
                    return root;
                }

                if (action == "list") {
                    return root;
                }

                cJSON_Delete(root);
                return std::unexpected("Unknown action: " + action);
            });

        // 工具 6：紧急联系人管理
        mcp.AddTool("self.emergency_contact",
                    "管理老人的紧急联系人（家属、医生等）。\n"
                    "Args:\n"
                    "  action: 'add' | 'remove' | 'list'\n"
                    "  name: 联系人姓名（add 必填）\n"
                    "  relation: 关系，如'儿子''医生'（add 必填）\n"
                    "  phone: 电话号码（add 必填）\n"
                    "Return:\n"
                    "  list 返回 [{name, relation, phone}, ...]。",
                    PropertyList({
                        Property("action", kPropertyTypeString),
                        Property("name", kPropertyTypeString, std::string("")),
                        Property("relation", kPropertyTypeString, std::string("")),
                        Property("phone", kPropertyTypeString, std::string("")),
                    }),
                    [](const PropertyList& properties) -> ToolResult {
                        auto action = properties["action"].value<std::string>();
                        auto name = properties["name"].value<std::string>();
                        auto relation = properties["relation"].value<std::string>();
                        auto phone = properties["phone"].value<std::string>();

                        Settings settings("contacts", true);
                        std::string stored = settings.GetString("list", "[]");
                        cJSON* root = cJSON_Parse(stored.c_str());
                        if (root == nullptr || !cJSON_IsArray(root)) {
                            if (root)
                                cJSON_Delete(root);
                            root = cJSON_CreateArray();
                        }

                        if (action == "add") {
                            if (name.empty() || phone.empty()) {
                                cJSON_Delete(root);
                                return std::unexpected("name and phone are required for 'add'");
                            }
                            cJSON* item = cJSON_CreateObject();
                            cJSON_AddStringToObject(item, "name", name.c_str());
                            cJSON_AddStringToObject(item, "relation", relation.c_str());
                            cJSON_AddStringToObject(item, "phone", phone.c_str());
                            cJSON_AddItemToArray(root, item);
                            StoreJson(settings, "emergency_contact", "list", root);
                            return root;
                        }

                        if (action == "remove") {
                            if (name.empty()) {
                                cJSON_Delete(root);
                                return std::unexpected("name is required for 'remove'");
                            }
                            // 原地删除匹配项；不能用 AddItemReferenceToArray+Delete(root)，
                            // 引用会悬垂导致 PrintUnformatted 访问已释放内存
                            cJSON* elem = root->child;
                            while (elem != nullptr) {
                                cJSON* next = elem->next;
                                cJSON* n = cJSON_GetObjectItem(elem, "name");
                                if (n != nullptr && cJSON_IsString(n) &&
                                    strcmp(n->valuestring, name.c_str()) == 0) {
                                    UnlinkAndDeleteItem(root, elem);
                                }
                                elem = next;
                            }
                            StoreJson(settings, "emergency_contact", "list", root);
                            return root;
                        }

                        if (action == "list") {
                            return root;
                        }

                        cJSON_Delete(root);
                        return std::unexpected("Unknown action: " + action);
                    });

        // 工具 7：找东西（复用摄像头+云端视觉）
        mcp.AddTool(
            "self.find_item",
            "拍照并让云端视觉模型帮忙找东西（眼镜、钥匙、手机等）。\n"
            "Args:\n"
            "  item: 要找的物品名称\n"
            "Return:\n"
            "  模型对画面中物品位置的描述文本，由 AI 转语音告诉老人。",
            PropertyList({
                Property("item", kPropertyTypeString),
            }),
            [](const PropertyList& properties) -> ToolResult {
                auto item = properties["item"].value<std::string>();
                if (item.empty()) {
                    return std::unexpected("item is required");
                }
                auto camera = Board::GetInstance().GetCamera();
                if (camera == nullptr) {
                    return std::unexpected("Camera not available on this board");
                }
                // 视觉互斥：周期检测任务可能正在用摄像头
                CameraLockGuard guard(camera);
                if (!guard.Held()) {
                    ESP_LOGW(TAG, "find_item: camera busy");
                    return std::unexpected("Camera is busy, please try again in a few seconds");
                }
                TaskPriorityReset priority_reset(1);
                if (!camera->Capture()) {
                    return std::unexpected("Failed to capture photo, please try again");
                }
                std::string prompt = "请仔细观察画面，帮忙寻找「" + item +
                                     "」。如果找到了，请描述它在画面中的位置"
                                     "（如'在左边的桌子上'）；如果没找到，请说'没看到'。"
                                     "请用简短的中文回答。";
                auto result = camera->Explain(prompt);
                if (!result) {
                    // 拍照已成功且照片已在屏幕显示；上传失败（含 429 限流）告知而非报错
                    return std::string("照片已拍摄并显示在屏幕上，但上传云端分析失败：") +
                           result.error();
                }
                // Explain 返回的是信封 JSON，取 text 字段
                std::string model_text;
                cJSON* envelope = cJSON_Parse(result->c_str());
                if (envelope != nullptr) {
                    cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
                    model_text =
                        (text_item && cJSON_IsString(text_item)) ? text_item->valuestring : *result;
                    cJSON_Delete(envelope);
                } else {
                    model_text = *result;
                }
                return model_text;
            });

        // 工具 8：门口是谁（防诈骗）
        mcp.AddTool(
            "self.door_identification",
            "拍照识别门口的人，防诈骗。\n"
            "无参数。返回对画面中人物的描述（人数、穿着、是否认识等），"
            "由 AI 转语音告诉老人。",
            PropertyList(), [](const PropertyList& properties) -> ToolResult {
                auto camera = Board::GetInstance().GetCamera();
                if (camera == nullptr) {
                    return std::unexpected("Camera not available on this board");
                }
                // 视觉互斥：周期检测任务可能正在用摄像头
                CameraLockGuard guard(camera);
                if (!guard.Held()) {
                    ESP_LOGW(TAG, "door_identification: camera busy");
                    return std::unexpected("Camera is busy, please try again in a few seconds");
                }
                TaskPriorityReset priority_reset(1);
                if (!camera->Capture()) {
                    return std::unexpected("Failed to capture photo, please try again");
                }
                std::string prompt =
                    "请观察画面中的人（们），描述：有几个人、大概年龄、"
                    "穿着什么衣服、手里拿着什么。如果画面里没有人，说'门口没人'。"
                    "请用简短的中文回答。";
                auto result = camera->Explain(prompt);
                if (!result) {
                    // 拍照已成功且照片已在屏幕显示；上传失败（含 429 限流）告知而非报错
                    return std::string("照片已拍摄并显示在屏幕上，但上传云端分析失败：") +
                           result.error();
                }
                std::string model_text;
                cJSON* envelope = cJSON_Parse(result->c_str());
                if (envelope != nullptr) {
                    cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
                    model_text =
                        (text_item && cJSON_IsString(text_item)) ? text_item->valuestring : *result;
                    cJSON_Delete(envelope);
                } else {
                    model_text = *result;
                }
                return model_text;
            });

        // 工具 9：天气查询
        mcp.AddTool("self.weather_query",
                    "查询当前实时天气数据（温度、天气状况）。\n"
                    "仅返回天气数据，不回答穿衣建议或健康指导（此类问题请直接回答）。\n"
                    "Args:\n"
                    "  city: 城市名（可选，默认自动定位）\n"
                    "Return:\n"
                    "  天气描述文本，由 AI 转语音告诉老人。",
                    PropertyList({
                        Property("city", kPropertyTypeString, std::string("")),
                    }),
                    [](const PropertyList& properties) -> ToolResult {
                        auto city = properties["city"].value<std::string>();
                        auto network = Board::GetInstance().GetNetwork();
                        if (network == nullptr) {
                            return std::unexpected("Network not available");
                        }
                        // wttr.in 支持纯文本格式：format=3 返回 "城市: 天气 温度"
                        std::string url = "https://wttr.in/";
                        if (!city.empty()) {
                            url += city;
                        }
                        url += "?format=3&lang=zh";

                        auto http = network->CreateHttp(10);
                        if (http == nullptr) {
                            return std::unexpected("Failed to create HTTP client");
                        }
                        auto opened = http->Open("GET", url);
                        if (!opened) {
                            std::string err = opened.error().ToString().c_str();
                            http->Close();
                            return std::unexpected("Failed to open HTTP connection: " + err);
                        }
                        auto status = http->GetStatusCode();
                        if (!status || *status != 200) {
                            http->Close();
                            return std::unexpected("Weather API returned status: " +
                                                   std::to_string(status ? *status : -1));
                        }
                        std::string body = http->ReadAll();
                        http->Close();
                        if (body.empty()) {
                            return std::unexpected("Empty response from weather API");
                        }
                        return body;
                    });
    }

#ifdef CONFIG_ENABLE_BOARD_FALL_DETECTION
    // ====================================================================
    // 跌倒检测后台周期任务（仅当 Kconfig 启用 CONFIG_ENABLE_BOARD_FALL_DETECTION 时编译）
    // --------------------------------------------------------------------
    // 与 AI 按需调用的 self.fall_detection 工具互补：
    //   - 工具：AI 在对话上下文里主动调用一次
    //   - 周期任务：每 N 秒后台自动拍照+云端视觉分析，无需对话触发
    //
    // 线程模型：
    //   1. esp_timer 在 timer service task 里回调 FallDetectionTimerCb
    //   2. 回调里用 std::atomic<bool> 做重入保护，防止上一轮还没结束又起一轮
    //   3. 实际工作丢到独立 FreeRTOS 任务 "fall_det" 里跑，避免阻塞
    //      timer task 和主任务
    //   4. 检测到跌倒时，UI/音频操作通过 Application::Schedule 丢回主任务
    // ====================================================================

    esp_timer_handle_t fall_detection_timer_ = nullptr;
    std::atomic<bool> fall_detection_running_{false};

    // esp_timer 回调（在 timer service task 里运行，必须短小、不能阻塞）
    static void FallDetectionTimerCb(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
        self->TryStartFallDetection();
    }

    // 重入保护：若上一轮检测还在跑，跳过本次触发
    void TryStartFallDetection() {
        bool expected = false;
        if (!fall_detection_running_.compare_exchange_strong(expected, true)) {
            ESP_LOGD(TAG, "fall_detection: previous round still running, skip");
            return;
        }
        BaseType_t ret = xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
                self->RunFallDetection();
                self->fall_detection_running_.store(false);
                vTaskDelete(NULL);
            },
            "fall_det", 8192, this, 2, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "fall_detection: failed to create task");
            fall_detection_running_.store(false);
        }
    }

    // 实际的检测逻辑（在独立任务 "fall_det" 里运行）
    void RunFallDetection() {
        if (camera_ == nullptr) {
            ESP_LOGW(TAG, "fall_detection: camera not available");
            return;
        }

        // 视觉互斥：手动视觉工具（find_item 等）可能正在用摄像头，跳过本轮
        CameraLockGuard guard(camera_);
        if (!guard.Held()) {
            ESP_LOGI(TAG, "fall_detection: camera busy, skip this round");
            return;
        }

        // 降低任务优先级，让出 CPU 给音视频任务（参考 take_photo 的做法）
        TaskPriorityReset priority_reset(1);

        if (!camera_->Capture()) {
            ESP_LOGW(TAG, "fall_detection: capture failed");
            return;
        }

        std::string prompt =
            "观察画面中是否有人呈跌倒或瘫坐姿态。"
            "返回 JSON：{\"fell\": bool, \"confidence\": 0.0-1.0, "
            "\"description\": 简短描述}";
        auto result = camera_->Explain(prompt);
        if (!result) {
            ESP_LOGW(TAG, "fall_detection: explain failed: %s", result.error().c_str());
            return;
        }

        // 解析云端返回。注意返回是"两层嵌套"结构：
        //   外层（服务器信封）: {"success":true,"filename":"xx.jpg","text":"<模型回答>"}
        //   内层（模型回答）  : {"fell": true, "confidence": 0.6, "description": "..."}
        // 必须先取 text 字段拿到字符串，再解析一次，否则在外层找不到 fell。
        std::string model_text;
        cJSON* envelope = cJSON_Parse(result->c_str());
        if (envelope != nullptr) {
            cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
            if (text_item != nullptr && cJSON_IsString(text_item)) {
                model_text = text_item->valuestring;
            } else {
                // 没有信封结构，说明返回本身就是模型回答
                model_text = *result;
            }
            cJSON_Delete(envelope);
        } else {
            model_text = *result;
        }

        // 模型可能返回 ```json {...} ``` 或夹带解释文字，截取第一个 { 到最后一个 }
        bool fell = false;
        std::string description;
        size_t begin = model_text.find('{');
        size_t end = model_text.rfind('}');
        if (begin != std::string::npos && end != std::string::npos && end > begin) {
            std::string payload = model_text.substr(begin, end - begin + 1);
            cJSON* root = cJSON_Parse(payload.c_str());
            if (root != nullptr) {
                cJSON* fell_item = cJSON_GetObjectItem(root, "fell");
                if (fell_item != nullptr && cJSON_IsBool(fell_item)) {
                    fell = cJSON_IsTrue(fell_item);
                }
                cJSON* desc_item = cJSON_GetObjectItem(root, "description");
                if (desc_item != nullptr && cJSON_IsString(desc_item)) {
                    description = desc_item->valuestring;
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "fall_detection: failed to parse model reply: %s",
                         model_text.c_str());
            }
        }

        if (!fell) {
            ESP_LOGI(TAG, "fall_detection: no fall detected");
            return;
        }

        ESP_LOGW(TAG, "fall_detection: FALL DETECTED! %s", description.c_str());
        // UI / 音频 / MCP 消息必须通过主任务执行（参考 AGENTS.md）
        Application::GetInstance().Schedule([description]() {
            auto& app = Application::GetInstance();
            // 把模型的描述一起带上，屏幕和家属端能看到具体发生了什么
            ShowElderAlert(
                "跌倒警报",
                description.empty() ? "检测到老人可能跌倒，请立即确认" : description.c_str(),
                "warning");
            app.SendMcpMessage("{\"type\":\"fall_alert\",\"source\":\"device\"}");
        });
    }

    void StartFallDetection() {
        esp_timer_create_args_t args = {};
        args.callback = FallDetectionTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "fall_det";
        esp_err_t err = esp_timer_create(&args, &fall_detection_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fall_detection: create timer failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_timer_start_periodic(fall_detection_timer_,
                                       CONFIG_BOARD_FALL_DETECTION_PERIOD_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fall_detection: start timer failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "fall_detection: timer started, period=%d ms",
                 CONFIG_BOARD_FALL_DETECTION_PERIOD_MS);
    }
#endif  // CONFIG_ENABLE_BOARD_FALL_DETECTION

#ifdef CONFIG_ENABLE_SCHEDULE_REMINDER
    // ====================================================================
    // 日程提醒后台任务：每分钟检查 NVS 中的 schedule/reminders
    // 到点后自动弹屏+提示音
    // ====================================================================
    esp_timer_handle_t schedule_timer_ = nullptr;
    std::string schedule_last_minute_;

    static void ScheduleTimerCb(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
        self->CheckScheduleReminders();
    }

    void CheckScheduleReminders() {
        time_t now = time(nullptr);
        if (now < 1700000000) {
            return;  // 系统时间未同步，跳过
        }
        char cur_hm[6] = {0};
        strftime(cur_hm, sizeof(cur_hm), "%H:%M", localtime(&now));
        std::string cur = cur_hm;
        if (cur == schedule_last_minute_) {
            return;  // 同一分钟只处理一次
        }
        schedule_last_minute_ = cur;

        Settings settings("schedule", false);
        std::string stored = settings.GetString("reminders", "[]");
        cJSON* root = cJSON_Parse(stored.c_str());
        if (root == nullptr || !cJSON_IsArray(root)) {
            if (root)
                cJSON_Delete(root);
            return;
        }
        cJSON* elem = nullptr;
        cJSON_ArrayForEach (elem, root) {
            cJSON* t = cJSON_GetObjectItem(elem, "time");
            cJSON* c = cJSON_GetObjectItem(elem, "content");
            if (t != nullptr && cJSON_IsString(t) && strcmp(t->valuestring, cur.c_str()) == 0) {
                std::string content =
                    (c != nullptr && cJSON_IsString(c)) ? c->valuestring : "该办事了";
                ESP_LOGI(TAG, "schedule_reminder: firing '%s' at %s", content.c_str(), cur.c_str());
                Application::GetInstance().Schedule(
                    [content]() { ShowElderAlert("日程提醒", content.c_str(), "info"); });
            }
        }
        cJSON_Delete(root);
    }

    void StartScheduleReminder() {
        esp_timer_create_args_t args = {};
        args.callback = ScheduleTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "schedule_rem";
        esp_err_t err = esp_timer_create(&args, &schedule_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "schedule_reminder: create timer failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_timer_start_periodic(schedule_timer_, 60 * 1000 * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "schedule_reminder: start timer failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "schedule_reminder: timer started, checking every 60s");
    }
#endif  // CONFIG_ENABLE_SCHEDULE_REMINDER

#ifdef CONFIG_ENABLE_MEDICATION_REMINDER_TASK
    // ====================================================================
    // 服药提醒闭环后台任务
    // --------------------------------------------------------------------
    // 完整流程：医嘱计划(medication_reminder) -> 到点主动提醒 -> 重复催促
    //          -> 老人打卡(medication_log checkin) / 超时判漏服 -> 记录查询
    //
    // NVS 布局（命名空间 medication）：
    //   reminders             : [{time:"08:00", medicine:"降压药"}]  医嘱计划
    //   logs                  : {"2026-09-23":[{medicine,time}]}    打卡事件流
    //   dYYYYMMDD（≤15字符）  : 当日剂量状态数组
    //     [{medicine, planned, status, reminds, actual, delay}]
    //     status: pending（未到点/未打卡）/ taken（按时打卡）
    //             / late（漏服判定后补卡）/ missed（漏服）
    //
    // 状态机（每条计划独立推进，绝对分钟数判断，避免分钟窗口漏触发）：
    //   到点 -> 提醒第 1 次；每 INTERVAL 分钟未打卡再提醒，最多 MAX_TIMES 次；
    //   超过 MISSED_TIMEOUT 分钟仍未打卡 -> missed，弹窗告警并通知云端。
    // ====================================================================

    esp_timer_handle_t medication_timer_ = nullptr;
    std::string medication_last_minute_;

    // 日期 -> 当日剂量状态 NVS 键，如 "d20260923"（NVS 键最长 15 字符）
    static std::string MedicationDoseKey(time_t t) {
        struct tm tm_info = {};
        localtime_r(&t, &tm_info);
        char buf[12] = {0};
        strftime(buf, sizeof(buf), "d%Y%m%d", &tm_info);
        return buf;
    }

    static std::string MedicationDate(time_t t) {
        struct tm tm_info = {};
        localtime_r(&t, &tm_info);
        char buf[16] = {0};
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_info);
        return buf;
    }

    static int ParseHHMM(const std::string& hm) {
        if (hm.size() < 5 || hm[2] != ':') {
            return -1;
        }
        int h = (hm[0] - '0') * 10 + (hm[1] - '0');
        int m = (hm[3] - '0') * 10 + (hm[4] - '0');
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return -1;
        }
        return h * 60 + m;
    }

    // 老人打卡时联动当日剂量状态。静态：MCP 回调是无捕获 lambda。
    static void RecordMedicationCheckin(const std::string& medicine, time_t now) {
        Settings settings("medication", true);
        std::string key = MedicationDoseKey(now);
        cJSON* doses = cJSON_Parse(settings.GetString(key, "[]").c_str());
        if (doses == nullptr || !cJSON_IsArray(doses)) {
            if (doses)
                cJSON_Delete(doses);
            return;  // 当天没有计划剂量（属于计划外打卡，只写 logs）
        }

        struct tm tm_info = {};
        localtime_r(&now, &tm_info);
        int now_min = tm_info.tm_hour * 60 + tm_info.tm_min;
        char clock[8] = {0};
        strftime(clock, sizeof(clock), "%H:%M", &tm_info);

        auto match_dose = [&](const char* want_status) -> cJSON* {
            cJSON* elem = nullptr;
            cJSON_ArrayForEach (elem, doses) {
                cJSON* med = cJSON_GetObjectItem(elem, "medicine");
                cJSON* st = cJSON_GetObjectItem(elem, "status");
                if (med && cJSON_IsString(med) && st && cJSON_IsString(st) &&
                    medicine == med->valuestring && strcmp(st->valuestring, want_status) == 0) {
                    return elem;
                }
            }
            return nullptr;
        };

        // 优先匹配仍在等待打卡的剂量；其次匹配已判漏服的（补卡 -> late 迟服）
        cJSON* target = match_dose("pending");
        const char* new_status = "taken";
        if (target == nullptr) {
            target = match_dose("missed");
            new_status = "late";
        }
        if (target == nullptr) {
            cJSON_Delete(doses);
            return;
        }

        cJSON* planned = cJSON_GetObjectItem(target, "planned");
        int planned_min =
            (planned && cJSON_IsString(planned)) ? ParseHHMM(planned->valuestring) : -1;
        // 延迟分钟数（提前吃药记 0）
        int delay = 0;
        if (planned_min >= 0 && now_min > planned_min) {
            delay = now_min - planned_min;
        }

        cJSON_ReplaceItemInObject(target, "status", cJSON_CreateString(new_status));
        cJSON_ReplaceItemInObject(target, "actual", cJSON_CreateString(clock));
        cJSON_ReplaceItemInObject(target, "delay", cJSON_CreateNumber(delay));

        StoreJson(settings, "medication_checkin", key.c_str(), doses);
        cJSON_Delete(doses);
        ESP_LOGI(TAG, "medication: checkin %s -> %s, delay=%d min", medicine.c_str(), new_status,
                 delay);
    }

    // 今日服药报告：医嘱剂量状态 + 计划外打卡 + 汇总计数
    static cJSON* BuildMedicationReport(time_t now) {
        Settings settings("medication", true);
        std::string key = MedicationDoseKey(now);
        cJSON* doses = cJSON_Parse(settings.GetString(key, "[]").c_str());
        if (doses == nullptr || !cJSON_IsArray(doses)) {
            if (doses)
                cJSON_Delete(doses);
            doses = cJSON_CreateArray();
        }

        cJSON* report = cJSON_CreateObject();
        cJSON_AddStringToObject(report, "date", MedicationDate(now).c_str());

        int n_total = 0, n_taken = 0, n_late = 0, n_missed = 0, n_pending = 0;
        cJSON* elem = nullptr;
        cJSON_ArrayForEach (elem, doses) {
            n_total++;
            cJSON* st = cJSON_GetObjectItem(elem, "status");
            if (st && cJSON_IsString(st)) {
                if (strcmp(st->valuestring, "taken") == 0) {
                    n_taken++;
                } else if (strcmp(st->valuestring, "late") == 0) {
                    n_late++;
                } else if (strcmp(st->valuestring, "missed") == 0) {
                    n_missed++;
                } else {
                    n_pending++;
                }
            }
        }
        cJSON_AddItemToObject(report, "doses", doses);  // 移交所有权
        cJSON_AddNumberToObject(report, "total", n_total);
        cJSON_AddNumberToObject(report, "taken", n_taken);
        cJSON_AddNumberToObject(report, "late", n_late);
        cJSON_AddNumberToObject(report, "missed", n_missed);
        cJSON_AddNumberToObject(report, "pending", n_pending);

        // 计划外打卡：logs 里有、但不在当日医嘱剂量中的药
        cJSON* unplanned = cJSON_CreateArray();
        cJSON* logs = cJSON_Parse(settings.GetString("logs", "{}").c_str());
        if (logs && cJSON_IsObject(logs)) {
            cJSON* day_logs = cJSON_GetObjectItem(logs, MedicationDate(now).c_str());
            if (day_logs && cJSON_IsArray(day_logs)) {
                cJSON* log_elem = nullptr;
                cJSON_ArrayForEach (log_elem, day_logs) {
                    const char* name = nullptr;
                    const char* at = "";
                    if (cJSON_IsString(log_elem)) {
                        name = log_elem->valuestring;
                    } else if (cJSON_IsObject(log_elem)) {
                        cJSON* m = cJSON_GetObjectItem(log_elem, "medicine");
                        cJSON* t = cJSON_GetObjectItem(log_elem, "time");
                        name = cJSON_IsString(m) ? m->valuestring : nullptr;
                        at = cJSON_IsString(t) ? t->valuestring : "";
                    }
                    if (name == nullptr) {
                        continue;
                    }
                    bool in_plan = false;
                    cJSON_ArrayForEach (elem, doses) {
                        cJSON* dmed = cJSON_GetObjectItem(elem, "medicine");
                        if (dmed && cJSON_IsString(dmed) && strcmp(dmed->valuestring, name) == 0) {
                            in_plan = true;
                            break;
                        }
                    }
                    if (!in_plan) {
                        cJSON* item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "medicine", name);
                        cJSON_AddStringToObject(item, "time", at);
                        cJSON_AddItemToArray(unplanned, item);
                    }
                }
            }
        }
        if (logs)
            cJSON_Delete(logs);
        cJSON_AddItemToObject(report, "unplanned", unplanned);
        return report;
    }

    // 历史记录：最近 days 天（含今天）的剂量记录，按日期升序
    static cJSON* BuildMedicationHistory(int days, time_t now) {
        if (days < 1) {
            days = 1;
        }
        if (days > CONFIG_MEDICATION_LOG_KEEP_DAYS) {
            days = CONFIG_MEDICATION_LOG_KEEP_DAYS;
        }
        Settings settings("medication", true);
        cJSON* history = cJSON_CreateArray();
        for (int age = days - 1; age >= 0; age--) {
            time_t day_time = now - age * 86400;
            std::string raw = settings.GetString(MedicationDoseKey(day_time), "");
            if (raw.empty()) {
                continue;
            }
            cJSON* doses = cJSON_Parse(raw.c_str());
            if (doses == nullptr || !cJSON_IsArray(doses)) {
                if (doses)
                    cJSON_Delete(doses);
                continue;
            }
            cJSON* day_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(day_obj, "date", MedicationDate(day_time).c_str());
            cJSON_AddItemToObject(day_obj, "doses", doses);
            cJSON_AddItemToArray(history, day_obj);
        }
        return history;
    }

    static void MedicationTimerCb(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
        self->CheckMedicationReminders();
    }

    void FireMedicationDue(const std::string& medicine, const std::string& planned,
                           int remind_times) {
        std::string message = "该吃" + medicine + "了（计划 " + planned +
                              "）。"
                              "吃完请对我说：我吃过" +
                              medicine + "了。";
        if (remind_times > 1) {
            message = "提醒第 " + std::to_string(remind_times) + " 次：" + message;
        }
        Application::GetInstance().Schedule([message, medicine, planned]() {
            ShowElderAlert("服药提醒", message.c_str(), "info");
            // 通知云端，服务器配置自动化后可触发语音询问
            cJSON* msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "medication_due");
            cJSON_AddStringToObject(msg, "source", "device");
            cJSON_AddStringToObject(msg, "medicine", medicine.c_str());
            cJSON_AddStringToObject(msg, "planned", planned.c_str());
            char* out = cJSON_PrintUnformatted(msg);
            if (out != nullptr) {
                Application::GetInstance().SendMcpMessage(out);
                free(out);
            } else {
                ESP_LOGE(TAG, "medication_due: serialize failed");
            }
            cJSON_Delete(msg);
        });
    }

    void FireMedicationMissed(const std::string& medicine, const std::string& planned) {
        std::string message = medicine + "（计划 " + planned + "）超过" +
                              std::to_string(CONFIG_MEDICATION_MISSED_TIMEOUT_MIN) +
                              "分钟未打卡，已记为漏服。";
        Application::GetInstance().Schedule([message, medicine, planned]() {
            ShowElderAlert("漏服提醒", message.c_str(), "warning");
            cJSON* msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "medication_missed");
            cJSON_AddStringToObject(msg, "source", "device");
            cJSON_AddStringToObject(msg, "medicine", medicine.c_str());
            cJSON_AddStringToObject(msg, "planned", planned.c_str());
            char* out = cJSON_PrintUnformatted(msg);
            if (out != nullptr) {
                Application::GetInstance().SendMcpMessage(out);
                free(out);
            } else {
                ESP_LOGE(TAG, "medication_missed: serialize failed");
            }
            cJSON_Delete(msg);
        });
    }

    void CheckMedicationReminders() {
        time_t now = time(nullptr);
        if (now < 1700000000) {
            return;  // 系统时间未同步
        }

        struct tm tm_info = {};
        localtime_r(&now, &tm_info);
        char cur_hm[6] = {0};
        strftime(cur_hm, sizeof(cur_hm), "%H:%M", &tm_info);
        std::string cur = cur_hm;
        if (cur == medication_last_minute_) {
            return;  // 同一分钟只处理一次
        }
        medication_last_minute_ = cur;
        int now_min = tm_info.tm_hour * 60 + tm_info.tm_min;

        Settings settings("medication", true);

        // 1. 根据医嘱计划生成当日剂量（计划新增后，第二天/当天首次检查时生效）
        std::string key = MedicationDoseKey(now);
        cJSON* doses = cJSON_Parse(settings.GetString(key, "[]").c_str());
        if (doses == nullptr) {
            doses = cJSON_CreateArray();
        }
        if (!cJSON_IsArray(doses)) {
            cJSON_Delete(doses);
            doses = cJSON_CreateArray();
        }

        bool changed = false;
        cJSON* plans = cJSON_Parse(settings.GetString("reminders", "[]").c_str());
        if (plans != nullptr && cJSON_IsArray(plans)) {
            // 当天是否需要该计划：与云端调度同语义
            // （daily 恒真；weekdays=周一~五；custom 按 weekdays 逗号列表，1=周一）
            struct tm today_tm = {};
            localtime_r(&now, &today_tm);
            int day_value = (today_tm.tm_wday == 0) ? 7 : today_tm.tm_wday;
            char day_str[3] = {0};
            snprintf(day_str, sizeof(day_str), "%d", day_value);

            cJSON* plan = nullptr;
            cJSON_ArrayForEach (plan, plans) {
                cJSON* ptime = cJSON_GetObjectItem(plan, "time");
                cJSON* pmed = cJSON_GetObjectItem(plan, "medicine");
                if (!cJSON_IsString(ptime) || !cJSON_IsString(pmed)) {
                    continue;
                }
                cJSON* rule = cJSON_GetObjectItem(plan, "rule");
                const char* rule_str = (rule && cJSON_IsString(rule)) ? rule->valuestring : "daily";
                if (strcmp(rule_str, "weekdays") == 0) {
                    if (day_value > 5)
                        continue;  // 周末不提醒
                } else if (strcmp(rule_str, "custom") == 0) {
                    cJSON* wd = cJSON_GetObjectItem(plan, "weekdays");
                    bool today = false;
                    if (wd && cJSON_IsString(wd)) {
                        // 逗号分隔的星期列表，逐段比对（避免 "1" 匹配到 "11" 之类）
                        std::string list = wd->valuestring;
                        size_t pos = 0;
                        while (pos < list.size()) {
                            size_t comma = list.find(',', pos);
                            std::string token = (comma == std::string::npos)
                                                    ? list.substr(pos)
                                                    : list.substr(pos, comma - pos);
                            // 去掉首尾空格
                            size_t b = token.find_first_not_of(" \t");
                            size_t e = token.find_last_not_of(" \t");
                            if (b != std::string::npos && token.substr(b, e - b + 1) == day_str) {
                                today = true;
                                break;
                            }
                            pos = (comma == std::string::npos) ? list.size() : comma + 1;
                        }
                    }
                    if (!today)
                        continue;
                }
                bool exists = false;
                cJSON* elem = nullptr;
                cJSON_ArrayForEach (elem, doses) {
                    cJSON* dmed = cJSON_GetObjectItem(elem, "medicine");
                    cJSON* dplanned = cJSON_GetObjectItem(elem, "planned");
                    if (dmed && cJSON_IsString(dmed) && dplanned && cJSON_IsString(dplanned) &&
                        strcmp(dmed->valuestring, pmed->valuestring) == 0 &&
                        strcmp(dplanned->valuestring, ptime->valuestring) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    cJSON* dose = cJSON_CreateObject();
                    cJSON_AddStringToObject(dose, "medicine", pmed->valuestring);
                    cJSON_AddStringToObject(dose, "planned", ptime->valuestring);
                    cJSON_AddStringToObject(dose, "status", "pending");
                    cJSON_AddNumberToObject(dose, "reminds", 0);
                    cJSON_AddStringToObject(dose, "actual", "");
                    cJSON_AddNumberToObject(dose, "delay", -1);
                    cJSON_AddItemToArray(doses, dose);
                    changed = true;
                }
            }
        }
        if (plans)
            cJSON_Delete(plans);
        if (changed) {
            StoreJson(settings, "medication_task", key.c_str(), doses);
        }

        // 2. 清理过期记录（保留 CONFIG_MEDICATION_LOG_KEEP_DAYS 天）
        for (int age = CONFIG_MEDICATION_LOG_KEEP_DAYS; age <= CONFIG_MEDICATION_LOG_KEEP_DAYS + 2;
             age++) {
            settings.EraseKey(MedicationDoseKey(now - age * 86400));
        }

        // 3. 逐条推进状态机
        cJSON* elem = nullptr;
        cJSON_ArrayForEach (elem, doses) {
            cJSON* st = cJSON_GetObjectItem(elem, "status");
            cJSON* planned = cJSON_GetObjectItem(elem, "planned");
            cJSON* med = cJSON_GetObjectItem(elem, "medicine");
            if (!cJSON_IsString(st) || !cJSON_IsString(planned) || !cJSON_IsString(med) ||
                strcmp(st->valuestring, "pending") != 0) {
                continue;
            }
            int planned_min = ParseHHMM(planned->valuestring);
            if (planned_min < 0) {
                continue;
            }
            int delta = now_min - planned_min;
            if (delta < 0) {
                continue;  // 还没到点
            }

            std::string medicine = med->valuestring;
            std::string planned_hm = planned->valuestring;

            if (delta >= CONFIG_MEDICATION_MISSED_TIMEOUT_MIN) {
                // 超时未打卡 -> 漏服
                cJSON_ReplaceItemInObject(elem, "status", cJSON_CreateString("missed"));
                cJSON_ReplaceItemInObject(elem, "delay", cJSON_CreateNumber(delta));
                StoreJson(settings, "medication_task", key.c_str(), doses);
                ESP_LOGW(TAG, "medication: MISSED %s planned=%s", medicine.c_str(),
                         planned_hm.c_str());
                FireMedicationMissed(medicine, planned_hm);
                continue;
            }

            // 应提醒次数：首次 1 次，之后每 INTERVAL 分钟 +1，封顶 MAX_TIMES
            int expected = 1 + delta / CONFIG_MEDICATION_REMIND_REPEAT_MINUTES;
            if (expected > CONFIG_MEDICATION_REMIND_MAX_TIMES) {
                expected = CONFIG_MEDICATION_REMIND_MAX_TIMES;
            }
            cJSON* reminds_item = cJSON_GetObjectItem(elem, "reminds");
            int reminds =
                (reminds_item && cJSON_IsNumber(reminds_item)) ? reminds_item->valueint : 0;
            if (reminds < expected) {
                cJSON_ReplaceItemInObject(elem, "reminds", cJSON_CreateNumber(expected));
                StoreJson(settings, "medication_task", key.c_str(), doses);
                ESP_LOGI(TAG, "medication: remind #%d for %s planned=%s", expected,
                         medicine.c_str(), planned_hm.c_str());
                FireMedicationDue(medicine, planned_hm, expected);
            }
        }
        cJSON_Delete(doses);
    }

    void StartMedicationReminder() {
        esp_timer_create_args_t args = {};
        args.callback = MedicationTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "med_rem";
        esp_err_t err = esp_timer_create(&args, &medication_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "medication_reminder: create timer failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_timer_start_periodic(medication_timer_, 60 * 1000 * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "medication_reminder: start timer failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "medication_reminder: timer started, checking every 60s");
    }
#endif  // CONFIG_ENABLE_MEDICATION_REMINDER_TASK

#ifdef CONFIG_ENABLE_SEDENTARY_REMINDER
    // ====================================================================
    // 久坐提醒后台任务：每隔设定时间拍照分析是否久坐，是则提醒
    // ====================================================================
    esp_timer_handle_t sedentary_timer_ = nullptr;
    std::atomic<bool> sedentary_running_{false};

    static void SedentaryTimerCb(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
        self->TryStartSedentaryCheck();
    }

    void TryStartSedentaryCheck() {
        bool expected = false;
        if (!sedentary_running_.compare_exchange_strong(expected, true)) {
            return;
        }
        BaseType_t ret = xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
                self->RunSedentaryCheck();
                self->sedentary_running_.store(false);
                vTaskDelete(NULL);
            },
            "sedentary", 8192, this, 2, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "sedentary: failed to create task");
            sedentary_running_.store(false);
        }
    }

    void RunSedentaryCheck() {
        if (camera_ == nullptr) {
            return;
        }
        // 视觉互斥：手动视觉工具可能正在用摄像头，跳过本轮
        CameraLockGuard guard(camera_);
        if (!guard.Held()) {
            ESP_LOGI(TAG, "sedentary: camera busy, skip this round");
            return;
        }
        TaskPriorityReset priority_reset(1);
        if (!camera_->Capture()) {
            ESP_LOGW(TAG, "sedentary: capture failed");
            return;
        }
        std::string prompt =
            "观察画面中的人，判断他是否长时间坐着或躺着没有明显活动。"
            "如果是，返回 JSON：{\"sedentary\": true}；否则 {\"sedentary\": false}。";
        auto result = camera_->Explain(prompt);
        if (!result) {
            ESP_LOGW(TAG, "sedentary: explain failed: %s", result.error().c_str());
            return;
        }
        bool sedentary = false;
        std::string model_text;
        cJSON* envelope = cJSON_Parse(result->c_str());
        if (envelope != nullptr) {
            cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
            model_text = (text_item != nullptr && cJSON_IsString(text_item))
                             ? text_item->valuestring
                             : *result;
            cJSON_Delete(envelope);
        } else {
            model_text = *result;
        }
        size_t begin = model_text.find('{');
        size_t end = model_text.rfind('}');
        if (begin != std::string::npos && end != std::string::npos && end > begin) {
            cJSON* root = cJSON_Parse(model_text.substr(begin, end - begin + 1).c_str());
            if (root != nullptr) {
                cJSON* s = cJSON_GetObjectItem(root, "sedentary");
                if (s != nullptr && cJSON_IsBool(s)) {
                    sedentary = cJSON_IsTrue(s);
                }
                cJSON_Delete(root);
            }
        }
        if (sedentary) {
            ESP_LOGW(TAG, "sedentary_reminder: person inactive, reminding");
            Application::GetInstance().Schedule(
                []() { ShowElderAlert("温馨提醒", "您已经坐了很久了，起来活动活动吧！", "info"); });
        }
    }

    void StartSedentaryReminder() {
        esp_timer_create_args_t args = {};
        args.callback = SedentaryTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "sedentary";
        esp_err_t err = esp_timer_create(&args, &sedentary_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sedentary: create timer failed: %s", esp_err_to_name(err));
            return;
        }
        err =
            esp_timer_start_periodic(sedentary_timer_, CONFIG_SEDENTARY_REMINDER_PERIOD_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sedentary: start timer failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "sedentary_reminder: timer started, period=%d ms",
                 CONFIG_SEDENTARY_REMINDER_PERIOD_MS);
    }
#endif  // CONFIG_ENABLE_SEDENTARY_REMINDER

#ifdef CONFIG_ENABLE_BED_EXIT_DETECTION
    // ====================================================================
    // 离床检测后台任务：摄像头对准床铺，定时判断床上是否有人
    // 连续无人超过阈值则告警（夜间跌倒/走失看护）
    // ====================================================================
    esp_timer_handle_t bed_exit_timer_ = nullptr;
    std::atomic<bool> bed_exit_running_{false};
    int bed_empty_count_ = 0;

    static void BedExitTimerCb(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
        self->TryStartBedExitCheck();
    }

    void TryStartBedExitCheck() {
        bool expected = false;
        if (!bed_exit_running_.compare_exchange_strong(expected, true)) {
            return;
        }
        BaseType_t ret = xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
                self->RunBedExitCheck();
                self->bed_exit_running_.store(false);
                vTaskDelete(NULL);
            },
            "bed_exit", 8192, this, 2, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "bed_exit: failed to create task");
            bed_exit_running_.store(false);
        }
    }

    void RunBedExitCheck() {
        if (camera_ == nullptr) {
            return;
        }
        // 视觉互斥：手动视觉工具可能正在用摄像头，跳过本轮
        CameraLockGuard guard(camera_);
        if (!guard.Held()) {
            ESP_LOGI(TAG, "bed_exit: camera busy, skip this round");
            return;
        }
        TaskPriorityReset priority_reset(1);
        if (!camera_->Capture()) {
            ESP_LOGW(TAG, "bed_exit: capture failed");
            return;
        }
        std::string prompt =
            "观察画面，判断床上是否有人躺着。"
            "如果床上有人，返回 JSON：{\"in_bed\": true}；"
            "如果床上没人，返回 JSON：{\"in_bed\": false}。";
        auto result = camera_->Explain(prompt);
        if (!result) {
            ESP_LOGW(TAG, "bed_exit: explain failed: %s", result.error().c_str());
            return;
        }
        bool in_bed = true;  // 默认有人，避免误报
        std::string model_text;
        cJSON* envelope = cJSON_Parse(result->c_str());
        if (envelope != nullptr) {
            cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
            model_text = (text_item != nullptr && cJSON_IsString(text_item))
                             ? text_item->valuestring
                             : *result;
            cJSON_Delete(envelope);
        } else {
            model_text = *result;
        }
        size_t begin = model_text.find('{');
        size_t end = model_text.rfind('}');
        if (begin != std::string::npos && end != std::string::npos && end > begin) {
            cJSON* root = cJSON_Parse(model_text.substr(begin, end - begin + 1).c_str());
            if (root != nullptr) {
                cJSON* ib = cJSON_GetObjectItem(root, "in_bed");
                if (ib != nullptr && cJSON_IsBool(ib)) {
                    in_bed = cJSON_IsTrue(ib);
                }
                cJSON_Delete(root);
            }
        }
        if (in_bed) {
            if (bed_empty_count_ > 0) {
                ESP_LOGI(TAG, "bed_exit: person returned to bed");
            }
            bed_empty_count_ = 0;
        } else {
            bed_empty_count_++;
            ESP_LOGI(TAG, "bed_exit: bed empty, count=%d", bed_empty_count_);
            int threshold =
                CONFIG_BED_EXIT_EMPTY_TIMEOUT_S * 1000 / CONFIG_BED_EXIT_CHECK_PERIOD_MS;
            if (bed_empty_count_ >= threshold) {
                ESP_LOGW(TAG, "bed_exit: bed empty too long, alerting!");
                bed_empty_count_ = 0;
                Application::GetInstance().Schedule([]() {
                    ShowElderAlert("离床告警", "老人已离床较长时间未返回，请确认是否安全。",
                                   "warning");
                    Application::GetInstance().SendMcpMessage(
                        "{\"type\":\"bed_exit_alert\",\"source\":\"device\"}");
                });
            }
        }
    }

    void StartBedExitDetection() {
        esp_timer_create_args_t args = {};
        args.callback = BedExitTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "bed_exit";
        esp_err_t err = esp_timer_create(&args, &bed_exit_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bed_exit: create timer failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_timer_start_periodic(bed_exit_timer_, CONFIG_BED_EXIT_CHECK_PERIOD_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bed_exit: start timer failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "bed_exit_detection: timer started, period=%d ms",
                 CONFIG_BED_EXIT_CHECK_PERIOD_MS);
    }
#endif  // CONFIG_ENABLE_BED_EXIT_DETECTION

#ifdef CONFIG_ENABLE_CARE_SYNC
    // ====================================================================
    // 关怀云端同步：与服务端老年关怀模块 REST 接口双向同步
    // --------------------------------------------------------------------
    // 服务端接口（/api/device/care/*，Device-Id=MAC 鉴权，无需登录态）：
    //   GET  /api/device/care/profile      老人档案（姓名/健康备注）
    //   GET  /api/device/care/plan         启用中的服药计划（云端为准）
    //   POST /api/device/care/plan         新增计划（设备语音设置上报）
    //   DEL  /api/device/care/plan/{id}    删除计划
    //   POST /api/device/care/plan/taken   上报已服药（msgId 幂等）
    //   GET  /api/device/care/messages     拉取关怀留言（代替 MQTT 订阅）
    //
    // 服务端地址从 OTA URL 推导（同主机同端口）：ota_url 末尾去掉
    // /api/device/ota 即得 base。局域网无 DNS 变更时无需额外配置。
    //
    // 数据一致性策略：
    //   - 带 planId 的计划：云端为准（每次同步整体刷新）
    //   - 不带 planId 的本地计划（语音设置时云端不可达）：保留，
    //     下轮同步时补推云端并回填 planId（自愈）
    //   - 打卡/留言按 msgId 幂等，重复请求不会产生重复记录
    // ====================================================================

    // 服务端 base URL，结果缓存（OTA URL 运行期不变）
    static std::string CareApiBase() {
        static std::string cached;
        static bool resolved = false;
        if (resolved) {
            return cached;
        }
        resolved = true;
#ifdef CONFIG_CARE_API_BASE
        if (strlen(CONFIG_CARE_API_BASE) > 0) {
            cached = CONFIG_CARE_API_BASE;
            ESP_LOGI(TAG, "care_sync: api base = %s (from CONFIG_CARE_API_BASE)", cached.c_str());
            return cached;
        }
#endif
        Settings settings("wifi", false);
        std::string url = settings.GetString("ota_url");
        if (url.empty()) {
            url = CONFIG_OTA_URL;
        }
        const std::string suffix = "/api/device/ota";
        if (url.size() > suffix.size() &&
            url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0) {
            cached = url.substr(0, url.size() - suffix.size());
        } else {
            // 兜底：scheme://host[:port]/... -> scheme://host[:port]
            cached = url;
            size_t scheme_end = cached.find("://");
            size_t path = (scheme_end == std::string::npos) ? std::string::npos
                                                            : cached.find('/', scheme_end + 3);
            if (path != std::string::npos) {
                cached = cached.substr(0, path);
            }
        }
        ESP_LOGI(TAG, "care_sync: api base = %s", cached.c_str());
        return cached;
    }

    // 统一 HTTP 响应解包：{code:200, data:...} -> data（调用方负责释放）
    static cJSON* CareUnwrap(cJSON* envelope) {
        if (envelope == nullptr) {
            return nullptr;
        }
        cJSON* code = cJSON_GetObjectItem(envelope, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 200) {
            cJSON_Delete(envelope);
            return nullptr;
        }
        cJSON* data = cJSON_GetObjectItem(envelope, "data");
        if (data == nullptr) {
            cJSON_Delete(envelope);
            return nullptr;
        }
        cJSON* detached = cJSON_DetachItemViaPointer(envelope, data);
        cJSON_Delete(envelope);
        return detached;
    }

    // GET {base}{path}，返回 data 部分（无数据/失败返回 nullptr）
    static cJSON* CareGetJson(const char* path) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            return nullptr;
        }
        auto http = network->CreateHttp(10);
        if (http == nullptr) {
            return nullptr;
        }
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
        auto opened = http->Open("GET", CareApiBase() + path);
        if (!opened) {
            ESP_LOGW(TAG, "care_sync: GET %s failed: %s", path, opened.error().ToString().c_str());
            http->Close();
            return nullptr;
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            ESP_LOGW(TAG, "care_sync: GET %s -> HTTP %d", path, status ? *status : -1);
            http->Close();
            return nullptr;
        }
        std::string body = http->ReadAll();
        http->Close();
        return CareUnwrap(cJSON_Parse(body.c_str()));
    }

    // POST/DELETE JSON body，envelope code==200 视为成功
    static bool CareSendJson(const char* method, const char* path, cJSON* body) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            return false;
        }
        auto http = network->CreateHttp(10);
        if (http == nullptr) {
            return false;
        }
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
        http->SetHeader("Content-Type", "application/json");
        char* out = cJSON_PrintUnformatted(body);
        std::string content = (out != nullptr) ? out : "{}";
        free(out);
        http->SetContent(std::move(content));
        auto opened = http->Open(method, CareApiBase() + path);
        if (!opened) {
            ESP_LOGW(TAG, "care_sync: %s %s failed: %s", method, path,
                     opened.error().ToString().c_str());
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        std::string resp = http->ReadAll();
        http->Close();
        if (!status || *status != 200) {
            ESP_LOGW(TAG, "care_sync: %s %s -> HTTP %d", method, path, status ? *status : -1);
            return false;
        }
        cJSON* envelope = cJSON_Parse(resp.c_str());
        bool ok = (envelope != nullptr);
        cJSON_Delete(envelope);
        return ok;
    }

    // 上报一条服药计划到云端，成功时回填 planId（静态：MCP 回调是无捕获 lambda）
    static bool CarePushPlan(const char* medicine, const char* take_time, std::string* plan_id) {
        cJSON* body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "medicine", medicine);
        cJSON_AddStringToObject(body, "takeTime", take_time);
        cJSON_AddStringToObject(body, "repeatRule", "daily");
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            cJSON_Delete(body);
            return false;
        }
        auto http = network->CreateHttp(10);
        if (http == nullptr) {
            cJSON_Delete(body);
            return false;
        }
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
        http->SetHeader("Content-Type", "application/json");
        char* out = cJSON_PrintUnformatted(body);
        std::string content = (out != nullptr) ? out : "{}";
        free(out);
        cJSON_Delete(body);
        http->SetContent(std::move(content));
        auto opened = http->Open("POST", CareApiBase() + "/api/device/care/plan");
        if (!opened) {
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        std::string resp = http->ReadAll();
        http->Close();
        if (!status || *status != 200) {
            ESP_LOGW(TAG, "care_sync: push plan -> HTTP %d", status ? *status : -1);
            return false;
        }
        cJSON* data = CareUnwrap(cJSON_Parse(resp.c_str()));
        if (data == nullptr) {
            return false;
        }
        cJSON* pid = cJSON_GetObjectItem(data, "planId");
        if (plan_id != nullptr && cJSON_IsString(pid)) {
            *plan_id = pid->valuestring;
        }
        cJSON_Delete(data);
        return true;
    }

    // 删除云端计划；200=已删除，404=云端本就不存在（care_sync 可能已按"云端为准"
    // 清理过），均视为成功
    static bool CareDeletePlan(const std::string& plan_id) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            return false;
        }
        auto http = network->CreateHttp(10);
        if (http == nullptr) {
            return false;
        }
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
        auto opened = http->Open("DELETE", CareApiBase() + "/api/device/care/plan/" + plan_id);
        if (!opened) {
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        http->ReadAll();
        http->Close();
        return status && (*status == 200 || *status == 404);
    }

    // 待删除对账队列（tombstone）：本地已删但云端尚未确认删除的 planId。
    // 持久化到 NVS 跨重启兜底；care_sync 每拍对账重试，成功或确认云端已无后清除。
    // 上限 16 条，超出丢弃最旧记录。
    static constexpr size_t kMaxPendingDeletes = 16;

    static std::vector<std::string> LoadPendingDeletes() {
        std::vector<std::string> ids;
        Settings settings("medication", true);
        cJSON* arr = cJSON_Parse(settings.GetString("pending_deletes", "[]").c_str());
        if (arr != nullptr && cJSON_IsArray(arr)) {
            cJSON* e = nullptr;
            cJSON_ArrayForEach (e, arr) {
                if (cJSON_IsString(e) && e->valuestring[0] != '\0') {
                    ids.emplace_back(e->valuestring);
                }
            }
        }
        cJSON_Delete(arr);
        return ids;
    }

    static void SavePendingDeletes(const std::vector<std::string>& ids) {
        cJSON* arr = cJSON_CreateArray();
        size_t start = ids.size() > kMaxPendingDeletes ? ids.size() - kMaxPendingDeletes : 0;
        for (size_t i = start; i < ids.size(); ++i) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(ids[i].c_str()));
        }
        char* out = cJSON_PrintUnformatted(arr);
        Settings settings("medication", true);
        settings.SetString("pending_deletes", out != nullptr ? out : "[]");
        free(out);
        cJSON_Delete(arr);
    }

    static void AddPendingDelete(const std::string& plan_id) {
        auto ids = LoadPendingDeletes();
        for (const auto& id : ids) {
            if (id == plan_id) {
                return;  // 幂等：已记录不重复
            }
        }
        ids.emplace_back(plan_id);
        SavePendingDeletes(ids);
    }

    static void RemovePendingDelete(const std::string& plan_id) {
        auto ids = LoadPendingDeletes();
        for (auto it = ids.begin(); it != ids.end(); ++it) {
            if (*it == plan_id) {
                ids.erase(it);
                SavePendingDeletes(ids);
                return;
            }
        }
    }

    // 上报服药打卡（云端按 msgId 幂等：每天每药最多一条记录）
    // 静态：MCP 回调是无捕获 lambda
    static void CareReportTaken(const std::string& medicine, time_t now) {
        char day[16] = {0};
        strftime(day, sizeof(day), "%Y%m%d", localtime(&now));
        cJSON* body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "medicine", medicine.c_str());
        std::string msg_id = std::string("chk-") + day + "-" + medicine;
        cJSON_AddStringToObject(body, "msgId", msg_id.c_str());
        if (CareSendJson("POST", "/api/device/care/plan/taken", body)) {
            ESP_LOGI(TAG, "care_sync: taken reported (%s)", msg_id.c_str());
        } else {
            ESP_LOGW(TAG, "care_sync: taken report failed (%s)", msg_id.c_str());
        }
        cJSON_Delete(body);
    }

    // 拉取云端启用中的服药计划，与本地 NVS 合并：
    // 带 planId 的条目以云端为准；本地无 planId 条目保留并补推云端
    void SyncMedicationPlansFromCloud() {
        cJSON* server_plans = CareGetJson("/api/device/care/plan");
        if (server_plans == nullptr || !cJSON_IsArray(server_plans)) {
            if (server_plans != nullptr)
                cJSON_Delete(server_plans);
            ESP_LOGW(TAG, "care_sync: fetch plans failed, keep local data");
            return;
        }

        Settings settings("medication", true);
        cJSON* local = cJSON_Parse(settings.GetString("reminders", "[]").c_str());
        if (local == nullptr || !cJSON_IsArray(local)) {
            if (local != nullptr)
                cJSON_Delete(local);
            local = cJSON_CreateArray();
        }

        // 云端计划归一化为本地格式 {time, medicine, planId, rule, weekdays}
        cJSON* cloud = cJSON_CreateArray();
        cJSON* elem = nullptr;
        cJSON_ArrayForEach (elem, server_plans) {
            cJSON* pid = cJSON_GetObjectItem(elem, "planId");
            cJSON* med = cJSON_GetObjectItem(elem, "medicine");
            cJSON* tt = cJSON_GetObjectItem(elem, "takeTime");
            if (!cJSON_IsString(pid) || !cJSON_IsString(med) || !cJSON_IsString(tt)) {
                continue;
            }
            cJSON* item = cJSON_CreateObject();
            std::string take = tt->valuestring;  // "HH:MM:SS" -> "HH:MM"
            if (take.size() >= 5) {
                take = take.substr(0, 5);
            }
            cJSON_AddStringToObject(item, "time", take.c_str());
            cJSON_AddStringToObject(item, "medicine", med->valuestring);
            cJSON_AddStringToObject(item, "planId", pid->valuestring);
            cJSON* rule = cJSON_GetObjectItem(elem, "repeatRule");
            cJSON_AddStringToObject(item, "rule",
                                    (rule && cJSON_IsString(rule)) ? rule->valuestring : "daily");
            cJSON* wd = cJSON_GetObjectItem(elem, "weekdays");
            if (wd && cJSON_IsString(wd) && wd->valuestring[0] != '\0') {
                cJSON_AddStringToObject(item, "weekdays", wd->valuestring);
            }
            cJSON_AddItemToArray(cloud, item);
        }
        cJSON_Delete(server_plans);

        // 本地删除对账：tombstone 里的 planId 若云端仍存在则重发 DELETE。
        // 成功 -> 从云端列表剔除（阻止本轮 merge 复活）并清除 tombstone；
        // 失败 -> 保留 tombstone 下拍重试，本轮仍按云端为准合并。
        auto pending = LoadPendingDeletes();
        if (!pending.empty()) {
            std::vector<std::string> still_pending;
            for (const auto& pid : pending) {
                bool in_cloud = false;
                cJSON* ce = nullptr;
                cJSON_ArrayForEach (ce, cloud) {
                    cJSON* p = cJSON_GetObjectItem(ce, "planId");
                    if (p != nullptr && cJSON_IsString(p) && pid == p->valuestring) {
                        in_cloud = true;
                        break;
                    }
                }
                if (!in_cloud) {
                    continue;  // 云端已无此计划，对账完成
                }
                if (CareDeletePlan(pid)) {
                    ESP_LOGI(TAG, "care_sync: pending delete done (%s)", pid.c_str());
                    cJSON_ArrayForEach (ce, cloud) {
                        cJSON* p = cJSON_GetObjectItem(ce, "planId");
                        if (p != nullptr && cJSON_IsString(p) && pid == p->valuestring) {
                            UnlinkAndDeleteItem(cloud, ce);
                            break;
                        }
                    }
                } else {
                    still_pending.emplace_back(pid);
                    ESP_LOGW(TAG, "care_sync: pending delete retry next cycle (%s)", pid.c_str());
                }
            }
            SavePendingDeletes(still_pending);
        }

        // 保留本地计划（无 planId），并尝试补推云端实现自愈
        cJSON* merged = cJSON_CreateArray();
        cJSON* local_elem = nullptr;
        cJSON_ArrayForEach (local_elem, local) {
            cJSON* pid = cJSON_GetObjectItem(local_elem, "planId");
            if (pid != nullptr && cJSON_IsString(pid)) {
                continue;  // 已同步的计划以云端为准
            }
            cJSON* med = cJSON_GetObjectItem(local_elem, "medicine");
            cJSON* tm = cJSON_GetObjectItem(local_elem, "time");
            if (cJSON_IsString(med) && cJSON_IsString(tm)) {
                std::string new_plan_id;
                if (CarePushPlan(med->valuestring, tm->valuestring, &new_plan_id)) {
                    cJSON_AddStringToObject(local_elem, "planId", new_plan_id.c_str());
                    ESP_LOGI(TAG, "care_sync: local plan pushed (%s %s)", med->valuestring,
                             tm->valuestring);
                }
            }
            cJSON_AddItemToArray(merged, cJSON_Duplicate(local_elem, 1));
        }
        cJSON_ArrayForEach (elem, cloud) {
            cJSON_AddItemToArray(merged, cJSON_Duplicate(elem, 1));
        }

        // 内容有变化才写 NVS，减少无谓的磨损
        char* merged_str = cJSON_PrintUnformatted(merged);
        if (merged_str == nullptr) {
            ESP_LOGE(TAG, "care_sync: serialize merged plans failed");
        } else {
            std::string stored_old = settings.GetString("reminders", "[]");
            if (stored_old != merged_str) {
                settings.SetString("reminders", merged_str);
                ESP_LOGI(TAG, "care_sync: plans updated, local=%d merged=%d",
                         cJSON_GetArraySize(local), cJSON_GetArraySize(merged));
            }
            free(merged_str);
        }
        cJSON_Delete(merged);
        cJSON_Delete(local);
        cJSON_Delete(cloud);
    }

    // 拉取家属关怀留言，追加到留言板（按 msgId 幂等去重）
    void PullCareMessages() {
        cJSON* msgs = CareGetJson("/api/device/care/messages");
        if (msgs == nullptr || !cJSON_IsArray(msgs)) {
            if (msgs != nullptr)
                cJSON_Delete(msgs);
            return;  // 无留言或拉取失败，静默
        }

        Settings settings("voice_board", true);
        cJSON* board = cJSON_Parse(settings.GetString("messages", "[]").c_str());
        if (board == nullptr || !cJSON_IsArray(board)) {
            if (board != nullptr)
                cJSON_Delete(board);
            board = cJSON_CreateArray();
        }
        cJSON* seen = cJSON_Parse(settings.GetString("seen", "[]").c_str());
        if (seen == nullptr || !cJSON_IsArray(seen)) {
            if (seen != nullptr)
                cJSON_Delete(seen);
            seen = cJSON_CreateArray();
        }

        auto seen_has = [&seen](const char* id) -> bool {
            cJSON* e = nullptr;
            cJSON_ArrayForEach (e, seen) {
                if (cJSON_IsString(e) && strcmp(e->valuestring, id) == 0) {
                    return true;
                }
            }
            return false;
        };

        time_t now = time(nullptr);
        bool board_changed = false;
        cJSON* elem = nullptr;
        cJSON_ArrayForEach (elem, msgs) {
            cJSON* mid = cJSON_GetObjectItem(elem, "msgId");
            cJSON* text = cJSON_GetObjectItem(elem, "text");
            cJSON* sender = cJSON_GetObjectItem(elem, "sender");
            cJSON* tstr = cJSON_GetObjectItem(elem, "time");
            if (!cJSON_IsString(mid) || !cJSON_IsString(text) || text->valuestring[0] == '\0') {
                continue;
            }
            if (seen_has(mid->valuestring)) {
                continue;
            }
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "sender",
                                    (sender && cJSON_IsString(sender) && sender->valuestring[0])
                                        ? sender->valuestring
                                        : "家人");
            cJSON_AddStringToObject(item, "message", text->valuestring);
            std::string when = (tstr && cJSON_IsString(tstr)) ? tstr->valuestring : "";
            if (when.size() >= 16) {
                when = when.substr(0, 16).replace(10, 1, " ");  // "T" -> " "
            } else {
                char buf[32] = {0};
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", localtime(&now));
                when = buf;
            }
            cJSON_AddStringToObject(item, "time", when.c_str());
            cJSON_AddNumberToObject(item, "epoch", (double)now);
            cJSON_AddItemToArray(board, item);
            cJSON_AddItemToArray(seen, cJSON_CreateString(mid->valuestring));
            board_changed = true;
            ESP_LOGI(TAG, "care_sync: message from %s: %s",
                     cJSON_GetObjectItem(item, "sender")->valuestring, text->valuestring);
        }

        // seen 去重集合封顶 100 条，防止无限增长
        while (cJSON_GetArraySize(seen) > 100) {
            cJSON* first = cJSON_GetArrayItem(seen, 0);
            UnlinkAndDeleteItem(seen, first);
        }
        if (board_changed) {
            PurgeExpiredVoiceMessages(board);
            StoreJson(settings, "care_sync", "messages", board);
        }
        StoreJson(settings, "care_sync", "seen", seen);
        cJSON_Delete(board);
        cJSON_Delete(seen);
        cJSON_Delete(msgs);
    }

    // 拉取老人档案（姓名/健康备注），SOS 告警会播报老人姓名
    void PullElderProfile() {
        cJSON* data = CareGetJson("/api/device/care/profile");
        if (data == nullptr || !cJSON_IsObject(data)) {
            if (data != nullptr)
                cJSON_Delete(data);
            return;
        }
        Settings settings("care", true);
        cJSON* name = cJSON_GetObjectItem(data, "name");
        if (name && cJSON_IsString(name) && name->valuestring[0]) {
            settings.SetString("elder_name", name->valuestring);
        }
        cJSON* note = cJSON_GetObjectItem(data, "healthNote");
        if (note && cJSON_IsString(note)) {
            settings.SetString("health_note", note->valuestring);
        }
        cJSON_Delete(data);
        ESP_LOGI(TAG, "care_sync: elder profile updated");
    }

    void RunCareSyncOnce() {
        PullElderProfile();
        SyncMedicationPlansFromCloud();
        PullCareMessages();
    }

    static void CareSyncTaskEntry(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3Cam*>(arg);
        // 等待 OTA 检查和协议初始化完成（避免并发 HTTP 连接导致内存崩溃）
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
        while (true) {
            // 仅在设备空闲时同步，避免对话中并发 HTTP 与 WebSocket 冲突
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateIdle) {
                self->RunCareSyncOnce();
            } else {
                ESP_LOGI(TAG, "care_sync: device busy (state=%d), skipping", state);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_CARE_SYNC_PERIOD_MINUTES * 60 * 1000));
        }
    }

    void StartCareSync() {
        BaseType_t ret = xTaskCreate(CareSyncTaskEntry, "care_sync", 8192, this, 1, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "care_sync: failed to create task");
            return;
        }
        ESP_LOGI(TAG, "care_sync: task started, period=%d min", CONFIG_CARE_SYNC_PERIOD_MINUTES);
    }
#endif  // CONFIG_ENABLE_CARE_SYNC

public:
    CompactWifiBoardS3Cam()
        : boot_button_(BOOT_BUTTON_GPIO), sos_button_(SOS_BUTTON_GPIO, false, 3000) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeSosButton();
        InitializeCamera();
        InitializeTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }

#ifdef CONFIG_ENABLE_BOARD_FALL_DETECTION
        StartFallDetection();
#endif
#ifdef CONFIG_ENABLE_SCHEDULE_REMINDER
        StartScheduleReminder();
#else
        ESP_LOGW(TAG,
                 "schedule_reminder: ENABLE_SCHEDULE_REMINDER is OFF, "
                 "saved reminders will NOT fire");
#endif
#ifdef CONFIG_ENABLE_MEDICATION_REMINDER_TASK
        StartMedicationReminder();
#endif
#ifdef CONFIG_ENABLE_SEDENTARY_REMINDER
        StartSedentaryReminder();
#endif
#ifdef CONFIG_ENABLE_BED_EXIT_DETECTION
        StartBedExitDetection();
#endif
#ifdef CONFIG_ENABLE_CARE_SYNC
        StartCareSync();
#endif
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                              AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(CompactWifiBoardS3Cam);
