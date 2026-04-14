#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sapi.h>
#include <sphelper.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef MIMOCA_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace {

struct Gesture {
    std::string label;
    double confidence;
};

struct Detection {
    std::string label;
    double confidence;
    std::vector<double> bbox;
};

struct HandPose {
    std::string label;
    double confidence;
};

struct Settings {
    bool speech_enabled;
    bool vision_enabled;
    bool gesture_enabled;
    bool tts_enabled;
};

struct TurnContext {
    std::string timestamp;
    std::string recipe_id;
    std::string step_id;
    std::string branch_id;
    std::string user_utterance;
    std::string current_step_instruction;
    std::string next_step_instruction;
    Gesture gesture;
    std::vector<Detection> detections;
    HandPose hand_pose;
    bool frame_available;
    std::string frame_summary;
    Settings settings;
};

struct CameraSnapshot {
    bool camera_started = false;
    bool frame_available = false;
    int width = 0;
    int height = 0;
    std::string capture_timestamp;
    uint64_t frame_count = 0;
    std::string status_message = "camera not initialized";
};

struct UiOverlay {
    std::string type;
    std::string target;
};

struct PlannerResponse {
    std::string assistant_text;
    bool speak;
    bool interruptible;
    bool advance_step;
    std::string new_branch_id;
    std::vector<UiOverlay> ui_overlays;
};

struct RecipeStep {
    std::string id;
    std::string instruction;
    std::string next_step_id;
    std::string branch_point_id;
    std::unordered_map<std::string, std::string> branch_next_step_ids;
};

struct BranchPoint {
    std::string id;
    std::vector<std::string> options;
};

struct Recipe {
    std::string id;
    std::string name;
    std::vector<RecipeStep> steps;
    std::unordered_map<std::string, BranchPoint> branch_points_by_id;
    std::unordered_map<std::string, size_t> step_index_by_id;
};

struct RecipeState {
    Recipe recipe;
    size_t current_step_index;
    std::unordered_map<std::string, std::string> selected_branch_by_point;
};

struct TranscriptEvent {
    std::string text;
    bool is_final;
};

struct SttResult {
    bool ok = false;
    std::string text;
    bool is_final = true;
    std::string error;
};

struct StreamingSttChunkResult {
    bool ok = false;
    bool speech_started = false;
    bool speech_active = false;
    bool is_final = false;
    bool vad_available = true;
    std::string text;
    std::string error;
};

struct PlannerRoundTripStatus {
    bool attempted = false;
    bool success = false;
    bool used_fallback = false;
    long long round_trip_ms = -1;
    std::string source = "unknown";
};

struct DebugSnapshot {
    std::string transcript = "(none)";
    Gesture gesture{"none", 0.0};
    std::vector<Detection> detections;
    std::string recipe_id = "(none)";
    std::string step_id = "(none)";
    std::string branch_id = "(none)";
    PlannerRoundTripStatus planner;
};

enum class IntentType {
    kNone,
    kStartupReady,
    kQueryCurrent,
    kNextStep,
    kRepeatStep,
    kBranchOptionA,
    kBranchOptionB,
    kExitDebug,
};

struct RuntimeIntent {
    IntentType type = IntentType::kNone;
    std::string utterance;
    std::string command;
    Gesture gesture{"none", 0.0};
    std::string source = "none";
};

void Log(const std::string& message);

#ifdef _WIN32
class TtsController {
   public:
    TtsController() {
        const char* configured_voice = std::getenv("MIMOCA_TTS_VOICE");
        if (configured_voice != nullptr) {
            selected_voice_ = configured_voice;
        }
    }

    ~TtsController() {
        Stop();
    }

    void Speak(const std::string& text) {
        if (text.empty()) {
            return;
        }

        Stop();
        stop_requested_ = false;
        speech_thread_ = std::thread([this, text]() {
            Log("TTS start: " + text);
            const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
                Log("TTS unavailable: COM initialization failed.");
                return;
            }
            const bool should_uninitialize = SUCCEEDED(init_hr);

            ISpVoice* voice = nullptr;
            const HRESULT create_hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                                       reinterpret_cast<void**>(&voice));
            if (FAILED(create_hr) || voice == nullptr) {
                Log("TTS unavailable: cannot create SAPI voice.");
                if (should_uninitialize) {
                    CoUninitialize();
                }
                return;
            }

            ApplyVoiceSelection(voice);

            std::wstring wtext(text.begin(), text.end());
            voice->Speak(wtext.c_str(), SPF_ASYNC, nullptr);

            while (!stop_requested_) {
                SPVOICESTATUS status{};
                if (FAILED(voice->GetStatus(&status, nullptr))) {
                    break;
                }
                if (status.dwRunningState != SPRS_IS_SPEAKING) {
                    break;
                }
                Sleep(30);
            }

            if (stop_requested_) {
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                Log("TTS stop requested; speech canceled.");
            } else {
                Log("TTS finished.");
            }

            voice->Release();
            if (should_uninitialize) {
                CoUninitialize();
            }
        });
    }

    void Stop() {
        if (!speech_thread_.joinable()) {
            return;
        }

        Log("TTS stop requested.");
        stop_requested_ = true;
        speech_thread_.join();
    }

   private:
    static std::wstring ToLowerCopyWide(std::wstring text) {
        std::transform(text.begin(), text.end(), text.begin(), [](const wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        return text;
    }

    static std::wstring Utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) {
            return L"";
        }
        const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
        if (chars <= 0) {
            std::wstring fallback;
            fallback.reserve(utf8.size());
            for (const unsigned char c : utf8) {
                fallback.push_back(static_cast<wchar_t>(c));
            }
            return fallback;
        }
        std::wstring wide(static_cast<size_t>(chars - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, wide.data(), chars);
        return wide;
    }

    static std::string WideToUtf8(const std::wstring& wide) {
        if (wide.empty()) {
            return "";
        }
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            return "";
        }
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
        return utf8;
    }

    void ApplyVoiceSelection(ISpVoice* voice) const {
        if (selected_voice_.empty()) {
            return;
        }

        IEnumSpObjectTokens* enum_tokens = nullptr;
        if (FAILED(SpEnumTokens(SPCAT_VOICES, nullptr, nullptr, &enum_tokens)) || enum_tokens == nullptr) {
            Log("TTS voice selection unavailable: could not enumerate installed voices.");
            return;
        }

        const std::wstring wanted = ToLowerCopyWide(Utf8ToWide(selected_voice_));
        ISpObjectToken* matched_token = nullptr;
        ULONG fetched = 0;
        while (enum_tokens->Next(1, &matched_token, &fetched) == S_OK && matched_token != nullptr) {
            wchar_t* description = nullptr;
            std::wstring description_text;
            if (SUCCEEDED(SpGetDescription(matched_token, &description)) && description != nullptr) {
                description_text.assign(description, description + std::wcslen(description));
                ::CoTaskMemFree(description);
            }

            wchar_t* token_id = nullptr;
            std::wstring token_id_text;
            if (SUCCEEDED(matched_token->GetId(&token_id)) && token_id != nullptr) {
                token_id_text.assign(token_id, token_id + std::wcslen(token_id));
                ::CoTaskMemFree(token_id);
            }

            const std::wstring haystack = ToLowerCopyWide(description_text + L" " + token_id_text);
            if (haystack.find(wanted) != std::wstring::npos) {
                if (SUCCEEDED(voice->SetVoice(matched_token))) {
                    Log("TTS voice selected: " + WideToUtf8(description_text));
                } else {
                    Log("TTS voice selection failed for: " + WideToUtf8(description_text));
                }
                matched_token->Release();
                enum_tokens->Release();
                return;
            }

            matched_token->Release();
        }

        enum_tokens->Release();
        Log("TTS voice '" + selected_voice_ + "' not found; using default voice.");
    }

    std::thread speech_thread_;
    std::atomic<bool> stop_requested_{false};
    std::string selected_voice_;
};
#else
class TtsController {
   public:
    void Speak(const std::string& text) {
        if (!text.empty()) {
            Log("TTS disabled on this platform; assistant_text not spoken.");
        }
    }
    void Stop() {}
};
#endif

void Log(const std::string& message) {
    std::cout << "[MiMoCA] " << message << '\n';
}

std::string BoolJson(const bool value) {
    return value ? "true" : "false";
}

std::string EscapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

size_t FindMatchingChar(const std::string& text, const size_t open_pos, const char open_char, const char close_char) {
    int depth = 0;
    for (size_t i = open_pos; i < text.size(); ++i) {
        if (text[i] == open_char) {
            ++depth;
        } else if (text[i] == close_char) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

std::string ExtractJsonFieldString(const std::string& json, const std::string& field) {
    const std::string key = "\"" + field + "\":";
    const size_t key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return "";
    }

    size_t pos = key_pos + key.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }

    if (json.compare(pos, 4, "null") == 0) {
        return "";
    }

    if (pos >= json.size() || json[pos] != '"') {
        return "";
    }
    ++pos;

    std::string result;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            result.push_back(json[pos++]);
        } else if (c == '"') {
            break;
        } else {
            result.push_back(c);
        }
    }
    return result;
}

bool ExtractJsonFieldBool(const std::string& json, const std::string& field, const bool default_value) {
    const std::string key = "\"" + field + "\":";
    const size_t key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return default_value;
    }
    size_t pos = key_pos + key.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    if (json.compare(pos, 4, "true") == 0) {
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        return false;
    }
    return default_value;
}

double ExtractJsonFieldDouble(const std::string& json, const std::string& field, const double default_value) {
    const std::string key = "\"" + field + "\":";
    const size_t key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return default_value;
    }
    size_t pos = key_pos + key.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    size_t end = pos;
    while (end < json.size()) {
        const char c = json[end];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
            ++end;
            continue;
        }
        break;
    }
    if (end <= pos) {
        return default_value;
    }
    try {
        return std::stod(json.substr(pos, end - pos));
    } catch (const std::exception&) {
        return default_value;
    }
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t octet_a = data[i];
        const uint32_t octet_b = (i + 1 < data.size()) ? data[i + 1] : 0;
        const uint32_t octet_c = (i + 2 < data.size()) ? data[i + 2] : 0;
        const uint32_t triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

        out.push_back(kTable[(triple >> 18U) & 0x3FU]);
        out.push_back(kTable[(triple >> 12U) & 0x3FU]);
        out.push_back((i + 1 < data.size()) ? kTable[(triple >> 6U) & 0x3FU] : '=');
        out.push_back((i + 2 < data.size()) ? kTable[triple & 0x3FU] : '=');
    }

    return out;
}

std::string ExtractJsonFieldObject(const std::string& json, const std::string& field) {
    const std::string key = "\"" + field + "\":";
    const size_t key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return "";
    }

    size_t pos = key_pos + key.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '{') {
        return "";
    }

    const size_t end = FindMatchingChar(json, pos, '{', '}');
    if (end == std::string::npos) {
        return "";
    }
    return json.substr(pos, end - pos + 1);
}

std::vector<std::string> ExtractJsonStringArray(const std::string& json, const std::string& field) {
    std::vector<std::string> values;
    const std::string key = "\"" + field + "\":";
    const size_t key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return values;
    }

    size_t pos = key_pos + key.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '[') {
        return values;
    }

    const size_t end = FindMatchingChar(json, pos, '[', ']');
    if (end == std::string::npos) {
        return values;
    }

    size_t cursor = pos + 1;
    while (cursor < end) {
        const size_t quote_start = json.find('"', cursor);
        if (quote_start == std::string::npos || quote_start >= end) {
            break;
        }
        const size_t quote_end = json.find('"', quote_start + 1);
        if (quote_end == std::string::npos || quote_end > end) {
            break;
        }
        values.push_back(json.substr(quote_start + 1, quote_end - quote_start - 1));
        cursor = quote_end + 1;
    }
    return values;
}

std::unordered_map<std::string, std::string> ExtractJsonStringMap(const std::string& json, const std::string& field) {
    std::unordered_map<std::string, std::string> values;
    const std::string object_json = ExtractJsonFieldObject(json, field);
    if (object_json.empty()) {
        return values;
    }

    size_t cursor = 0;
    while (cursor < object_json.size()) {
        const size_t key_start = object_json.find('"', cursor);
        if (key_start == std::string::npos) {
            break;
        }
        const size_t key_end = object_json.find('"', key_start + 1);
        if (key_end == std::string::npos) {
            break;
        }
        const size_t value_start = object_json.find('"', key_end + 1);
        if (value_start == std::string::npos) {
            break;
        }
        const size_t value_end = object_json.find('"', value_start + 1);
        if (value_end == std::string::npos) {
            break;
        }
        values[object_json.substr(key_start + 1, key_end - key_start - 1)] =
            object_json.substr(value_start + 1, value_end - value_start - 1);
        cursor = value_end + 1;
    }

    return values;
}

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool ContainsText(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

IntentType IntentFromUtterance(const std::string& utterance) {
    const std::string normalized = ToLower(utterance);
    if (ContainsText(normalized, "exit")) {
        return IntentType::kExitDebug;
    }
    if (ContainsText(normalized, "repeat")) {
        return IntentType::kRepeatStep;
    }
    if (ContainsText(normalized, "current")) {
        return IntentType::kQueryCurrent;
    }
    if (ContainsText(normalized, "next")) {
        return IntentType::kNextStep;
    }
    if (ContainsText(normalized, "option a") || ContainsText(normalized, "left option")) {
        return IntentType::kBranchOptionA;
    }
    if (ContainsText(normalized, "option b") || ContainsText(normalized, "right option")) {
        return IntentType::kBranchOptionB;
    }
    return IntentType::kNone;
}

IntentType IntentFromGestureLabel(const std::string& gesture_label) {
    if (gesture_label == "next") {
        return IntentType::kNextStep;
    }
    if (gesture_label == "repeat") {
        return IntentType::kRepeatStep;
    }
    if (gesture_label == "option_a") {
        return IntentType::kBranchOptionA;
    }
    if (gesture_label == "option_b") {
        return IntentType::kBranchOptionB;
    }
    return IntentType::kNone;
}

std::string FallbackCommandForIntent(const RuntimeIntent& intent) {
    if (!intent.command.empty()) {
        return intent.command;
    }
    switch (intent.type) {
        case IntentType::kStartupReady:
            return "ready";
        case IntentType::kQueryCurrent:
            return "current";
        case IntentType::kNextStep:
            return "next";
        case IntentType::kRepeatStep:
            return "repeat";
        case IntentType::kBranchOptionA:
            return "option_a";
        case IntentType::kBranchOptionB:
            return "option_b";
        case IntentType::kExitDebug:
            return "exit";
        case IntentType::kNone:
        default:
            return "";
    }
}

std::string EnvOrDefault(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    return value;
}

std::string Trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string SerializeTurnContext(const TurnContext& tc) {
    std::string json = "{";
    json += "\"timestamp\":\"" + EscapeJson(tc.timestamp) + "\",";
    json += "\"recipe_id\":\"" + EscapeJson(tc.recipe_id) + "\",";
    json += "\"step_id\":\"" + EscapeJson(tc.step_id) + "\",";
    if (tc.branch_id.empty()) {
        json += "\"branch_id\":null,";
    } else {
        json += "\"branch_id\":\"" + EscapeJson(tc.branch_id) + "\",";
    }
    json += "\"user_utterance\":\"" + EscapeJson(tc.user_utterance) + "\",";
    json += "\"current_step_instruction\":\"" + EscapeJson(tc.current_step_instruction) + "\",";
    json += "\"next_step_instruction\":\"" + EscapeJson(tc.next_step_instruction) + "\",";
    json += "\"gesture\":{";
    json += "\"label\":\"" + EscapeJson(tc.gesture.label) + "\",";
    json += "\"confidence\":" + std::to_string(tc.gesture.confidence);
    json += "},";
    json += "\"detections\":[";
    for (size_t i = 0; i < tc.detections.size(); ++i) {
        const auto& d = tc.detections[i];
        if (i > 0) {
            json += ",";
        }
        json += "{";
        json += "\"label\":\"" + EscapeJson(d.label) + "\",";
        json += "\"confidence\":" + std::to_string(d.confidence) + ",";
        json += "\"bbox\":[";
        for (size_t j = 0; j < d.bbox.size(); ++j) {
            if (j > 0) {
                json += ",";
            }
            json += std::to_string(d.bbox[j]);
        }
        json += "]}";
    }
    json += "],";
    json += "\"hand_pose\":{";
    json += "\"label\":\"" + EscapeJson(tc.hand_pose.label) + "\",";
    json += "\"confidence\":" + std::to_string(tc.hand_pose.confidence);
    json += "},";
    json += "\"frame_available\":" + BoolJson(tc.frame_available) + ",";
    json += "\"frame_summary\":\"" + EscapeJson(tc.frame_summary) + "\",";
    json += "\"settings\":{";
    json += "\"speech_enabled\":" + BoolJson(tc.settings.speech_enabled) + ",";
    json += "\"vision_enabled\":" + BoolJson(tc.settings.vision_enabled) + ",";
    json += "\"gesture_enabled\":" + BoolJson(tc.settings.gesture_enabled) + ",";
    json += "\"tts_enabled\":" + BoolJson(tc.settings.tts_enabled);
    json += "}}";
    return json;
}

std::string ExtractHttpBody(const std::string& response) {
    const std::string header_delim = "\r\n\r\n";
    const size_t pos = response.find(header_delim);
    if (pos == std::string::npos) {
        return "";
    }
    return response.substr(pos + header_delim.size());
}

PlannerResponse ParsePlannerResponseBody(const std::string& body) {
    PlannerResponse response{};
    response.assistant_text = ExtractJsonFieldString(body, "assistant_text");
    response.speak = ExtractJsonFieldBool(body, "speak", false);
    response.interruptible = ExtractJsonFieldBool(body, "interruptible", true);
    response.advance_step = ExtractJsonFieldBool(body, "advance_step", false);
    response.new_branch_id = ExtractJsonFieldString(body, "new_branch_id");

    const size_t overlays_pos = body.find("\"ui_overlays\"");
    if (overlays_pos != std::string::npos) {
        const std::string type = ExtractJsonFieldString(body.substr(overlays_pos), "type");
        const std::string target = ExtractJsonFieldString(body.substr(overlays_pos), "target");
        if (!type.empty() || !target.empty()) {
            response.ui_overlays.push_back({type, target});
        }
    }

    return response;
}

bool SendHttpRequest(const std::string& host, int port, const std::string& request, std::string& response_out) {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        Log("Failed to initialize Winsock.");
        return false;
    }
#endif

    int socket_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (socket_fd < 0) {
        Log("Failed to create socket.");
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        Log("Invalid host address: " + host);
#ifdef _WIN32
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
        return false;
    }

    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        Log("Could not connect to Python sidecar at http://" + host + ":" + std::to_string(port));
#ifdef _WIN32
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
        return false;
    }

#ifdef _WIN32
    const int sent = send(socket_fd, request.c_str(), static_cast<int>(request.size()), 0);
#else
    const ssize_t sent = send(socket_fd, request.c_str(), request.size(), 0);
#endif
    if (sent <= 0) {
        Log("Failed to send request to Python sidecar.");
#ifdef _WIN32
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
        return false;
    }

    response_out.clear();
    char buffer[1024];
    while (true) {
#ifdef _WIN32
        const int bytes_read = recv(socket_fd, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const ssize_t bytes_read = recv(socket_fd, buffer, sizeof(buffer), 0);
#endif
        if (bytes_read <= 0) {
            break;
        }
        response_out.append(buffer, static_cast<size_t>(bytes_read));
    }

#ifdef _WIN32
    closesocket(socket_fd);
    WSACleanup();
#else
    close(socket_fd);
#endif

    return !response_out.empty();
}

bool CheckPythonHealth(const std::string& host, int port) {
    const std::string request =
        "GET /health HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Connection: close\r\n\r\n";

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return false;
    }

    const bool ok = response.find("200 OK") != std::string::npos;
    if (ok) {
        Log("Python sidecar health check succeeded.");
    } else {
        Log("Python sidecar health check failed; unexpected response.");
    }

    Log("Sidecar response preview: " + response.substr(0, std::min<size_t>(response.size(), 180)));
    return ok;
}

std::string MakeIsoTimestampNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buffer);
}

std::string ReadFileText(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool LoadFirstRecipe(const std::string& path, Recipe& out_recipe) {
    const std::string json = ReadFileText(path);
    if (json.empty()) {
        Log("Could not read recipe file: " + path);
        return false;
    }

    const size_t recipes_key = json.find("\"recipes\"");
    if (recipes_key == std::string::npos) {
        Log("Recipe file missing recipes array.");
        return false;
    }

    const size_t recipes_array_start = json.find('[', recipes_key);
    if (recipes_array_start == std::string::npos) {
        Log("Recipe file missing recipes array start.");
        return false;
    }

    const size_t first_recipe_start = json.find('{', recipes_array_start);
    if (first_recipe_start == std::string::npos) {
        Log("Recipe file contains no recipe objects.");
        return false;
    }

    const size_t first_recipe_end = FindMatchingChar(json, first_recipe_start, '{', '}');
    if (first_recipe_end == std::string::npos) {
        Log("Recipe object is malformed.");
        return false;
    }

    const std::string recipe_json = json.substr(first_recipe_start, first_recipe_end - first_recipe_start + 1);
    out_recipe.id = ExtractJsonFieldString(recipe_json, "id");
    out_recipe.name = ExtractJsonFieldString(recipe_json, "name");

    const size_t steps_key = recipe_json.find("\"steps\"");
    if (steps_key == std::string::npos) {
        Log("Recipe object missing steps.");
        return false;
    }

    const size_t steps_array_start = recipe_json.find('[', steps_key);
    if (steps_array_start == std::string::npos) {
        Log("Recipe steps array malformed.");
        return false;
    }

    const size_t steps_array_end = FindMatchingChar(recipe_json, steps_array_start, '[', ']');
    if (steps_array_end == std::string::npos) {
        Log("Recipe steps array malformed.");
        return false;
    }

    size_t cursor = steps_array_start + 1;
    while (cursor < steps_array_end) {
        const size_t step_start = recipe_json.find('{', cursor);
        if (step_start == std::string::npos || step_start > steps_array_end) {
            break;
        }

        const size_t step_end = FindMatchingChar(recipe_json, step_start, '{', '}');
        if (step_end == std::string::npos || step_end > steps_array_end) {
            break;
        }

        const std::string step_json = recipe_json.substr(step_start, step_end - step_start + 1);
        RecipeStep step{};
        step.id = ExtractJsonFieldString(step_json, "id");
        step.instruction = ExtractJsonFieldString(step_json, "instruction");
        step.next_step_id = ExtractJsonFieldString(step_json, "next_step_id");
        step.branch_point_id = ExtractJsonFieldString(step_json, "branch_point_id");
        step.branch_next_step_ids = ExtractJsonStringMap(step_json, "branch_next_step_ids");
        if (!step.id.empty() && !step.instruction.empty()) {
            out_recipe.steps.push_back(step);
        }

        cursor = step_end + 1;
    }

    out_recipe.step_index_by_id.clear();
    for (size_t i = 0; i < out_recipe.steps.size(); ++i) {
        out_recipe.step_index_by_id[out_recipe.steps[i].id] = i;
    }

    out_recipe.branch_points_by_id.clear();
    const size_t branch_points_key = recipe_json.find("\"branch_points\"");
    if (branch_points_key != std::string::npos) {
        const size_t branch_points_start = recipe_json.find('[', branch_points_key);
        if (branch_points_start != std::string::npos) {
            const size_t branch_points_end = FindMatchingChar(recipe_json, branch_points_start, '[', ']');
            if (branch_points_end != std::string::npos) {
                size_t branch_cursor = branch_points_start + 1;
                while (branch_cursor < branch_points_end) {
                    const size_t branch_start = recipe_json.find('{', branch_cursor);
                    if (branch_start == std::string::npos || branch_start > branch_points_end) {
                        break;
                    }
                    const size_t branch_end = FindMatchingChar(recipe_json, branch_start, '{', '}');
                    if (branch_end == std::string::npos || branch_end > branch_points_end) {
                        break;
                    }

                    const std::string branch_json = recipe_json.substr(branch_start, branch_end - branch_start + 1);
                    BranchPoint point{};
                    point.id = ExtractJsonFieldString(branch_json, "id");
                    point.options = ExtractJsonStringArray(branch_json, "options");
                    if (!point.id.empty() && !point.options.empty()) {
                        out_recipe.branch_points_by_id[point.id] = point;
                    }

                    branch_cursor = branch_end + 1;
                }
            }
        }
    }

    if (out_recipe.id.empty() || out_recipe.steps.empty()) {
        Log("Recipe parse produced empty data.");
        return false;
    }

    Log("Loaded recipe '" + out_recipe.name + "' with " + std::to_string(out_recipe.steps.size()) + " steps and " +
        std::to_string(out_recipe.branch_points_by_id.size()) + " branch points.");
    return true;
}

const RecipeStep* GetCurrentStep(const RecipeState& state) {
    if (state.current_step_index >= state.recipe.steps.size()) {
        return nullptr;
    }
    return &state.recipe.steps[state.current_step_index];
}

const RecipeStep* GetNextStep(const RecipeState& state) {
    const RecipeStep* current = GetCurrentStep(state);
    if (current == nullptr) {
        return nullptr;
    }

    std::string next_step_id = current->next_step_id;
    if (!current->branch_point_id.empty()) {
        const auto selected_it = state.selected_branch_by_point.find(current->branch_point_id);
        if (selected_it != state.selected_branch_by_point.end()) {
            const auto branch_next_it = current->branch_next_step_ids.find(selected_it->second);
            if (branch_next_it != current->branch_next_step_ids.end()) {
                next_step_id = branch_next_it->second;
                Log("Branch flow: branch_point_id='" + current->branch_point_id + "', selected='" +
                    selected_it->second + "', next_step_id='" + next_step_id + "'.");
            }
        }
    }

    if (next_step_id.empty()) {
        return nullptr;
    }

    const auto it = state.recipe.step_index_by_id.find(next_step_id);
    if (it == state.recipe.step_index_by_id.end()) {
        return nullptr;
    }

    return &state.recipe.steps[it->second];
}

bool AdvanceCurrentStep(RecipeState& state) {
    const RecipeStep* next = GetNextStep(state);
    if (next == nullptr) {
        return false;
    }

    state.current_step_index = state.recipe.step_index_by_id[next->id];
    return true;
}

std::string NormalizeBranchText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        if (c == '_' || c == '-') {
            return ' ';
        }
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string ResolveBranchSelection(const RecipeState& state, const std::string& utterance, const Gesture& gesture) {
    const RecipeStep* current = GetCurrentStep(state);
    if (current == nullptr || current->branch_point_id.empty()) {
        return "";
    }

    const auto point_it = state.recipe.branch_points_by_id.find(current->branch_point_id);
    if (point_it == state.recipe.branch_points_by_id.end() || point_it->second.options.empty()) {
        return "";
    }

    const std::vector<std::string>& options = point_it->second.options;
    if (gesture.label == "option_a") {
        return options[0];
    }
    if (gesture.label == "option_b" && options.size() > 1) {
        return options[1];
    }

    const std::string normalized_utterance = NormalizeBranchText(utterance);
    for (const std::string& option : options) {
        if (ContainsText(normalized_utterance, NormalizeBranchText(option))) {
            return option;
        }
    }

    return "";
}

bool ApplyBranchSelection(RecipeState& state, const std::string& selected_branch, const std::string& source) {
    const RecipeStep* current = GetCurrentStep(state);
    if (current == nullptr || current->branch_point_id.empty() || selected_branch.empty()) {
        return false;
    }

    const auto point_it = state.recipe.branch_points_by_id.find(current->branch_point_id);
    if (point_it == state.recipe.branch_points_by_id.end()) {
        return false;
    }

    bool option_exists = false;
    for (const std::string& option : point_it->second.options) {
        if (option == selected_branch) {
            option_exists = true;
            break;
        }
    }
    if (!option_exists) {
        Log("Branch selection ignored: unknown branch '" + selected_branch + "' for branch point '" +
            current->branch_point_id + "'.");
        return false;
    }

    state.selected_branch_by_point[current->branch_point_id] = selected_branch;
    Log("Branch selected via " + source + ": branch_point_id='" + current->branch_point_id + "', branch='" +
        selected_branch + "'.");
    return true;
}

TurnContext BuildTurnContext(const RecipeState& state,
                             const std::string& utterance,
                             const Gesture& gesture,
                             const std::vector<Detection>& detections,
                             const CameraSnapshot& camera_snapshot) {
    const RecipeStep* current = GetCurrentStep(state);
    const RecipeStep* next = GetNextStep(state);
    const std::string frame_summary = camera_snapshot.frame_available
                                          ? std::to_string(camera_snapshot.width) + "x" +
                                                std::to_string(camera_snapshot.height) + " at " +
                                                camera_snapshot.capture_timestamp
                                          : camera_snapshot.status_message;

    return TurnContext{
        MakeIsoTimestampNow(),
        state.recipe.id,
        current != nullptr ? current->id : "",
        [&state, current]() -> std::string {
            if (current == nullptr || current->branch_point_id.empty()) {
                if (!state.selected_branch_by_point.empty()) {
                    return state.selected_branch_by_point.begin()->second;
                }
                return "";
            }
            const auto it = state.selected_branch_by_point.find(current->branch_point_id);
            if (it != state.selected_branch_by_point.end()) {
                return it->second;
            }
            if (!state.selected_branch_by_point.empty()) {
                return state.selected_branch_by_point.begin()->second;
            }
            return "";
        }(),
        utterance,
        current != nullptr ? current->instruction : "",
        next != nullptr ? next->instruction : "",
        gesture,
        detections,
        HandPose{"unknown", 0.0},
        camera_snapshot.frame_available,
        frame_summary,
        Settings{true, true, true, true},
    };
}

bool RequestMockPlanner(const std::string& host, int port, const TurnContext& turn_context, PlannerResponse& out) {
    const std::string body = SerializeTurnContext(turn_context);
    Log("Serialized TurnContext request: " + body);

    const std::string request =
        "POST /plan HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return false;
    }

    const std::string response_body = ExtractHttpBody(response);
    Log("Serialized PlannerResponse body: " + response_body);

    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return false;
    }

    out = ParsePlannerResponseBody(response_body);
    return true;
}

bool ReadBinaryFile(const std::string& path, std::vector<uint8_t>& out_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    out_bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

SttResult RequestSttTranscription(const std::string& host,
                                  const int port,
                                  const std::vector<uint8_t>& wav_bytes,
                                  const bool is_final) {
    if (wav_bytes.empty()) {
        return SttResult{false, "", is_final, "empty_audio"};
    }

    const std::string body = "{\"audio_base64\":\"" + Base64Encode(wav_bytes) +
                             "\",\"audio_format\":\"wav\",\"is_final\":" + BoolJson(is_final) + "}";
    const std::string request =
        "POST /stt/transcribe HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return SttResult{false, "", is_final, "sidecar_unreachable"};
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        const std::string error = ExtractJsonFieldString(response_body, "error");
        return SttResult{false, "", is_final, error.empty() ? "stt_http_error" : error};
    }
    return SttResult{
        true,
        ExtractJsonFieldString(response_body, "text"),
        ExtractJsonFieldBool(response_body, "is_final", is_final),
        "",
    };
}

bool ParseWavPcm16MonoOrStereo(const std::vector<uint8_t>& wav_bytes,
                               int& out_sample_rate_hz,
                               std::vector<uint8_t>& out_pcm_s16le) {
    if (wav_bytes.size() < 44) {
        return false;
    }
    auto read_u16 = [&wav_bytes](const size_t offset) -> uint16_t {
        return static_cast<uint16_t>(wav_bytes[offset] | (static_cast<uint16_t>(wav_bytes[offset + 1]) << 8U));
    };
    auto read_u32 = [&wav_bytes](const size_t offset) -> uint32_t {
        return static_cast<uint32_t>(wav_bytes[offset]) | (static_cast<uint32_t>(wav_bytes[offset + 1]) << 8U) |
               (static_cast<uint32_t>(wav_bytes[offset + 2]) << 16U) | (static_cast<uint32_t>(wav_bytes[offset + 3]) << 24U);
    };

    if (std::string(reinterpret_cast<const char*>(&wav_bytes[0]), 4) != "RIFF" ||
        std::string(reinterpret_cast<const char*>(&wav_bytes[8]), 4) != "WAVE") {
        return false;
    }

    size_t cursor = 12;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;
    size_t data_start = 0;
    uint32_t data_size = 0;

    while (cursor + 8 <= wav_bytes.size()) {
        const std::string chunk_id(reinterpret_cast<const char*>(&wav_bytes[cursor]), 4);
        const uint32_t chunk_size = read_u32(cursor + 4);
        cursor += 8;
        if (cursor + chunk_size > wav_bytes.size()) {
            return false;
        }
        if (chunk_id == "fmt ") {
            if (chunk_size < 16) {
                return false;
            }
            audio_format = read_u16(cursor);
            channels = read_u16(cursor + 2);
            sample_rate = read_u32(cursor + 4);
            bits_per_sample = read_u16(cursor + 14);
        } else if (chunk_id == "data") {
            data_start = cursor;
            data_size = chunk_size;
            break;
        }
        cursor += chunk_size + (chunk_size % 2);
    }

    if (audio_format != 1 || (channels != 1 && channels != 2) || bits_per_sample != 16 || data_start == 0 || data_size == 0) {
        return false;
    }

    out_sample_rate_hz = static_cast<int>(sample_rate);
    if (channels == 1) {
        out_pcm_s16le.assign(wav_bytes.begin() + static_cast<long>(data_start),
                             wav_bytes.begin() + static_cast<long>(data_start + data_size));
        return true;
    }

    out_pcm_s16le.clear();
    out_pcm_s16le.reserve(data_size / 2);
    for (size_t i = data_start; i + 3 < data_start + data_size; i += 4) {
        const int16_t left = static_cast<int16_t>(static_cast<uint16_t>(wav_bytes[i]) |
                                                  (static_cast<uint16_t>(wav_bytes[i + 1]) << 8U));
        const int16_t right = static_cast<int16_t>(static_cast<uint16_t>(wav_bytes[i + 2]) |
                                                   (static_cast<uint16_t>(wav_bytes[i + 3]) << 8U));
        const int16_t mono = static_cast<int16_t>((static_cast<int>(left) + static_cast<int>(right)) / 2);
        out_pcm_s16le.push_back(static_cast<uint8_t>(mono & 0xFF));
        out_pcm_s16le.push_back(static_cast<uint8_t>((mono >> 8) & 0xFF));
    }
    return !out_pcm_s16le.empty();
}

bool StartStreamingSttSession(const std::string& host, int port, int sample_rate_hz, std::string& session_id_out) {
    const std::string body = "{\"sample_rate_hz\":" + std::to_string(sample_rate_hz) + "}";
    const std::string request =
        "POST /stt/session/start HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return false;
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return false;
    }
    session_id_out = ExtractJsonFieldString(response_body, "session_id");
    return !session_id_out.empty();
}

StreamingSttChunkResult AppendStreamingSttChunk(const std::string& host,
                                                int port,
                                                const std::string& session_id,
                                                const std::vector<uint8_t>& chunk) {
    const std::string body = "{\"session_id\":\"" + EscapeJson(session_id) + "\",\"audio_chunk_base64\":\"" +
                             Base64Encode(chunk) + "\"}";
    const std::string request =
        "POST /stt/session/chunk HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return StreamingSttChunkResult{false, false, false, false, true, "", "sidecar_unreachable"};
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return StreamingSttChunkResult{false, false, false, false, true, "", "stt_chunk_http_error"};
    }
    return StreamingSttChunkResult{
        true,
        ExtractJsonFieldBool(response_body, "speech_started", false),
        ExtractJsonFieldBool(response_body, "speech_active", false),
        ExtractJsonFieldBool(response_body, "is_final", false),
        ExtractJsonFieldBool(response_body, "vad_available", true),
        ExtractJsonFieldString(response_body, "text"),
        "",
    };
}

SttResult FinalizeStreamingSttSession(const std::string& host, int port, const std::string& session_id) {
    const std::string body = "{\"session_id\":\"" + EscapeJson(session_id) + "\"}";
    const std::string request =
        "POST /stt/session/finalize HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return SttResult{false, "", true, "sidecar_unreachable"};
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return SttResult{false, "", true, "stt_finalize_http_error"};
    }
    return SttResult{true, ExtractJsonFieldString(response_body, "text"), true, ""};
}

bool RequestGestureDetection(const std::string& host,
                             const int port,
                             const std::vector<uint8_t>& jpeg_bytes,
                             Gesture& out_gesture) {
    if (jpeg_bytes.empty()) {
        return false;
    }
    const std::string body =
        "{\"image_base64\":\"" + Base64Encode(jpeg_bytes) + "\",\"image_format\":\"jpeg\"}";
    const std::string request =
        "POST /gesture/detect HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return false;
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return false;
    }
    const std::string gesture_object = ExtractJsonFieldObject(response_body, "gesture");
    if (gesture_object.empty()) {
        return false;
    }
    const std::string label = ExtractJsonFieldString(gesture_object, "label");
    const double confidence = ExtractJsonFieldDouble(gesture_object, "confidence", 0.0);
    out_gesture = Gesture{label.empty() ? "none" : label, std::clamp(confidence, 0.0, 1.0)};
    return true;
}

std::vector<double> ParseNumberArray(const std::string& json_array) {
    std::vector<double> values;
    size_t cursor = 0;
    while (cursor < json_array.size()) {
        while (cursor < json_array.size() && json_array[cursor] != '-' && json_array[cursor] != '+' &&
               (json_array[cursor] < '0' || json_array[cursor] > '9')) {
            ++cursor;
        }
        if (cursor >= json_array.size()) {
            break;
        }
        size_t end = cursor;
        while (end < json_array.size()) {
            const char c = json_array[end];
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
                ++end;
                continue;
            }
            break;
        }
        try {
            values.push_back(std::stod(json_array.substr(cursor, end - cursor)));
        } catch (const std::exception&) {
        }
        cursor = end + 1;
    }
    return values;
}

bool RequestVisionDetections(const std::string& host,
                             const int port,
                             const std::vector<uint8_t>& jpeg_bytes,
                             std::vector<Detection>& out_detections) {
    out_detections.clear();
    if (jpeg_bytes.empty()) {
        return false;
    }
    const std::string body = "{\"image_base64\":\"" + Base64Encode(jpeg_bytes) +
                             "\",\"image_format\":\"jpeg\"}";
    const std::string request =
        "POST /vision/detect HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return false;
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return false;
    }

    const size_t detections_key = response_body.find("\"detections\"");
    if (detections_key == std::string::npos) {
        return true;
    }
    const size_t array_start = response_body.find('[', detections_key);
    if (array_start == std::string::npos) {
        return true;
    }
    const size_t array_end = FindMatchingChar(response_body, array_start, '[', ']');
    if (array_end == std::string::npos) {
        return true;
    }
    size_t cursor = array_start + 1;
    while (cursor < array_end) {
        const size_t obj_start = response_body.find('{', cursor);
        if (obj_start == std::string::npos || obj_start > array_end) {
            break;
        }
        const size_t obj_end = FindMatchingChar(response_body, obj_start, '{', '}');
        if (obj_end == std::string::npos || obj_end > array_end) {
            break;
        }
        const std::string det_obj = response_body.substr(obj_start, obj_end - obj_start + 1);
        Detection det{};
        det.label = ExtractJsonFieldString(det_obj, "label");
        det.confidence = std::clamp(ExtractJsonFieldDouble(det_obj, "confidence", 0.0), 0.0, 1.0);
        const size_t bbox_key = det_obj.find("\"bbox\"");
        if (bbox_key != std::string::npos) {
            const size_t bbox_start = det_obj.find('[', bbox_key);
            if (bbox_start != std::string::npos) {
                const size_t bbox_end = FindMatchingChar(det_obj, bbox_start, '[', ']');
                if (bbox_end != std::string::npos) {
                    det.bbox = ParseNumberArray(det_obj.substr(bbox_start, bbox_end - bbox_start + 1));
                }
            }
        }
        if (!det.label.empty()) {
            out_detections.push_back(det);
        }
        cursor = obj_end + 1;
    }
    return true;
}

std::string FormatDetectionsCompact(const std::vector<Detection>& detections) {
    if (detections.empty()) {
        return "[]";
    }

    std::string out = "[";
    for (size_t i = 0; i < detections.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += detections[i].label + "(" + std::to_string(detections[i].confidence) + ")";
    }
    out += "]";
    return out;
}

void PrintDebugSnapshot(const DebugSnapshot& snapshot) {
    std::cout << "[MiMoCA][debug] transcript=\"" << snapshot.transcript << "\"\n";
    std::cout << "[MiMoCA][debug] gesture=" << snapshot.gesture.label << " (" << snapshot.gesture.confidence << ")\n";
    std::cout << "[MiMoCA][debug] detections=" << FormatDetectionsCompact(snapshot.detections) << '\n';
    std::cout << "[MiMoCA][debug] state=recipe:" << snapshot.recipe_id << " step:" << snapshot.step_id
              << " branch:" << snapshot.branch_id << '\n';
    std::cout << "[MiMoCA][debug] planner=source:" << snapshot.planner.source
              << " attempted:" << (snapshot.planner.attempted ? "true" : "false")
              << " success:" << (snapshot.planner.success ? "true" : "false")
              << " fallback:" << (snapshot.planner.used_fallback ? "true" : "false")
              << " round_trip_ms:" << snapshot.planner.round_trip_ms << '\n';
}

void PrintPlannerResponse(const PlannerResponse& response) {
    Log("Planner response parsed:");
    std::cout << "  assistant_text: " << response.assistant_text << '\n';
    std::cout << "  speak: " << (response.speak ? "true" : "false") << '\n';
    std::cout << "  interruptible: " << (response.interruptible ? "true" : "false") << '\n';
    std::cout << "  advance_step: " << (response.advance_step ? "true" : "false") << '\n';
    std::cout << "  new_branch_id: " << (response.new_branch_id.empty() ? "null" : response.new_branch_id) << '\n';
    if (!response.ui_overlays.empty()) {
        std::cout << "  ui_overlays[0].type: " << response.ui_overlays[0].type << '\n';
        std::cout << "  ui_overlays[0].target: " << response.ui_overlays[0].target << '\n';
    } else {
        std::cout << "  ui_overlays: []\n";
    }
}

PlannerResponse LocalRecipeFallback(const RecipeState& state, const std::string& command) {
    const RecipeStep* current = GetCurrentStep(state);
    const RecipeStep* next = GetNextStep(state);
    const std::string normalized = ToLower(command);

    PlannerResponse response{};
    response.speak = true;
    response.interruptible = true;
    response.advance_step = false;

    if (ContainsText(normalized, "repeat")) {
        response.assistant_text = current != nullptr ? "Repeat: " + current->instruction : "No current step available.";
        return response;
    }

    if (ContainsText(normalized, "current")) {
        response.assistant_text = current != nullptr ? "Current step: " + current->instruction : "No current step available.";
        return response;
    }

    if (ContainsText(normalized, "next")) {
        if (next != nullptr) {
            response.assistant_text = "Next instruction: " + next->instruction;
            response.advance_step = true;
        } else {
            response.assistant_text = "You are at the final step.";
        }
        return response;
    }

    response.assistant_text = "Say 'current' or 'next'.";
    return response;
}

bool IsSupportedGestureLabel(const std::string& label) {
    return label == "next" || label == "repeat" || label == "option_a" || label == "option_b" || label == "none";
}

class SpeechInputAdapter {
   public:
    virtual ~SpeechInputAdapter() = default;
    virtual bool TryConsumeConsoleLine(const std::string& line, TranscriptEvent& out_event) = 0;
    virtual bool TryConsumeLiveMicrophoneFinalizedEvent(TtsController& tts, TranscriptEvent& out_event) = 0;
    virtual std::string Name() const = 0;
};

class SidecarSpeechInputAdapter final : public SpeechInputAdapter {
   public:
    SidecarSpeechInputAdapter(std::string host, const int port) : host_(std::move(host)), port_(port) {}

    bool TryConsumeConsoleLine(const std::string& line, TranscriptEvent& out_event) override {
        const std::string prefix = "stt-file ";
        if (line.rfind(prefix, 0) == 0) {
            const std::string path = Trim(line.substr(prefix.size()));
            if (path.empty()) {
                Log("STT input ignored: missing wav path. Usage: stt-file <path_to_wav>");
                return false;
            }
            std::vector<uint8_t> wav_bytes;
            if (!ReadBinaryFile(path, wav_bytes)) {
                Log("STT input failed: cannot open audio file '" + path + "'.");
                return false;
            }
            const SttResult stt = RequestSttTranscription(host_, port_, wav_bytes, true);
            if (!stt.ok) {
                Log("STT sidecar failure: " + stt.error);
                return false;
            }
            out_event = TranscriptEvent{stt.text, stt.is_final};
            return true;
        }

        return false;
    }

    std::string Name() const override {
        return "python-sidecar-faster-whisper (live mic polling + stt-file/stt-stream-file debug)";
    }

    bool TryConsumeLiveMicrophoneFinalizedEvent(TtsController& tts, TranscriptEvent& out_event) override {
        if (!live_mic_supported_) {
            return false;
        }
        const std::string endpoint = EnvOrDefault("MIMOCA_STT_LIVE_FINALIZED_ENDPOINT", "/stt/live/finalized");
        const std::string body = "{}";
        const std::string request =
            "POST " + endpoint + " HTTP/1.1\r\n"
            "Host: " + host_ +
            "\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "Content-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;

        std::string response;
        if (!SendHttpRequest(host_, port_, request, response)) {
            if (!live_mic_warning_emitted_) {
                Log("Live mic polling endpoint unavailable; continuing without speech-driven turns.");
                live_mic_warning_emitted_ = true;
            }
            live_mic_supported_ = false;
            return false;
        }

        const std::string response_body = ExtractHttpBody(response);
        if (response.find("200 OK") == std::string::npos || response_body.empty()) {
            if (!live_mic_warning_emitted_) {
                Log("Live mic polling returned non-200; disabling live mic turns for this run.");
                live_mic_warning_emitted_ = true;
            }
            live_mic_supported_ = false;
            return false;
        }

        if (ExtractJsonFieldBool(response_body, "speech_started", false)) {
            tts.Stop();
        }
        if (!ExtractJsonFieldBool(response_body, "is_final", false)) {
            return false;
        }
        const std::string text = Trim(ExtractJsonFieldString(response_body, "text"));
        if (text.empty()) {
            return false;
        }
        out_event = TranscriptEvent{text, true};
        return true;
    }

    bool TryStreamWavWithVadInterruption(const std::string& line, TtsController& tts, TranscriptEvent& out_event) const {
        const std::string prefix = "stt-stream-file ";
        if (line.rfind(prefix, 0) != 0) {
            return false;
        }
        const std::string path = Trim(line.substr(prefix.size()));
        if (path.empty()) {
            Log("STT stream input ignored: missing wav path. Usage: stt-stream-file <path_to_wav>");
            return false;
        }
        std::vector<uint8_t> wav_bytes;
        if (!ReadBinaryFile(path, wav_bytes)) {
            Log("STT stream input failed: cannot open audio file '" + path + "'.");
            return false;
        }

        int sample_rate_hz = 16000;
        std::vector<uint8_t> pcm_s16le;
        if (!ParseWavPcm16MonoOrStereo(wav_bytes, sample_rate_hz, pcm_s16le)) {
            Log("STT stream input failed: only PCM16 WAV mono/stereo is supported.");
            return false;
        }

        std::string session_id;
        if (!StartStreamingSttSession(host_, port_, sample_rate_hz, session_id)) {
            Log("STT stream failed: cannot start sidecar session.");
            return false;
        }

        const size_t chunk_bytes = static_cast<size_t>(std::max(1, sample_rate_hz / 50) * 2);
        bool interrupted = false;
        std::string utterance_text;
        for (size_t offset = 0; offset < pcm_s16le.size(); offset += chunk_bytes) {
            const size_t remaining = pcm_s16le.size() - offset;
            const size_t take = std::min(chunk_bytes, remaining);
            std::vector<uint8_t> chunk(pcm_s16le.begin() + static_cast<long>(offset),
                                       pcm_s16le.begin() + static_cast<long>(offset + take));
            const StreamingSttChunkResult chunk_result = AppendStreamingSttChunk(host_, port_, session_id, chunk);
            if (!chunk_result.ok) {
                Log("STT stream chunk failed: " + chunk_result.error);
                return false;
            }
            if (!chunk_result.vad_available && !interrupted) {
                Log("VAD unavailable; falling back to manual interruption command ('stop').");
            }
            if (chunk_result.speech_started && !interrupted) {
                Log("VAD detected speech start; interrupting TTS immediately.");
                tts.Stop();
                interrupted = true;
            }
            if (chunk_result.is_final && !chunk_result.text.empty()) {
                if (!utterance_text.empty()) {
                    utterance_text += " ";
                }
                utterance_text += chunk_result.text;
            }
        }

        const SttResult final_result = FinalizeStreamingSttSession(host_, port_, session_id);
        if (!final_result.ok) {
            Log("STT stream finalize failed: " + final_result.error);
            return false;
        }
        if (!final_result.text.empty()) {
            utterance_text = final_result.text;
        }
        out_event = TranscriptEvent{utterance_text, true};
        return true;
    }

   private:
    std::string host_;
    int port_;
    bool live_mic_supported_ = true;
    bool live_mic_warning_emitted_ = false;
};

class DebugConsoleInput {
   public:
    bool TryPopLine(std::string& out_line) {
        if (std::cin.rdbuf()->in_avail() <= 0) {
            return false;
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            return false;
        }
        out_line = Trim(line);
        if (out_line.empty()) {
            return false;
        }
        return true;
    }

    void Stop() {}
};

class CameraController {
   public:
    ~CameraController() {
        Stop();
    }

    bool Start(const int device_index) {
#ifdef MIMOCA_HAS_OPENCV
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return true;
        }

        Log("Camera start requested on device index " + std::to_string(device_index) + ".");
        if (!capture_.open(device_index, cv::CAP_ANY)) {
            snapshot_.camera_started = false;
            snapshot_.frame_available = false;
            snapshot_.status_message = "camera unavailable (open failed)";
            Log("Camera unavailable: failed to open device.");
            return false;
        }

        running_ = true;
        capture_thread_ = std::thread([this]() { CaptureLoop(); });
        snapshot_.camera_started = true;
        snapshot_.status_message = "camera started; waiting for first frame";
        Log("Camera started.");
        return true;
#else
        (void)device_index;
        snapshot_.camera_started = false;
        snapshot_.frame_available = false;
        snapshot_.status_message = "camera disabled (OpenCV not linked)";
        Log("Camera path disabled: build without OpenCV.");
        return false;
#endif
    }

    void Stop() {
#ifdef MIMOCA_HAS_OPENCV
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }

        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_.isOpened()) {
            capture_.release();
        }
        snapshot_.camera_started = false;
        snapshot_.status_message = "camera stopped";
        Log("Camera stopped after " + std::to_string(snapshot_.frame_count) + " captured frames.");
#endif
    }

    CameraSnapshot GetLatestSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    bool TryEncodeLatestFrameJpeg(std::vector<uint8_t>& out_jpeg) const {
#ifdef MIMOCA_HAS_OPENCV
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_frame_.empty()) {
            return false;
        }
        std::vector<int> encode_params{cv::IMWRITE_JPEG_QUALITY, 70};
        return cv::imencode(".jpg", latest_frame_, out_jpeg, encode_params);
#else
        (void)out_jpeg;
        return false;
#endif
    }

   private:
#ifdef MIMOCA_HAS_OPENCV
    void CaptureLoop() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_) {
                    return;
                }
            }

            cv::Mat frame;
            if (!capture_.read(frame) || frame.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.frame_available = false;
                snapshot_.status_message = "camera frame read failed";
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                continue;
            }

            bool log_first_frame = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!snapshot_.frame_available) {
                    log_first_frame = true;
                }
                snapshot_.frame_available = true;
                snapshot_.width = frame.cols;
                snapshot_.height = frame.rows;
                snapshot_.capture_timestamp = MakeIsoTimestampNow();
                snapshot_.frame_count += 1;
                snapshot_.status_message = "camera running";
                latest_frame_ = frame.clone();
            }

            if (log_first_frame) {
                Log("Camera frame available (" + std::to_string(frame.cols) + "x" + std::to_string(frame.rows) + ").");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    mutable std::mutex mutex_;
    cv::VideoCapture capture_;
    std::thread capture_thread_;
    cv::Mat latest_frame_;
    bool running_ = false;
#else
    mutable std::mutex mutex_;
#endif
    CameraSnapshot snapshot_{};
};

}  // namespace

int main() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::cout << "[MiMoCA] Startup at " << std::ctime(&now_time);
    Log("Windows-first prototype shell is running.");

    RecipeState recipe_state{};
    if (!LoadFirstRecipe("assets/recipes.json", recipe_state.recipe)) {
        Log("Failed to load startup recipe; exiting.");
        return 1;
    }
    recipe_state.current_step_index = 0;

    const RecipeStep* startup_step = GetCurrentStep(recipe_state);
    if (startup_step != nullptr) {
        Log("Current recipe: " + recipe_state.recipe.id + " | current step: " + startup_step->id);
    }

    Log("Checking Python sidecar at http://127.0.0.1:8080/health");
    const bool sidecar_ok = CheckPythonHealth("127.0.0.1", 8080);
    if (!sidecar_ok) {
        Log("Continuing without Python sidecar (graceful degradation). Start python/service.py to enable it.");
    }

    TtsController tts;
    SidecarSpeechInputAdapter speech_input("127.0.0.1", 8080);
    CameraController camera;
    bool debug_mode_enabled = false;
    const std::string debug_env = ToLower(Trim(EnvOrDefault("MIMOCA_DEBUG", "0")));
    if (debug_env == "1" || debug_env == "true" || debug_env == "on") {
        debug_mode_enabled = true;
    }
    DebugSnapshot debug_snapshot{};
    camera.Start(0);
    Log("Speech input adapter: " + speech_input.Name());
    Log("Debug mode: " + std::string(debug_mode_enabled ? "enabled" : "disabled") +
        " (set MIMOCA_DEBUG=1 or use 'debug on').");

    Log("Continuous runtime loop active (speech + vision + gestures).");
    Log("Debug console fallback commands: debug on|off|status | camera | stop | stt-file <wav_path> | "
        "stt-stream-file <wav_path> | exit");

    DebugConsoleInput debug_console;
    bool running = true;
    bool startup_prompt_pending = true;
    bool gesture_unavailable_logged = false;
    bool vision_unavailable_logged = false;
    std::string last_triggered_gesture = "none";
    auto last_gesture_trigger = std::chrono::steady_clock::now() - std::chrono::seconds(3);

    auto process_turn = [&](const RuntimeIntent& intent, const std::vector<Detection>& detections,
                            const CameraSnapshot& camera_snapshot) {
        std::string utterance = intent.utterance;
        Gesture turn_gesture = intent.gesture;
        std::vector<Detection> turn_detections = detections;
        if (const std::string detected_branch = ResolveBranchSelection(recipe_state, utterance, turn_gesture);
            !detected_branch.empty()) {
            ApplyBranchSelection(recipe_state, detected_branch, turn_gesture.label == "none" ? "utterance" : "gesture");
        }

        PlannerResponse response{};
        const TurnContext turn_context = BuildTurnContext(recipe_state, utterance, turn_gesture, turn_detections, camera_snapshot);
        PlannerRoundTripStatus planner_status{};
        if (sidecar_ok) {
            planner_status.attempted = true;
            planner_status.source = "python_sidecar";
            const auto planner_start = std::chrono::steady_clock::now();
            if (!RequestMockPlanner("127.0.0.1", 8080, turn_context, response)) {
                const auto planner_end = std::chrono::steady_clock::now();
                planner_status.round_trip_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(planner_end - planner_start).count();
                planner_status.success = false;
                planner_status.used_fallback = true;
                Log("Planner call failed; using local recipe fallback for this turn.");
                response = LocalRecipeFallback(recipe_state, FallbackCommandForIntent(intent));
                planner_status.source = "local_fallback_after_sidecar_error";
            } else {
                const auto planner_end = std::chrono::steady_clock::now();
                planner_status.round_trip_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(planner_end - planner_start).count();
                planner_status.success = true;
            }
        } else {
            response = LocalRecipeFallback(recipe_state, FallbackCommandForIntent(intent));
            planner_status.attempted = false;
            planner_status.success = true;
            planner_status.used_fallback = true;
            planner_status.source = "local_fallback_sidecar_unavailable";
        }

        if (!response.new_branch_id.empty()) {
            std::string planner_branch = response.new_branch_id;
            if (planner_branch == "option_a" || planner_branch == "option_b") {
                const Gesture planner_gesture{planner_branch, 1.0};
                planner_branch = ResolveBranchSelection(recipe_state, "", planner_gesture);
            }
            if (!planner_branch.empty()) {
                ApplyBranchSelection(recipe_state, planner_branch, "planner_response");
            }
        }

        PrintPlannerResponse(response);
        if (response.speak && !response.assistant_text.empty()) {
            tts.Speak(response.assistant_text);
        }
        if (response.advance_step) {
            if (AdvanceCurrentStep(recipe_state)) {
                const RecipeStep* after = GetCurrentStep(recipe_state);
                if (after != nullptr) {
                    const std::string active_branch =
                        recipe_state.selected_branch_by_point.empty() ? "" : recipe_state.selected_branch_by_point.begin()->second;
                    Log("Advanced to step: " + after->id +
                        (active_branch.empty() ? "" : " (active branch: " + active_branch + ")"));
                }
            } else {
                Log("No next step to advance to.");
            }
        }

        const RecipeStep* debug_current_step = GetCurrentStep(recipe_state);
        debug_snapshot.transcript = utterance.empty() ? debug_snapshot.transcript : utterance;
        debug_snapshot.gesture = turn_gesture;
        debug_snapshot.detections = turn_context.detections;
        debug_snapshot.recipe_id = recipe_state.recipe.id;
        debug_snapshot.step_id = debug_current_step != nullptr ? debug_current_step->id : "(none)";
        debug_snapshot.branch_id =
            recipe_state.selected_branch_by_point.empty() ? "(none)" : recipe_state.selected_branch_by_point.begin()->second;
        debug_snapshot.planner = planner_status;
        if (debug_mode_enabled) {
            PrintDebugSnapshot(debug_snapshot);
        }
    };

    while (running) {
        RuntimeIntent intent{};
        const CameraSnapshot camera_snapshot = camera.GetLatestSnapshot();
        std::vector<Detection> turn_detections;
        std::vector<uint8_t> latest_frame_jpeg;
        const bool has_jpeg = sidecar_ok && camera.TryEncodeLatestFrameJpeg(latest_frame_jpeg);

        if (has_jpeg) {
            if (!RequestVisionDetections("127.0.0.1", 8080, latest_frame_jpeg, turn_detections)) {
                if (!vision_unavailable_logged) {
                    Log("Vision detection unavailable; continuing with detections=[].");
                    vision_unavailable_logged = true;
                }
            } else {
                vision_unavailable_logged = false;
            }
        }

        if (startup_prompt_pending) {
            startup_prompt_pending = false;
            intent.type = IntentType::kStartupReady;
            intent.command = "ready";
            intent.source = "internal";
        } else {
            TranscriptEvent transcript_event{};
            if (sidecar_ok && speech_input.TryConsumeLiveMicrophoneFinalizedEvent(tts, transcript_event)) {
                intent.type = IntentFromUtterance(transcript_event.text);
                intent.utterance = transcript_event.text;
                intent.command = transcript_event.text;
                intent.source = "speech_live_mic";
                debug_snapshot.transcript = transcript_event.text;
                Log("Speech final transcript: '" + transcript_event.text + "'");
            }
        }

        if (intent.type == IntentType::kNone && has_jpeg) {
            Gesture detected_gesture{"none", 0.0};
            if (RequestGestureDetection("127.0.0.1", 8080, latest_frame_jpeg, detected_gesture)) {
                if (IsSupportedGestureLabel(detected_gesture.label)) {
                    const auto now_tp = std::chrono::steady_clock::now();
                    const bool cooldown_elapsed =
                        (now_tp - last_gesture_trigger) >= std::chrono::milliseconds(1500);
                    if (detected_gesture.label != "none" && detected_gesture.confidence >= 0.6 &&
                        (detected_gesture.label != last_triggered_gesture || cooldown_elapsed)) {
                        intent.type = IntentFromGestureLabel(detected_gesture.label);
                        intent.gesture = detected_gesture;
                        intent.command = detected_gesture.label;
                        intent.source = "gesture";
                        last_triggered_gesture = detected_gesture.label;
                        last_gesture_trigger = now_tp;
                    }
                    debug_snapshot.gesture = detected_gesture;
                }
                gesture_unavailable_logged = false;
            } else if (!gesture_unavailable_logged) {
                Log("Gesture detection unavailable; continuing without gesture-triggered turns.");
                gesture_unavailable_logged = true;
            }
        }

        std::string debug_line;
        if (debug_console.TryPopLine(debug_line)) {
            if (debug_line == "exit") {
                running = false;
                continue;
            }
            if (debug_line == "stop") {
                tts.Stop();
            } else if (debug_line == "debug on") {
                debug_mode_enabled = true;
                Log("Debug mode enabled.");
            } else if (debug_line == "debug off") {
                debug_mode_enabled = false;
                Log("Debug mode disabled.");
            } else if (debug_line == "debug status") {
                Log("Debug mode is " + std::string(debug_mode_enabled ? "enabled" : "disabled") + ".");
                PrintDebugSnapshot(debug_snapshot);
            } else if (debug_line == "camera") {
                std::cout << "[MiMoCA] Camera status: " << camera_snapshot.status_message << '\n';
                std::cout << "  started: " << (camera_snapshot.camera_started ? "true" : "false") << '\n';
                std::cout << "  frame_available: " << (camera_snapshot.frame_available ? "true" : "false") << '\n';
                if (camera_snapshot.frame_available) {
                    std::cout << "  latest_frame: " << camera_snapshot.width << "x" << camera_snapshot.height << '\n';
                    std::cout << "  captured_at: " << camera_snapshot.capture_timestamp << '\n';
                    std::cout << "  frame_count: " << camera_snapshot.frame_count << '\n';
                }
            } else {
                TranscriptEvent debug_transcript{};
                if (speech_input.TryConsumeConsoleLine(debug_line, debug_transcript) ||
                    speech_input.TryStreamWavWithVadInterruption(debug_line, tts, debug_transcript)) {
                    if (!debug_transcript.text.empty()) {
                        intent.type = IntentFromUtterance(debug_transcript.text);
                        intent.utterance = debug_transcript.text;
                        intent.command = debug_transcript.text;
                        intent.source = "speech_debug_file";
                    }
                } else if (!debug_line.empty()) {
                    Log("Debug command ignored: " + debug_line);
                }
            }
        }

        if (intent.type == IntentType::kExitDebug) {
            running = false;
            continue;
        }
        if (intent.type != IntentType::kNone) {
            process_turn(intent, turn_detections, camera_snapshot);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    debug_console.Stop();

    camera.Stop();
    tts.Stop();
    Log("TTS interruption: VAD-driven interruption is available through streamed audio path; 'stop' remains manual fallback.");
    Log("Exiting MiMoCA prototype.");
    return 0;
}
