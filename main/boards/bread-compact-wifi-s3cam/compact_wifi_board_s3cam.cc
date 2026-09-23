#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "esp32_camera.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <esp_timer.h>

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
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "CompactWifiBoardS3Cam"

// ------------------------------------------------------------------
// 提醒音重复播放：Alert 自带音效只有一声短音，老人容易漏听。
// 在首次 Alert 之后，用 esp_timer 再补播 extra_times 次，让提醒更醒目。
// sound 必须指向 static 存储期的音效对象（如 &Lang::Sounds::OGG_EXCLAMATION），
// 保证 timer 回调执行时指针有效。可在任意任务上下文调用。
// ------------------------------------------------------------------
static void ScheduleRepeatedSound(const std::string_view* sound,
                                  int extra_times, int interval_ms) {
    for (int i = 1; i <= extra_times; ++i) {
        esp_timer_handle_t timer = nullptr;
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* s = static_cast<const std::string_view*>(arg);
            Application::GetInstance().Schedule([s]() {
                Application::GetInstance().PlaySound(*s);
            });
        };
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "alert_rep";
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, (uint64_t)i * interval_ms * 1000);
        }
    }
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
        explicit CameraLockGuard(Camera* c) : cam(c) {
            held = c ? c->TryLock() : false;
        }
        ~CameraLockGuard() { if (held) cam->Unlock(); }
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
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
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
        sos_button_.OnLongPress([this]() {
            ESP_LOGW(TAG, "SOS button long pressed!");
            // 读取紧急联系人，拼到告警消息里
            std::string contacts_info;
            Settings settings("contacts", false);
            std::string stored = settings.GetString("list", "[]");
            cJSON* root = cJSON_Parse(stored.c_str());
            if (root != nullptr && cJSON_IsArray(root)) {
                cJSON* elem = nullptr;
                int idx = 0;
                cJSON_ArrayForEach(elem, root) {
                    if (idx >= 2) break;  // 最多显示前两个
                    cJSON* name = cJSON_GetObjectItem(elem, "name");
                    cJSON* rel = cJSON_GetObjectItem(elem, "relation");
                    cJSON* phone = cJSON_GetObjectItem(elem, "phone");
                    contacts_info += "联系";
                    if (rel && cJSON_IsString(rel)) contacts_info += rel->valuestring;
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
            if (root) cJSON_Delete(root);

            std::string message = "紧急求助！老人按下了 SOS 键。\n" + contacts_info;
            // UI/音频/MCP 消息必须回主任务
            Application::GetInstance().Schedule([message]() {
                auto& app = Application::GetInstance();
                app.Alert("紧急求助", message.c_str(), "warning",
                          Lang::Sounds::OGG_EXCLAMATION);
                // SOS 必须醒目：共播 3 声，间隔 2 秒
                ScheduleRepeatedSound(&Lang::Sounds::OGG_EXCLAMATION, 2, 2000);
                app.SendMcpMessage(
                    "{\"type\":\"sos_alert\",\"source\":\"device\"}");
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
                    if (root) cJSON_Delete(root);
                    root = cJSON_CreateArray();
                }

                if (action == "add") {
                    if (time.empty() || medicine.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("medicine and time are required for 'add'");
                    }
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "time", time.c_str());
                    cJSON_AddStringToObject(item, "medicine", medicine.c_str());
                    cJSON_AddItemToArray(root, item);
                } else if (action == "remove") {
                    if (time.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("time is required for 'remove'");
                    }
                    cJSON* new_arr = cJSON_CreateArray();
                    cJSON* elem = nullptr;
                    cJSON_ArrayForEach(elem, root) {
                        cJSON* t = cJSON_GetObjectItem(elem, "time");
                        if (t == nullptr || !cJSON_IsString(t) ||
                            strcmp(t->valuestring, time.c_str()) != 0) {
                            cJSON_AddItemReferenceToArray(new_arr, elem);
                        }
                    }
                    cJSON_Delete(root);
                    root = new_arr;
                } else if (action != "list") {
                    cJSON_Delete(root);
                    return std::unexpected("Unknown action: " + action);
                }

                if (action != "list") {
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("reminders", out);
                    free(out);
                }
                return root;
            });

        // 工具 2：跌倒检测（AI 按需触发拍照分析）
        mcp.AddTool(
            "self.fall_detection",
            "拍照并调用云端视觉模型判断画面中是否有人呈跌倒姿态。\n"
            "用于实时检测，不回答跌倒预防/急救知识问题（此类问题请直接回答）。\n"
            "返回 JSON {fell, confidence, description}。",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
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
                    return std::unexpected(std::move(result.error()));
                }
                return std::move(*result);
            });

        // 工具 3：家属留言板（文字版）
        mcp.AddTool(
            "self.family_voice_board",
            "家属留言板。家属可远程为老人添加文字留言，老人按键时设备会朗读。\n"
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
                    if (root) cJSON_Delete(root);
                    root = cJSON_CreateArray();
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
                    cJSON_AddItemToArray(root, item);
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("messages", out);
                    free(out);
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
            "记录和查询老人今天是否已服药（打卡功能）。\n"
            "仅用于 checkin（老人说'我吃过药了'）和 status（查'今天药吃了没'），\n"
            "不回答用药咨询问题（如漏服处理、药物禁忌、何时补服等），\n"
            "此类问题请直接回答，不要调用本工具。\n"
            "Args:\n"
            "  action: 'checkin' | 'status' | 'list_today'\n"
            "  medicine: 药名（checkin 时必填）\n"
            "Return:\n"
            "  list_today 返回今日已打卡的药名列表；status 返回某药今日是否已打卡。",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("medicine", kPropertyTypeString, std::string("")),
            }),
            [](const PropertyList& properties) -> ToolResult {
                auto action = properties["action"].value<std::string>();
                auto medicine = properties["medicine"].value<std::string>();

                Settings settings("medication", true);
                std::string stored = settings.GetString("logs", "{}");
                cJSON* root = cJSON_Parse(stored.c_str());
                if (root == nullptr || !cJSON_IsObject(root)) {
                    if (root) cJSON_Delete(root);
                    root = cJSON_CreateObject();
                }

                time_t now = time(nullptr);
                char today[16] = {0};
                if (now > 1700000000) {
                    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&now));
                } else {
                    snprintf(today, sizeof(today), "unknown");
                }

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
                    // 避免重复打卡
                    cJSON* elem = nullptr;
                    cJSON_ArrayForEach(elem, day_arr) {
                        if (cJSON_IsString(elem) &&
                            strcmp(elem->valuestring, medicine.c_str()) == 0) {
                            char* out = cJSON_PrintUnformatted(root);
                            settings.SetString("logs", out);
                            free(out);
                            return root;
                        }
                    }
                    cJSON_AddItemToArray(day_arr, cJSON_CreateString(medicine.c_str()));
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("logs", out);
                    free(out);
                    return root;
                }

                if (action == "status") {
                    if (medicine.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("medicine is required for 'status'");
                    }
                    cJSON* day_arr = cJSON_GetObjectItem(root, today);
                    bool taken = false;
                    if (day_arr != nullptr && cJSON_IsArray(day_arr)) {
                        cJSON* elem = nullptr;
                        cJSON_ArrayForEach(elem, day_arr) {
                            if (cJSON_IsString(elem) &&
                                strcmp(elem->valuestring, medicine.c_str()) == 0) {
                                taken = true;
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root);
                    std::string result = taken ? "taken" : "not_taken";
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

                cJSON_Delete(root);
                return std::unexpected("Unknown action: " + action);
            });

        // 工具 5：通用日程提醒
        mcp.AddTool(
            "self.schedule_reminder",
            "管理老人的日程提醒（不止吃药，如看病、交水费、生日等）。\n"
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
                    if (root) cJSON_Delete(root);
                    root = cJSON_CreateArray();
                }

                if (action == "add") {
                    if (time.empty() || content.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("time and content are required for 'add'");
                    }
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "time", time.c_str());
                    cJSON_AddStringToObject(item, "content", content.c_str());
                    cJSON_AddItemToArray(root, item);
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("reminders", out);
                    free(out);
                    return root;
                }

                if (action == "remove") {
                    if (time.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("time is required for 'remove'");
                    }
                    cJSON* new_arr = cJSON_CreateArray();
                    cJSON* elem = nullptr;
                    cJSON_ArrayForEach(elem, root) {
                        cJSON* t = cJSON_GetObjectItem(elem, "time");
                        if (t == nullptr || !cJSON_IsString(t) ||
                            strcmp(t->valuestring, time.c_str()) != 0) {
                            cJSON_AddItemReferenceToArray(new_arr, elem);
                        }
                    }
                    cJSON_Delete(root);
                    root = new_arr;
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("reminders", out);
                    free(out);
                    return root;
                }

                if (action == "list") {
                    return root;
                }

                cJSON_Delete(root);
                return std::unexpected("Unknown action: " + action);
            });

        // 工具 6：紧急联系人管理
        mcp.AddTool(
            "self.emergency_contact",
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
                    if (root) cJSON_Delete(root);
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
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("list", out);
                    free(out);
                    return root;
                }

                if (action == "remove") {
                    if (name.empty()) {
                        cJSON_Delete(root);
                        return std::unexpected("name is required for 'remove'");
                    }
                    cJSON* new_arr = cJSON_CreateArray();
                    cJSON* elem = nullptr;
                    cJSON_ArrayForEach(elem, root) {
                        cJSON* n = cJSON_GetObjectItem(elem, "name");
                        if (n == nullptr || !cJSON_IsString(n) ||
                            strcmp(n->valuestring, name.c_str()) != 0) {
                            cJSON_AddItemReferenceToArray(new_arr, elem);
                        }
                    }
                    cJSON_Delete(root);
                    root = new_arr;
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("list", out);
                    free(out);
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
            [this](const PropertyList& properties) -> ToolResult {
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
                std::string prompt =
                    "请仔细观察画面，帮忙寻找「" + item +
                    "」。如果找到了，请描述它在画面中的位置"
                    "（如'在左边的桌子上'）；如果没找到，请说'没看到'。"
                    "请用简短的中文回答。";
                auto result = camera->Explain(prompt);
                if (!result) {
                    std::string err = result.error();
                    // 服务器限流（429）时给 AI 一个可转述的友好提示
                    if (err.find("Failed to upload photo") != std::string::npos) {
                        return std::unexpected(
                            "Photo service is rate limited, please tell the user to wait "
                            "tens of seconds and try again");
                    }
                    return std::unexpected(err);
                }
                // Explain 返回的是信封 JSON，取 text 字段
                std::string model_text;
                cJSON* envelope = cJSON_Parse(result->c_str());
                if (envelope != nullptr) {
                    cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
                    model_text = (text_item && cJSON_IsString(text_item))
                                     ? text_item->valuestring : *result;
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
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
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
                    std::string err = result.error();
                    // 服务器限流（429）时给 AI 一个可转述的友好提示
                    if (err.find("Failed to upload photo") != std::string::npos) {
                        return std::unexpected(
                            "Photo service is rate limited, please tell the user to wait "
                            "tens of seconds and try again");
                    }
                    return std::unexpected(err);
                }
                std::string model_text;
                cJSON* envelope = cJSON_Parse(result->c_str());
                if (envelope != nullptr) {
                    cJSON* text_item = cJSON_GetObjectItem(envelope, "text");
                    model_text = (text_item && cJSON_IsString(text_item))
                                     ? text_item->valuestring : *result;
                    cJSON_Delete(envelope);
                } else {
                    model_text = *result;
                }
                return model_text;
            });

        // 工具 9：天气查询
        mcp.AddTool(
            "self.weather_query",
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
            app.Alert("跌倒警报",
                      description.empty() ? "检测到老人可能跌倒，请立即确认"
                                          : description.c_str(),
                      "warning", Lang::Sounds::OGG_EXCLAMATION);
            // 跌倒警报重复播放，共 3 声
            ScheduleRepeatedSound(&Lang::Sounds::OGG_EXCLAMATION, 2, 2000);
            app.SendMcpMessage(
                "{\"type\":\"fall_alert\",\"source\":\"device\"}");
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
            ESP_LOGE(TAG, "fall_detection: create timer failed: %s",
                     esp_err_to_name(err));
            return;
        }
        err = esp_timer_start_periodic(fall_detection_timer_,
            CONFIG_BOARD_FALL_DETECTION_PERIOD_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fall_detection: start timer failed: %s",
                     esp_err_to_name(err));
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
            if (root) cJSON_Delete(root);
            return;
        }
        cJSON* elem = nullptr;
        cJSON_ArrayForEach(elem, root) {
            cJSON* t = cJSON_GetObjectItem(elem, "time");
            cJSON* c = cJSON_GetObjectItem(elem, "content");
            if (t != nullptr && cJSON_IsString(t) &&
                strcmp(t->valuestring, cur.c_str()) == 0) {
                std::string content =
                    (c != nullptr && cJSON_IsString(c)) ? c->valuestring : "该办事了";
                ESP_LOGI(TAG, "schedule_reminder: firing '%s' at %s",
                         content.c_str(), cur.c_str());
                Application::GetInstance().Schedule([content]() {
                    Application::GetInstance().Alert(
                        "日程提醒", content.c_str(), "info",
                        Lang::Sounds::OGG_EXCLAMATION);
                    // 短音容易漏听：共播 3 声，间隔 2 秒
                    ScheduleRepeatedSound(&Lang::Sounds::OGG_EXCLAMATION, 2, 2000);
                });
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
            ESP_LOGE(TAG, "schedule_reminder: create timer failed: %s",
                     esp_err_to_name(err));
            return;
        }
        err = esp_timer_start_periodic(schedule_timer_, 60 * 1000 * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "schedule_reminder: start timer failed: %s",
                     esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "schedule_reminder: timer started, checking every 60s");
    }
#endif  // CONFIG_ENABLE_SCHEDULE_REMINDER

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
                             ? text_item->valuestring : *result;
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
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().Alert(
                    "温馨提醒", "您已经坐了很久了，起来活动活动吧！",
                    "info", Lang::Sounds::OGG_EXCLAMATION);
                // 久坐提醒共播 3 声，间隔 2 秒
                ScheduleRepeatedSound(&Lang::Sounds::OGG_EXCLAMATION, 2, 2000);
            });
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
        err = esp_timer_start_periodic(
            sedentary_timer_, CONFIG_SEDENTARY_REMINDER_PERIOD_MS * 1000);
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
                             ? text_item->valuestring : *result;
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
            int threshold = CONFIG_BED_EXIT_EMPTY_TIMEOUT_S * 1000 /
                            CONFIG_BED_EXIT_CHECK_PERIOD_MS;
            if (bed_empty_count_ >= threshold) {
                ESP_LOGW(TAG, "bed_exit: bed empty too long, alerting!");
                bed_empty_count_ = 0;
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().Alert(
                        "离床告警",
                        "老人已离床较长时间未返回，请确认是否安全。",
                        "warning", Lang::Sounds::OGG_EXCLAMATION);
                    // 离床告警共播 3 声，间隔 2 秒
                    ScheduleRepeatedSound(&Lang::Sounds::OGG_EXCLAMATION, 2, 2000);
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
        err = esp_timer_start_periodic(
            bed_exit_timer_, CONFIG_BED_EXIT_CHECK_PERIOD_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bed_exit: start timer failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "bed_exit_detection: timer started, period=%d ms",
                 CONFIG_BED_EXIT_CHECK_PERIOD_MS);
    }
#endif  // CONFIG_ENABLE_BED_EXIT_DETECTION

public:
    CompactWifiBoardS3Cam() :
        boot_button_(BOOT_BUTTON_GPIO),
        sos_button_(SOS_BUTTON_GPIO, false, 3000) {
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
#endif
#ifdef CONFIG_ENABLE_SEDENTARY_REMINDER
        StartSedentaryReminder();
#endif
#ifdef CONFIG_ENABLE_BED_EXIT_DETECTION
        StartBedExitDetection();
#endif
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CompactWifiBoardS3Cam);
