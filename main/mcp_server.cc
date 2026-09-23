/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>
#include <iterator>

#include "application.h"
#include "board.h"
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "MCP"

McpServer::McpServer() {}

McpServer::~McpServer() = default;

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the "
            "audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call "
            "`self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList& properties) -> ReturnValue {
                    uint8_t brightness =
                        static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList& properties) -> ReturnValue {
                    auto theme_name = properties["theme"].value<std::string>();
                    auto& theme_manager = LvglThemeManager::GetInstance();
                    auto theme = theme_manager.GetTheme(theme_name);
                    if (theme != nullptr) {
                        display->SetTheme(theme);
                        return true;
                    }
                    return false;
                });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
                "Always remember you have a camera. If the user asks you to see something, use "
                "this tool to take a photo and then explain it.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList& properties) -> ToolResult {
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = camera->Explain(question);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    return std::move(*result);
                });
    }
#endif

#ifdef CONFIG_ENABLE_SILVER_ECONOMY_DEMO
    // ========================================================================
    // 银发经济演示工具（学习示例）
    // ------------------------------------------------------------------
    // 注意：按 mcp_server.cc:36-37 注释的规则，自定义工具应在板子的
    // InitializeTools() 中注册，不应放在这里。此区块仅作学习演示，
    // 通过 Kconfig 选项 CONFIG_ENABLE_SILVER_ECONOMY_DEMO 控制启用。
    // 烧入设备前请迁移到 boards/<your_board>/<board>.cc 的 InitializeTools()。
    // ========================================================================

    // 工具 1：服药提醒管理
    // 思路：NVS Settings 没有遍历 API，所以用一个 summary key "reminders"
    //       存所有提醒的 JSON 数组字符串，list/add/remove 都直接重写整个数组。
    AddTool("self.medication_reminder",
            "管理老人的服药提醒。支持添加、删除、查询当前所有提醒。\n"
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
            [this](const PropertyList& properties) -> ToolResult {
                auto action = properties["action"].value<std::string>();
                auto medicine = properties["medicine"].value<std::string>();
                auto time = properties["time"].value<std::string>();

                // 用 Settings 把所有提醒整体存为 JSON 字符串
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
                            // 不匹配则保留
                            cJSON_AddItemReferenceToArray(new_arr, elem);
                        }
                    }
                    cJSON_Delete(root);
                    root = new_arr;
                } else if (action != "list") {
                    cJSON_Delete(root);
                    return std::unexpected("Unknown action: " + action);
                }

                // 非 list 操作写回 NVS
                if (action != "list") {
                    char* out = cJSON_PrintUnformatted(root);
                    settings.SetString("reminders", out);
                    free(out);
                }
                return root;  // 返回 cJSON*，框架会负责释放
            });

    // 工具 2：跌倒检测（按需拍照分析）
    // 思路：暴露一个 AI 可调用的 check_once 工具。AI 根据对话上下文
    //       （比如老人喊"我摔倒了"或家属远程询问）触发一次拍照+云端视觉分析。
    //       真正的"主动周期检测"应另起 esp_timer 任务，不放在 MCP 工具里。
    AddTool("self.fall_detection",
            "拍照并调用云端视觉模型判断画面中是否有人呈跌倒姿态。\n"
            "用于跌倒检测场景。返回 JSON {fell, confidence, description}。\n"
            "若 fell=true，调用方应立即通知家属或拨打 120。",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
                auto camera = Board::GetInstance().GetCamera();
                if (camera == nullptr) {
                    return std::unexpected("Camera not available on this board");
                }
                // 降低任务优先级，避免摄像头采集阻塞音频/网络任务
                TaskPriorityReset priority_reset(1);
                if (!camera->Capture()) {
                    return std::unexpected("Failed to capture photo");
                }
                // 复用 camera->Explain() 接口，提示词约定 JSON 输出格式
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
    // 思路：家属通过云端 MCP 调用 add_message 推送文字留言，
    //       设备存到 NVS（避开音频文件解码的复杂性）。
    //       老人按键时通过协议让云端 TTS 念最新一条。
    //       真实音频留言版需扩展协议 + SPIFFS + Opus 解码，留作 TODO。
    AddTool("self.family_voice_board",
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
            [this](const PropertyList& properties) -> ToolResult {
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
                    // 用系统时间生成时间戳；如果没有同步过服务器时间，
                    // 使用 clock_ticks *tick 作为 fallback（粗略）。
                    time_t now = time(nullptr);
                    char time_buf[32] = {0};
                    if (now > 1700000000) {  // 2023 年之后才算有效
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
                    // 调用方（云端 LLM）拿到这段文本后会用 TTS 念给老人听
                    return text;
                }

                cJSON_Delete(root);
                return std::unexpected("Unknown action: " + action);
            });

    // TODO：为 family_voice_board 实现真实音频版：
    //   1. 扩展 websocket/mqtt 协议，新增 "voice_board_push" 消息类型
    //      携带 base64 编码的 Opus 音频
    //   2. 设备解码后存入 SPIFFS（/spiffs/voice_board/<n>.opus）
    //   3. 老人按 SOS 按钮 → audio_service_.PlayLocalFile(path)
    //   4. 需要在 audio_service.cc 新增 PlayLocalFile 接口
#endif  // CONFIG_ENABLE_SILVER_ECONOMY_DEMO

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), std::make_move_iterator(original_tools.begin()),
                  std::make_move_iterator(original_tools.end()));
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info", "Get the system information", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& app = Application::GetInstance();
                        app.Schedule([&app]() {
                            ESP_LOGW(TAG, "User requested reboot");
                            vTaskDelay(pdMS_TO_TICKS(1000));

                            app.Reboot();
                        });
                        return true;
                    });

    // Firmware upgrade
    AddUserOnlyTool(
        "self.upgrade_firmware",
        "Upgrade firmware from a specific URL. This will download and install the firmware, then "
        "reboot the device.",
        PropertyList({Property("url", kPropertyTypeString,
                               "The URL of the firmware binary file to download and install")}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());

            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });

            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->SupportsGuiOperations()) {
        AddUserOnlyTool("self.screen.get_info",
                        "Information about the screen, including width, height, etc.",
                        PropertyList(), [display](const PropertyList& properties) -> ReturnValue {
                            cJSON* json = cJSON_CreateObject();
                            cJSON_AddNumberToObject(json, "width", display->width());
                            cJSON_AddNumberToObject(json, "height", display->height());
                            cJSON_AddBoolToObject(json, "monochrome", display->IsMonochrome());
                            return json;
                        });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool(
            "self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    return std::unexpected("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (auto opened = http->Open("POST", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header +=
                        "Content-Disposition: form-data; name=\"file\"; "
                        "filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                auto upload_status = http->GetStatusCode();
                if (!upload_status) {
                    return std::unexpected(upload_status.error().ToString());
                }
                if (*upload_status != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*upload_status));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });

        AddUserOnlyTool(
            "self.screen.preview_image", "Preview an image on the screen",
            PropertyList({Property("url", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (auto opened = http->Open("GET", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto status_code = http->GetStatusCode();
                if (!status_code) {
                    return std::unexpected(status_code.error().ToString());
                }
                if (*status_code != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*status_code));
                }

                size_t content_length = http->GetBodyLength();
                using BufferPtr = std::unique_ptr<char, decltype(&heap_caps_free)>;
                BufferPtr data(
                    static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT)),
                    heap_caps_free);
                if (data == nullptr) {
                    return std::unexpected("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    auto ret = http->Read(data.get() + total_read, content_length - total_read);
                    if (!ret) {
                        return std::unexpected("Failed to download image: " + url);
                    }
                    if (*ret == 0) {
                        break;
                    }
                    total_read += *ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data.release(), total_read);
                if (!image->IsValid()) {
                    return std::unexpected("Downloaded image is invalid: " + url);
                }
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif  // CONFIG_LV_USE_SNAPSHOT
    }
#endif  // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition
    // layout)
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [](const PropertyList& properties) -> ReturnValue {
                        auto url = properties["url"].value<std::string>();
                        Settings settings("assets", true);
                        settings.SetString("download_url", url);
                        return true;
                    });
}

void McpServer::AddTool(std::unique_ptr<McpTool> tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [&tool](const auto& existing) {
            return existing->name() == tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(std::move(tool));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, std::move(callback)));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_Parse(message.c_str()));
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json.get(), std::move(response_sender));
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{"
            "\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message, response_sender);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools, response_sender);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, -32602, "Missing params", response_sender);
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, -32602, "Missing tool name", response_sender);
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, -32602, "Invalid arguments: expected object", response_sender);
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments,
                   std::move(response_sender));
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, -32601, "Method not implemented: " + method_str, response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(int id, const std::string& result,
                            const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, int code, const std::string& message,
                           const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"code\":";
    payload += std::to_string(code);
    payload += ",\"message\":\"";
    payload += message;
    payload += "\"}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, const std::string& message,
                           const ResponseSender& response_sender) {
    ReplyError(id, -32603, message, response_sender);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools,
                             const ResponseSender& response_sender) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit",
                 next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit",
                   response_sender);
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json, response_sender);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments,
                           ResponseSender response_sender) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const auto& tool) {
        return tool->name() == tool_name;
    });

    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, -32602, "Unknown tool: " + tool_name, response_sender);
        return;
    }

    McpTool* tool = tool_iter->get();
    PropertyList arguments = tool->properties();
    for (auto& argument : arguments) {
        bool found = false;
        std::expected<void, std::string> validation;
        if (cJSON_IsObject(tool_arguments)) {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                validation = argument.set_value<bool>(value->valueint == 1);
                found = true;
            } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                validation = argument.set_value<int>(value->valueint);
                found = true;
            } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                validation = argument.set_value<std::string>(value->valuestring);
                found = true;
            } else if (value != nullptr) {
                ESP_LOGE(TAG, "tools/call: Invalid type for argument: %s", argument.name().c_str());
                ReplyError(id, -32602, "Invalid type for argument: " + argument.name(),
                           response_sender);
                return;
            }
        }

        if (found && !validation) {
            ESP_LOGE(TAG, "tools/call: %s", validation.error().c_str());
            ReplyError(id, -32602, validation.error(), response_sender);
            return;
        }

        if (!argument.has_default_value() && !found) {
            ESP_LOGE(TAG, "tools/call: Missing required argument: %s", argument.name().c_str());
            ReplyError(id, -32602, "Missing required argument: " + argument.name(),
                       response_sender);
            return;
        }
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool, arguments = std::move(arguments),
                  response_sender = std::move(response_sender)]() {
        auto result = tool->Call(arguments);
        if (!result) {
            ESP_LOGE(TAG, "tools/call: %s", result.error().c_str());
            ReplyError(id, result.error(), response_sender);
            return;
        }
        ReplyResult(id, *result, response_sender);
    });
}
