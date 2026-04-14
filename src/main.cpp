#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <vector>

#ifdef MIMOCA_HAS_QT
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#endif

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
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wincred.h>
#include <dpapi.h>
#include <sapi.h>
#include <sphelper.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Audioclient.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Crypt32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef MIMOCA_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
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
    bool speech_ended = false;
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

struct AppConfig {
    std::string sidecar_env_path;
    std::string planner_mode = "llm";
};

struct SidecarBootstrapResult {
    bool ok = false;
    std::string venv_path;
    std::string interpreter_path;
    std::string error;
    std::string raw_output;
};

void Log(const std::string& message);
std::string ReadFileText(const std::string& path);
bool SendHttpRequest(const std::string& host, int port, const std::string& request, std::string& response_out);

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
        speaking_ = true;
        speech_thread_ = std::thread([this, text]() {
            Log("TTS start: " + text);
            const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
                Log("TTS unavailable: COM initialization failed.");
                speaking_ = false;
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
                speaking_ = false;
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
            speaking_ = false;
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

    bool IsSpeaking() const {
        return speaking_;
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
    std::atomic<bool> speaking_{false};
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
    bool IsSpeaking() const { return false; }
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

int EnvIntOrDefault(const char* name, const int fallback) {
    const std::string raw = Trim(EnvOrDefault(name, ""));
    if (raw.empty()) {
        return fallback;
    }
    const char* begin = raw.c_str();
    char* end = nullptr;
    const long parsed = std::strtol(begin, &end, 10);
    if (end == begin || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

std::string DefaultAppConfigPath() {
    return EnvOrDefault("MIMOCA_APP_CONFIG_PATH", "mimoca_app_config.json");
}

AppConfig LoadAppConfig(const std::string& path) {
    AppConfig config{};
    const std::string json = ReadFileText(path);
    if (json.empty()) {
        return config;
    }
    config.sidecar_env_path = Trim(ExtractJsonFieldString(json, "sidecar_env_path"));
    config.planner_mode = ToLower(Trim(ExtractJsonFieldString(json, "planner_mode")));
    if (config.planner_mode != "llm" && config.planner_mode != "mock") {
        config.planner_mode = "llm";
    }
    return config;
}

bool SaveAppConfig(const std::string& path, const AppConfig& config) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        Log("Failed to write app config: " + path);
        return false;
    }
    output << "{\n";
    output << "  \"sidecar_env_path\": \"" << EscapeJson(config.sidecar_env_path) << "\",\n";
    output << "  \"planner_mode\": \"" << EscapeJson(config.planner_mode) << "\"\n";
    output << "}\n";
    return true;
}

std::string QuoteForShell(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('"');
    return escaped;
}

bool RunCommandCapture(const std::string& command, int& exit_code, std::string& output) {
    output.clear();
#ifdef _WIN32
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (pipe == nullptr) {
        exit_code = -1;
        return false;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#ifdef _WIN32
    exit_code = _pclose(pipe);
#else
    exit_code = pclose(pipe);
#endif
    return true;
}

std::string TrimToJsonObject(const std::string& text) {
    const size_t begin = text.find('{');
    const size_t end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        return "";
    }
    return text.substr(begin, end - begin + 1);
}

std::string SidecarInterpreterFromVenv(const std::string& venv_path) {
    const std::filesystem::path base(venv_path);
#ifdef _WIN32
    return (base / "Scripts" / "python.exe").string();
#else
    return (base / "bin" / "python3").string();
#endif
}

SidecarBootstrapResult BootstrapManagedSidecarEnv(const std::string& bootstrap_python,
                                                  const std::string& bootstrap_script,
                                                  const std::string& venv_path,
                                                  const std::string& requirements_path) {
    SidecarBootstrapResult result{};
    const std::string command = QuoteForShell(bootstrap_python) + " " + QuoteForShell(bootstrap_script) +
                                " --venv-path " + QuoteForShell(venv_path) + " --requirements " +
                                QuoteForShell(requirements_path);

    int exit_code = -1;
    std::string output;
    if (!RunCommandCapture(command, exit_code, output)) {
        result.error = "bootstrap_tool_unavailable";
        return result;
    }

    result.raw_output = output;
    const std::string json = TrimToJsonObject(output);
    if (!json.empty()) {
        result.ok = ExtractJsonFieldBool(json, "ok", false);
        result.venv_path = ExtractJsonFieldString(json, "venv_path");
        result.interpreter_path = ExtractJsonFieldString(json, "interpreter_path");
        result.error = ExtractJsonFieldString(json, "error");
    }

    if (result.interpreter_path.empty() && !result.venv_path.empty()) {
        result.interpreter_path = SidecarInterpreterFromVenv(result.venv_path);
    }
    if (result.venv_path.empty()) {
        result.venv_path = venv_path;
    }
    if (exit_code != 0 && result.error.empty()) {
        result.error = "bootstrap_exit_code_" + std::to_string(exit_code);
    }
    if (result.ok) {
        Log("Managed sidecar environment ready at: " + result.venv_path);
    } else {
        Log("Managed sidecar environment failed: " + result.error);
        Log("Bootstrap output: " + result.raw_output);
    }
    return result;
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

int ParseHttpStatusCode(const std::string& response) {
    const size_t first_space = response.find(' ');
    if (first_space == std::string::npos) {
        return 0;
    }
    const size_t second_space = response.find(' ', first_space + 1);
    const std::string token = response.substr(first_space + 1, second_space - first_space - 1);
    try {
        return std::stoi(token);
    } catch (const std::exception&) {
        return 0;
    }
}

bool SendJsonPost(const std::string& host,
                  int port,
                  const std::string& path,
                  const std::string& body,
                  int& status_code_out,
                  std::string& response_body_out) {
    const std::string request =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Content-Type: application/json\r\n"
               "Connection: close\r\n"
               "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        status_code_out = 0;
        response_body_out.clear();
        return false;
    }
    status_code_out = ParseHttpStatusCode(response);
    response_body_out = ExtractHttpBody(response);
    return true;
}

#ifdef _WIN32
constexpr wchar_t kPlannerApiKeyCredentialTarget[] = L"MiMoCA.OpenAIApiKey";

bool SavePlannerApiKeySecure(const std::string& api_key) {
    if (api_key.empty()) {
        return false;
    }
    DATA_BLOB in_blob{};
    in_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(api_key.data()));
    in_blob.cbData = static_cast<DWORD>(api_key.size());
    DATA_BLOB out_blob{};
    if (!CryptProtectData(&in_blob, L"MiMoCA planner API key", nullptr, nullptr, nullptr, 0, &out_blob)) {
        return false;
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(kPlannerApiKeyCredentialTarget);
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = out_blob.cbData;
    credential.CredentialBlob = out_blob.pbData;
    credential.UserName = const_cast<LPWSTR>(L"mimoca");
    const bool ok = CredWriteW(&credential, 0) != FALSE;
    LocalFree(out_blob.pbData);
    return ok;
}

bool LoadPlannerApiKeySecure(std::string& api_key_out) {
    api_key_out.clear();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kPlannerApiKeyCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) {
        return false;
    }
    DATA_BLOB in_blob{};
    in_blob.pbData = credential->CredentialBlob;
    in_blob.cbData = credential->CredentialBlobSize;
    DATA_BLOB out_blob{};
    const bool decrypted = CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr, 0, &out_blob) != FALSE;
    if (decrypted && out_blob.pbData != nullptr && out_blob.cbData > 0) {
        api_key_out.assign(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    }
    if (out_blob.pbData != nullptr) {
        LocalFree(out_blob.pbData);
    }
    CredFree(credential);
    return decrypted && !api_key_out.empty();
}

bool DeletePlannerApiKeySecure() {
    const BOOL deleted = CredDeleteW(kPlannerApiKeyCredentialTarget, CRED_TYPE_GENERIC, 0);
    if (deleted != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_NOT_FOUND;
}
#else
bool SavePlannerApiKeySecure(const std::string& api_key) {
    (void)api_key;
    return false;
}
bool LoadPlannerApiKeySecure(std::string& api_key_out) {
    api_key_out.clear();
    return false;
}
bool DeletePlannerApiKeySecure() { return false; }
#endif

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

struct SidecarHealthStatus {
    bool reachable = false;
    bool ready = false;
    std::string summary;
    double progress = 0.0;
    std::string body;
    std::string planner_mode = "llm";
    bool planner_llm_configured = false;
    bool planner_llm_ready = false;
    bool planner_fallback_active = false;
};

SidecarHealthStatus QueryPythonHealth(const std::string& host, int port) {
    SidecarHealthStatus health{};
    const std::string request =
        "GET /health HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Connection: close\r\n\r\n";

    std::string response;
    if (!SendHttpRequest(host, port, request, response)) {
        return health;
    }

    const bool http_ok = response.find("200 OK") != std::string::npos;
    const std::string body = ExtractHttpBody(response);
    health.reachable = response.find("HTTP/1.1") != std::string::npos;
    health.ready = http_ok && ExtractJsonFieldBool(body, "startup_ready", true);
    health.summary = ExtractJsonFieldString(body, "startup_summary");
    health.progress = ExtractJsonFieldDouble(body, "progress", 0.0);
    health.body = body;
    health.planner_mode = Trim(ExtractJsonFieldString(body, "planner_mode"));
    if (health.planner_mode.empty()) {
        health.planner_mode = "llm";
    }
    health.planner_llm_configured = ExtractJsonFieldBool(body, "planner_llm_configured", false);
    health.planner_llm_ready = ExtractJsonFieldBool(body, "planner_llm_ready", false);
    health.planner_fallback_active = ExtractJsonFieldBool(body, "planner_fallback_active", false);
    if (health.ready) {
        Log("Python sidecar health check succeeded.");
    } else {
        Log("Python sidecar health check not ready yet.");
    }
    Log("Sidecar response preview: " + response.substr(0, std::min<size_t>(response.size(), 180)));
    return health;
}

class PythonSidecarManager {
   public:
    struct StartOptions {
        std::string host = "127.0.0.1";
        int port = 8080;
        std::string python_path = "python";
        std::string script_path = "python/service.py";
        int startup_retries = 20;
        int startup_retry_delay_ms = 250;
    };

    using ProgressCallback = std::function<void(const std::string&)>;

    bool EnsureReady(const StartOptions& options, const ProgressCallback& progress_callback) {
        options_ = options;
        runtime_missing_ = false;
        runtime_missing_message_.clear();

        progress_callback("Initializing speech/vision services…");
        const SidecarHealthStatus initial_health = QueryPythonHealth(options.host, options.port);
        if (initial_health.ready) {
            ready_ = true;
            progress_callback("Speech/vision services ready.");
            return true;
        }

        progress_callback("Launching local speech/vision sidecar…");
        if (!Spawn(options)) {
            ready_ = false;
            runtime_missing_ = true;
            runtime_missing_message_ =
                "Python runtime missing or not executable. Install Python 3.11+ and set MIMOCA_PYTHON_EXECUTABLE.";
            progress_callback(runtime_missing_message_);
            return false;
        }

        for (int attempt = 1; attempt <= options.startup_retries; ++attempt) {
            SidecarHealthStatus health = QueryPythonHealth(options.host, options.port);
            std::string message = "Initializing speech/vision services… (" + std::to_string(attempt) + "/" +
                                  std::to_string(options.startup_retries) + ")";
            if (!health.summary.empty()) {
                message += " " + health.summary;
            }
            progress_callback(message);
            if (health.ready) {
                ready_ = true;
                progress_callback("Speech/vision services ready.");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(options.startup_retry_delay_ms));
        }

        ready_ = false;
        progress_callback("Speech/vision services are offline; using recipe-only fallback.");
        return false;
    }

    bool PollAndRestartIfNeeded(const ProgressCallback& progress_callback) {
        if (runtime_missing_) {
            ready_ = false;
            return false;
        }
        if (IsManagedProcessAlive()) {
            ready_ = QueryPythonHealth(options_.host, options_.port).ready;
            return ready_;
        }
        if (!is_spawned_) {
            ready_ = QueryPythonHealth(options_.host, options_.port).ready;
            return ready_;
        }

        progress_callback("Speech/vision service stopped unexpectedly; restarting…");
        ready_ = EnsureReady(options_, progress_callback);
        return ready_;
    }

    void Shutdown() {
        ready_ = false;
        if (!is_spawned_) {
            return;
        }
        Log("Stopping Python sidecar process.");
#ifdef _WIN32
        if (process_info_.hProcess != nullptr) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_info_.dwProcessId);
            if (WaitForSingleObject(process_info_.hProcess, 1500) == WAIT_TIMEOUT) {
                TerminateProcess(process_info_.hProcess, 0);
                WaitForSingleObject(process_info_.hProcess, 1000);
            }
            CloseHandle(process_info_.hThread);
            CloseHandle(process_info_.hProcess);
            process_info_ = {};
        }
#else
        if (child_pid_ > 0) {
            kill(child_pid_, SIGTERM);
            int status = 0;
            for (int attempt = 0; attempt < 20; ++attempt) {
                const pid_t result = waitpid(child_pid_, &status, WNOHANG);
                if (result == child_pid_) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (waitpid(child_pid_, &status, WNOHANG) == 0) {
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0);
            }
            child_pid_ = -1;
        }
#endif
        is_spawned_ = false;
    }

    bool ready() const { return ready_; }
    bool runtime_missing() const { return runtime_missing_; }
    const std::string& runtime_missing_message() const { return runtime_missing_message_; }

   private:
    bool Spawn(const StartOptions& options) {
#ifdef _WIN32
        std::string command_line = "\"" + options.python_path + "\" \"" + options.script_path + "\"";
        STARTUPINFOA startup_info{};
        startup_info.cb = sizeof(startup_info);
        ZeroMemory(&process_info_, sizeof(process_info_));
        std::vector<char> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back('\0');
        if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
                            nullptr, &startup_info, &process_info_)) {
            Log("Failed to spawn sidecar with command: " + command_line);
            return false;
        }
        is_spawned_ = true;
        Log("Spawned Python sidecar process.");
        return true;
#else
        const pid_t pid = fork();
        if (pid < 0) {
            Log("Failed to fork sidecar process.");
            return false;
        }
        if (pid == 0) {
            execlp(options.python_path.c_str(), options.python_path.c_str(), options.script_path.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        child_pid_ = pid;
        is_spawned_ = true;
        Log("Spawned Python sidecar process (pid=" + std::to_string(static_cast<long long>(pid)) + ").");
        return true;
#endif
    }

    bool IsManagedProcessAlive() {
        if (!is_spawned_) {
            return false;
        }
#ifdef _WIN32
        if (process_info_.hProcess == nullptr) {
            return false;
        }
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_info_.hProcess, &exit_code)) {
            return false;
        }
        if (exit_code != STILL_ACTIVE) {
            CloseHandle(process_info_.hThread);
            CloseHandle(process_info_.hProcess);
            process_info_ = {};
            is_spawned_ = false;
            return false;
        }
        return true;
#else
        if (child_pid_ <= 0) {
            return false;
        }
        int status = 0;
        const pid_t result = waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = -1;
            is_spawned_ = false;
            return false;
        }
        return result == 0;
#endif
    }

    StartOptions options_{};
    bool ready_ = false;
    bool is_spawned_ = false;
    bool runtime_missing_ = false;
    std::string runtime_missing_message_;
#ifdef _WIN32
    PROCESS_INFORMATION process_info_{};
#else
    pid_t child_pid_ = -1;
#endif
};

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

bool RequestPlannerConfigure(const std::string& host,
                             int port,
                             const std::string& mode,
                             const std::string& api_key,
                             std::string& error_out) {
    const std::string body = "{\"mode\":\"" + EscapeJson(mode) + "\",\"api_key\":\"" + EscapeJson(api_key) + "\"}";
    int status_code = 0;
    std::string response_body;
    if (!SendJsonPost(host, port, "/planner/configure", body, status_code, response_body)) {
        error_out = "sidecar_unreachable";
        return false;
    }
    if (status_code != 200) {
        error_out = ExtractJsonFieldString(response_body, "error");
        if (error_out.empty()) {
            error_out = "planner_configure_failed";
        }
        return false;
    }
    return true;
}

bool RequestPlannerValidateKey(const std::string& host, int port, const std::string& api_key, std::string& error_out) {
    const std::string body = "{\"api_key\":\"" + EscapeJson(api_key) + "\"}";
    int status_code = 0;
    std::string response_body;
    if (!SendJsonPost(host, port, "/planner/validate_key", body, status_code, response_body)) {
        error_out = "sidecar_unreachable";
        return false;
    }
    const bool ok = ExtractJsonFieldBool(response_body, "ok", status_code == 200);
    if (status_code != 200 || !ok) {
        error_out = ExtractJsonFieldString(response_body, "error");
        if (error_out.empty()) {
            error_out = "api_key_validation_failed";
        }
        return false;
    }
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
        return StreamingSttChunkResult{false, false, false, false, false, true, "", "sidecar_unreachable"};
    }
    const std::string response_body = ExtractHttpBody(response);
    if (response.find("200 OK") == std::string::npos || response_body.empty()) {
        return StreamingSttChunkResult{false, false, false, false, false, true, "", "stt_chunk_http_error"};
    }
    return StreamingSttChunkResult{
        true,
        ExtractJsonFieldBool(response_body, "speech_started", false),
        ExtractJsonFieldBool(response_body, "speech_ended", false),
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



#ifdef _WIN32
class WasapiMicCapture {
   public:
    using ChunkCallback = std::function<void(const std::vector<uint8_t>&, int)>;

    ~WasapiMicCapture() {
        Stop();
    }

    bool Start(const ChunkCallback& callback) {
        if (running_) {
            return true;
        }
        callback_ = callback;
        stop_requested_ = false;
        capture_thread_ = std::thread([this]() { CaptureLoop(); });
        return true;
    }

    void Stop() {
        stop_requested_ = true;
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        running_ = false;
    }

    bool running() const { return running_; }
    int sample_rate_hz() const { return sample_rate_hz_; }

   private:
    void CaptureLoop() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninit = SUCCEEDED(hr);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* audio_client = nullptr;
        IAudioCaptureClient* capture_client = nullptr;
        WAVEFORMATEX* mix_format = nullptr;
        HANDLE capture_event = nullptr;

        auto cleanup = [&]() {
            if (capture_event != nullptr) {
                CloseHandle(capture_event);
            }
            if (mix_format != nullptr) {
                CoTaskMemFree(mix_format);
            }
            if (capture_client != nullptr) {
                capture_client->Release();
            }
            if (audio_client != nullptr) {
                audio_client->Release();
            }
            if (device != nullptr) {
                device->Release();
            }
            if (enumerator != nullptr) {
                enumerator->Release();
            }
            if (uninit) {
                CoUninitialize();
            }
        };

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || enumerator == nullptr) {
            Log("WASAPI capture unavailable: cannot create MMDeviceEnumerator.");
            cleanup();
            return;
        }
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (FAILED(hr) || device == nullptr) {
            Log("WASAPI capture unavailable: default capture endpoint not found.");
            cleanup();
            return;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client));
        if (FAILED(hr) || audio_client == nullptr) {
            Log("WASAPI capture unavailable: failed to activate audio client.");
            cleanup();
            return;
        }
        hr = audio_client->GetMixFormat(&mix_format);
        if (FAILED(hr) || mix_format == nullptr) {
            Log("WASAPI capture unavailable: failed to query mix format.");
            cleanup();
            return;
        }

        sample_rate_hz_ = static_cast<int>(mix_format->nSamplesPerSec);
        capture_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (capture_event == nullptr) {
            Log("WASAPI capture unavailable: failed to create capture event.");
            cleanup();
            return;
        }

        hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      0,
                                      0,
                                      mix_format,
                                      nullptr);
        if (FAILED(hr)) {
            Log("WASAPI capture unavailable: audio client initialize failed.");
            cleanup();
            return;
        }
        hr = audio_client->SetEventHandle(capture_event);
        if (FAILED(hr)) {
            Log("WASAPI capture unavailable: SetEventHandle failed.");
            cleanup();
            return;
        }
        hr = audio_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_client));
        if (FAILED(hr) || capture_client == nullptr) {
            Log("WASAPI capture unavailable: capture service unavailable.");
            cleanup();
            return;
        }
        hr = audio_client->Start();
        if (FAILED(hr)) {
            Log("WASAPI capture unavailable: could not start capture.");
            cleanup();
            return;
        }

        running_ = true;
        Log("WASAPI capture started at " + std::to_string(sample_rate_hz_) + " Hz.");

        while (!stop_requested_) {
            const DWORD wait = WaitForSingleObject(capture_event, 200);
            if (wait != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 packet_frames = 0;
            hr = capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                break;
            }

            while (packet_frames > 0 && !stop_requested_) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    break;
                }
                if (frames > 0) {
                    std::vector<uint8_t> pcm_mono;
                    if (ConvertToMonoPcm16(*mix_format, data, frames, flags, pcm_mono) && !pcm_mono.empty() && callback_) {
                        callback_(pcm_mono, sample_rate_hz_);
                    }
                }
                capture_client->ReleaseBuffer(frames);
                hr = capture_client->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) {
                    break;
                }
            }
        }

        audio_client->Stop();
        running_ = false;
        Log("WASAPI capture stopped.");
        cleanup();
    }

    static bool ConvertToMonoPcm16(const WAVEFORMATEX& wf,
                                   const BYTE* data,
                                   const UINT32 frames,
                                   const DWORD flags,
                                   std::vector<uint8_t>& out_pcm) {
        out_pcm.clear();
        const int channels = std::max(1, static_cast<int>(wf.nChannels));
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
            out_pcm.assign(static_cast<size_t>(frames) * 2U, 0);
            return true;
        }

        if (wf.wFormatTag == WAVE_FORMAT_PCM && wf.wBitsPerSample == 16) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(data);
            out_pcm.reserve(static_cast<size_t>(frames) * 2U);
            for (UINT32 i = 0; i < frames; ++i) {
                int sum = 0;
                for (int c = 0; c < channels; ++c) {
                    sum += samples[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)];
                }
                const int16_t mono = static_cast<int16_t>(sum / channels);
                out_pcm.push_back(static_cast<uint8_t>(mono & 0xFF));
                out_pcm.push_back(static_cast<uint8_t>((mono >> 8) & 0xFF));
            }
            return true;
        }

        if (wf.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf.wBitsPerSample == 32) {
            const float* samples = reinterpret_cast<const float*>(data);
            out_pcm.reserve(static_cast<size_t>(frames) * 2U);
            for (UINT32 i = 0; i < frames; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    sum += samples[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)];
                }
                const float mono_f = std::clamp(sum / static_cast<float>(channels), -1.0f, 1.0f);
                const int16_t mono = static_cast<int16_t>(mono_f * 32767.0f);
                out_pcm.push_back(static_cast<uint8_t>(mono & 0xFF));
                out_pcm.push_back(static_cast<uint8_t>((mono >> 8) & 0xFF));
            }
            return true;
        }

        Log("WASAPI capture format unsupported: bits=" + std::to_string(wf.wBitsPerSample));
        return false;
    }

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
    ChunkCallback callback_;
    int sample_rate_hz_ = 16000;
};
#else
class WasapiMicCapture {
   public:
    using ChunkCallback = std::function<void(const std::vector<uint8_t>&, int)>;
    bool Start(const ChunkCallback& callback) {
        (void)callback;
        return false;
    }
    void Stop() {}
    bool running() const { return false; }
    int sample_rate_hz() const { return 16000; }
};
#endif
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

    ~SidecarSpeechInputAdapter() override {
        mic_capture_.Stop();
    }

    bool StartConversation() {
#ifdef _WIN32
        if (!mic_capture_.running()) {
            const bool started = mic_capture_.Start([this](const std::vector<uint8_t>& pcm, const int sample_rate_hz) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                sample_rate_hz_ = sample_rate_hz;
                pending_pcm_chunks_.push_back(pcm);
                if (pending_pcm_chunks_.size() > 256) {
                    pending_pcm_chunks_.pop_front();
                }
            });
            if (!started) {
                Log("WASAPI microphone capture unavailable; speech turns disabled.");
                return false;
            }
        }

        if (session_id_.empty()) {
            std::string session_id;
            if (!StartStreamingSttSession(host_, port_, sample_rate_hz_, session_id)) {
                return false;
            }
            session_id_ = session_id;
            Log("STT streaming session started.");
        }
        return true;
#else
        return false;
#endif
    }

    void OnSidecarRestarted() {
        session_id_.clear();
    }

    bool ConsumeSpeechStartEvent() {
        if (!pending_speech_start_event_) {
            return false;
        }
        pending_speech_start_event_ = false;
        return true;
    }

    bool speech_active() const {
        return speech_active_;
    }

    bool vad_available() const {
        return vad_available_;
    }

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
        return "python-sidecar-faster-whisper (WASAPI mic streaming + optional stt-file debug)";
    }

    bool TryConsumeLiveMicrophoneFinalizedEvent(TtsController& tts, TranscriptEvent& out_event) override {
        (void)tts;
        out_event = TranscriptEvent{};
        if (!StartConversation()) {
            return false;
        }

        std::vector<std::vector<uint8_t>> chunks;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            while (!pending_pcm_chunks_.empty()) {
                chunks.push_back(std::move(pending_pcm_chunks_.front()));
                pending_pcm_chunks_.pop_front();
            }
        }
        if (chunks.empty()) {
            return false;
        }

        bool saw_final = false;
        std::string final_text;
        for (const auto& chunk : chunks) {
            if (chunk.empty()) {
                continue;
            }
            const StreamingSttChunkResult chunk_result = AppendStreamingSttChunk(host_, port_, session_id_, chunk);
            if (!chunk_result.ok) {
                session_id_.clear();
                if (!stt_error_logged_) {
                    Log("Live mic streaming chunk failed: " + chunk_result.error);
                    stt_error_logged_ = true;
                }
                return false;
            }
            stt_error_logged_ = false;
            vad_available_ = chunk_result.vad_available;
            speech_active_ = chunk_result.speech_active;
            if (chunk_result.speech_started) {
                const auto now = std::chrono::steady_clock::now();
                if (!last_speech_start_at_.has_value() ||
                    now - *last_speech_start_at_ >= std::chrono::milliseconds(kSpeechStartDebounceMs)) {
                    pending_speech_start_event_ = true;
                    last_speech_start_at_ = now;
                    Log("VAD speech-start emitted from live mic stream.");
                }
            }
            if ((chunk_result.speech_ended || chunk_result.is_final) && !chunk_result.text.empty()) {
                saw_final = true;
                final_text = chunk_result.text;
            }
        }

        if (!saw_final) {
            return false;
        }

        out_event = TranscriptEvent{Trim(final_text), true};
        return !out_event.text.empty();
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
            if ((chunk_result.speech_ended || chunk_result.is_final) && !chunk_result.text.empty()) {
                utterance_text = chunk_result.text;
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
    static constexpr int kSpeechStartDebounceMs = 220;
    mutable std::mutex queue_mutex_;
    std::deque<std::vector<uint8_t>> pending_pcm_chunks_;
    std::string session_id_;
    int sample_rate_hz_ = 16000;
    bool stt_error_logged_ = false;
    bool pending_speech_start_event_ = false;
    bool speech_active_ = false;
    bool vad_available_ = true;
    std::optional<std::chrono::steady_clock::time_point> last_speech_start_at_;
    WasapiMicCapture mic_capture_;
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

    bool TryGetLatestFrameBgrCopy(
#ifdef MIMOCA_HAS_OPENCV
        cv::Mat& out_frame
#else
        std::vector<uint8_t>& out_frame
#endif
    ) const {
#ifdef MIMOCA_HAS_OPENCV
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_frame_.empty()) {
            return false;
        }
        out_frame = latest_frame_.clone();
        return true;
#else
        (void)out_frame;
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


#ifdef MIMOCA_HAS_QT
class MainWindow : public QMainWindow {
   public:
    MainWindow() {
        setWindowTitle("MiMoCA");
        resize(1280, 760);
        if (!LoadFirstRecipe("assets/recipes.json", recipe_state_.recipe)) {
            Log("Failed to load startup recipe; exiting.");
            startup_failed_ = true;
            return;
        }
        recipe_state_.current_step_index = 0;

        const std::string debug_env = ToLower(Trim(EnvOrDefault("MIMOCA_DEBUG", "0")));
        debug_mode_enabled_ = debug_env == "1" || debug_env == "true" || debug_env == "on";

        SetupUi();
        InitializeSidecarWithProgress();
        EnsurePlannerApiKeyAtStartup();
        camera_.Start(0);
        RefreshStatusIndicators();

        poll_timer_.setInterval(120);
        connect(&poll_timer_, &QTimer::timeout, this, [this]() { OnPollTick(); });
        poll_timer_.start();

        startup_prompt_pending_ = true;
        AppendChat("system", "UI ready. Ask 'what next?' by voice or type below.");
    }

    ~MainWindow() override {
        sidecar_manager_.Shutdown();
    }

    bool StartupFailed() const { return startup_failed_; }

   private:
    void InitializeSidecarWithProgress() {
        app_config_path_ = DefaultAppConfigPath();
        app_config_ = LoadAppConfig(app_config_path_);

        PythonSidecarManager::StartOptions options;
        options.host = EnvOrDefault("MIMOCA_SIDECAR_HOST", "127.0.0.1");
        options.port = EnvIntOrDefault("MIMOCA_SIDECAR_PORT", 8080);
        options.python_path = ResolveManagedSidecarInterpreter();
        options.script_path = EnvOrDefault("MIMOCA_SIDECAR_SCRIPT", "python/service.py");
        options.startup_retries = EnvIntOrDefault("MIMOCA_SIDECAR_HEALTH_RETRIES", 20);
        options.startup_retry_delay_ms = EnvIntOrDefault("MIMOCA_SIDECAR_HEALTH_RETRY_MS", 250);

        const auto progress = [this](const std::string& message) {
            statusBar()->showMessage(QString::fromStdString(message));
            sidecar_status_->setText(("sidecar: " + message).c_str());
            QApplication::processEvents();
            Log(message);
        };

        if (options.python_path.empty()) {
            sidecar_ok_ = false;
            const std::string message =
                "Could not prepare Python sidecar environment. Open Settings or installer repair and retry sidecar setup.";
            AppendChat("system", message);
            sidecar_status_->setText("sidecar: setup failed");
            return;
        }

        sidecar_ok_ = sidecar_manager_.EnsureReady(options, progress);
        if (sidecar_ok_) {
            if (!speech_input_.StartConversation()) {
                AppendChat("system", "Microphone capture unavailable. Voice turn automation is disabled.");
            }
        }
        if (sidecar_manager_.runtime_missing()) {
            AppendChat("system",
                       "Speech/vision sidecar unavailable: Python runtime not found. Install Python 3.11+ and set "
                       "MIMOCA_PYTHON_EXECUTABLE.");
        }
        statusBar()->clearMessage();
    }

    std::string ResolveManagedSidecarInterpreter() {
        const std::string explicit_python = Trim(EnvOrDefault("MIMOCA_PYTHON_EXECUTABLE", ""));
        if (!explicit_python.empty()) {
            Log("Using explicit sidecar python from MIMOCA_PYTHON_EXECUTABLE.");
            return explicit_python;
        }

        const std::string configured_env = Trim(app_config_.sidecar_env_path);
        if (!configured_env.empty()) {
            const std::string configured_interpreter = SidecarInterpreterFromVenv(configured_env);
            if (std::filesystem::exists(std::filesystem::path(configured_interpreter))) {
                Log("Using persisted managed sidecar environment: " + configured_env);
                return configured_interpreter;
            }
            Log("Persisted sidecar environment missing, will bootstrap again: " + configured_env);
        }

        const std::filesystem::path default_env_path = std::filesystem::path(".mimoca_sidecar_venv");
        const std::string bootstrap_python = EnvOrDefault("MIMOCA_BOOTSTRAP_PYTHON", "python");
        const SidecarBootstrapResult bootstrap = BootstrapManagedSidecarEnv(
            bootstrap_python, "python/bootstrap_sidecar_env.py", default_env_path.string(), "python/requirements.txt");

        if (bootstrap.ok && !bootstrap.interpreter_path.empty()) {
            app_config_.sidecar_env_path = bootstrap.venv_path;
            SaveAppConfig(app_config_path_, app_config_);
            return bootstrap.interpreter_path;
        }

        return ShowBootstrapFailureAndRetry(default_env_path.string(), bootstrap_python, bootstrap);
    }

    std::string ShowBootstrapFailureAndRetry(const std::string& default_env_path,
                                             const std::string& bootstrap_python,
                                             const SidecarBootstrapResult& first_result) {
        SidecarBootstrapResult last_result = first_result;
        while (true) {
            QString detail = QString::fromStdString(
                "Automatic sidecar setup failed.\n\n"
                "What failed: " +
                (last_result.error.empty() ? std::string("unknown error") : last_result.error) +
                "\n\nActions:\n"
                "1) Ensure Python 3.11+ is installed and on PATH.\n"
                "2) Verify internet access for dependency download.\n"
                "3) Use Retry after fixing the issue.");
            if (!last_result.raw_output.empty()) {
                detail += "\n\nInstaller output:\n" + QString::fromStdString(last_result.raw_output);
            }

            const auto button = QMessageBox::critical(
                this, "Sidecar setup failed", detail, QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
            if (button != QMessageBox::Retry) {
                return "";
            }

            last_result = BootstrapManagedSidecarEnv(bootstrap_python, "python/bootstrap_sidecar_env.py",
                                                     default_env_path, "python/requirements.txt");
            if (last_result.ok && !last_result.interpreter_path.empty()) {
                app_config_.sidecar_env_path = last_result.venv_path;
                SaveAppConfig(app_config_path_, app_config_);
                return last_result.interpreter_path;
            }
        }
    }

    bool ProductPolicyAllowsMockSkip() const {
        const std::string raw = ToLower(Trim(EnvOrDefault("MIMOCA_ALLOW_PLANNER_SKIP_TO_MOCK", "false")));
        return raw == "1" || raw == "true" || raw == "yes" || raw == "on";
    }

    bool ConfigurePlannerLlmWithKey(const std::string& api_key, std::string& error_out) {
        if (!sidecar_ok_) {
            error_out = "sidecar_unavailable";
            return false;
        }
        if (!RequestPlannerValidateKey("127.0.0.1", 8080, api_key, error_out)) {
            return false;
        }
        if (!SavePlannerApiKeySecure(api_key)) {
            error_out = "secure_store_write_failed";
            return false;
        }
        if (!RequestPlannerConfigure("127.0.0.1", 8080, "llm", api_key, error_out)) {
            return false;
        }
        sidecar_status_->setText("sidecar: planner llm configured");
        return true;
    }

    void EnsurePlannerApiKeyAtStartup() {
        if (!sidecar_ok_) {
            return;
        }
        const SidecarHealthStatus health = QueryPythonHealth("127.0.0.1", 8080);
        planner_mode_ = health.planner_mode;
        planner_llm_configured_ = health.planner_llm_configured;
        planner_fallback_active_ = health.planner_fallback_active;
        if (health.planner_mode != "llm" || health.planner_llm_ready) {
            return;
        }

        std::string stored_key;
        if (LoadPlannerApiKeySecure(stored_key)) {
            std::string configure_error;
            if (ConfigurePlannerLlmWithKey(stored_key, configure_error)) {
                AppendChat("system", "Planner API key loaded securely. LLM mode enabled.");
                return;
            }
            Log("Stored planner API key could not be used: " + configure_error);
        }
        ShowPlannerSettingsDialog(true);
    }

    void RevokePlannerKey() {
        const bool deleted = DeletePlannerApiKeySecure();
        std::string error;
        if (sidecar_ok_) {
            RequestPlannerConfigure("127.0.0.1", 8080, "mock", "", error);
        }
        if (!deleted) {
            QMessageBox::warning(this, "Planner key revoke", "Could not remove stored API key from secure storage.");
            return;
        }
        AppendChat("system", "Planner key revoked. Running in mock planner mode.");
    }

    void ShowPlannerSettingsDialog(bool startup_required) {
        const bool allow_skip_to_mock = ProductPolicyAllowsMockSkip();
        while (true) {
            QDialog dialog(this);
            dialog.setWindowTitle(startup_required ? "Planner setup required" : "Planner settings");
            auto* layout = new QVBoxLayout(&dialog);
            auto* info = new QLabel(
                startup_required
                    ? "Planner mode is set to LLM and no valid API key is configured.\nEnter an OpenAI API key to continue."
                    : "Enter an OpenAI API key to update planner credentials.",
                &dialog);
            info->setWordWrap(true);
            layout->addWidget(info);

            auto* key_input = new QLineEdit(&dialog);
            key_input->setPlaceholderText("sk-...");
            key_input->setEchoMode(QLineEdit::Password);
            layout->addWidget(key_input);

            auto* button_row = new QHBoxLayout();
            auto* save_btn = new QPushButton("Validate + Save", &dialog);
            auto* revoke_btn = new QPushButton("Revoke key", &dialog);
            auto* cancel_btn = new QPushButton(startup_required ? "Cancel" : "Close", &dialog);
            button_row->addWidget(save_btn);
            button_row->addWidget(revoke_btn);
            if (startup_required && allow_skip_to_mock) {
                auto* skip_btn = new QPushButton("Skip for now (mock mode)", &dialog);
                button_row->addWidget(skip_btn);
                connect(skip_btn, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(2); });
            }
            button_row->addWidget(cancel_btn);
            layout->addLayout(button_row);

            connect(save_btn, &QPushButton::clicked, &dialog, [&dialog]() { dialog.accept(); });
            connect(revoke_btn, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(3); });
            connect(cancel_btn, &QPushButton::clicked, &dialog, [&dialog]() { dialog.reject(); });

            const int result = dialog.exec();
            if (result == QDialog::Accepted) {
                const std::string api_key = Trim(key_input->text().toStdString());
                if (api_key.empty()) {
                    QMessageBox::warning(this, "Planner key required", "API key cannot be empty.");
                    continue;
                }
                std::string configure_error;
                if (!ConfigurePlannerLlmWithKey(api_key, configure_error)) {
                    QMessageBox::warning(this, "Planner key invalid",
                                         QString::fromStdString("Could not validate/save API key: " + configure_error));
                    continue;
                }
                AppendChat("system", "Planner API key updated. LLM mode is active.");
                return;
            }
            if (result == 3) {
                RevokePlannerKey();
                if (startup_required && !allow_skip_to_mock) {
                    continue;
                }
                return;
            }
            if (result == 2 && startup_required && allow_skip_to_mock) {
                std::string configure_error;
                if (!RequestPlannerConfigure("127.0.0.1", 8080, "mock", "", configure_error)) {
                    QMessageBox::warning(this, "Planner fallback failed",
                                         QString::fromStdString("Could not switch planner to mock mode: " + configure_error));
                    continue;
                }
                AppendChat("system", "Planner key setup skipped by policy. Running mock mode.");
                return;
            }
            if (!startup_required) {
                return;
            }
            if (!allow_skip_to_mock) {
                QMessageBox::warning(this, "Planner setup required",
                                     "A valid API key is required by product policy for current planner mode.");
                continue;
            }
            return;
        }
    }

    void SetupUi() {
        auto* central = new QWidget(this);
        auto* root = new QHBoxLayout(central);
        root->setContentsMargins(10, 10, 10, 10);

        auto* left = new QVBoxLayout();
        camera_label_ = new QLabel("Camera preview unavailable", this);
        camera_label_->setMinimumSize(880, 640);
        camera_label_->setStyleSheet("QLabel { background: #111; color: #aaa; border: 1px solid #333; }");
        camera_label_->setAlignment(Qt::AlignCenter);
        left->addWidget(camera_label_, 1);

        auto* right = new QVBoxLayout();
        right->setSpacing(8);
        right->setContentsMargins(0, 0, 0, 0);

        chat_history_ = new QPlainTextEdit(this);
        chat_history_->setReadOnly(true);
        chat_history_->setMinimumWidth(320);
        chat_history_->setMaximumWidth(380);
        right->addWidget(chat_history_, 1);

        auto* input_row = new QHBoxLayout();
        input_line_ = new QLineEdit(this);
        input_line_->setPlaceholderText("Type a question (developer/manual input)");
        auto* send_btn = new QPushButton("Send", this);
        input_row->addWidget(input_line_, 1);
        input_row->addWidget(send_btn);
        right->addLayout(input_row);

        planner_settings_btn_ = new QPushButton("Planner settings", this);
        right->addWidget(planner_settings_btn_);

        dev_toggle_ = new QCheckBox("Developer debug", this);
        dev_toggle_->setChecked(debug_mode_enabled_);
        right->addWidget(dev_toggle_);

        debug_text_ = new QPlainTextEdit(this);
        debug_text_->setReadOnly(true);
        debug_text_->setMaximumHeight(130);
        debug_text_->setVisible(debug_mode_enabled_);
        right->addWidget(debug_text_);

        root->addLayout(left, 4);
        root->addLayout(right, 1);
        setCentralWidget(central);

        mic_status_ = new QLabel("mic: idle", this);
        tts_status_ = new QLabel("tts: idle", this);
        gesture_status_ = new QLabel("gesture: idle", this);
        planner_status_ = new QLabel("planner: unknown", this);
        sidecar_status_ = new QLabel(sidecar_ok_ ? "sidecar: healthy" : "sidecar: unavailable", this);
        statusBar()->addPermanentWidget(mic_status_);
        statusBar()->addPermanentWidget(tts_status_);
        statusBar()->addPermanentWidget(gesture_status_);
        statusBar()->addPermanentWidget(planner_status_);
        statusBar()->addPermanentWidget(sidecar_status_);

        connect(send_btn, &QPushButton::clicked, this, [this]() { SubmitManualUtterance(); });
        connect(input_line_, &QLineEdit::returnPressed, this, [this]() { SubmitManualUtterance(); });
        connect(dev_toggle_, &QCheckBox::toggled, this, [this](const bool checked) {
            debug_mode_enabled_ = checked;
            debug_text_->setVisible(checked);
            RefreshStatusIndicators();
        });
        connect(planner_settings_btn_, &QPushButton::clicked, this, [this]() { ShowPlannerSettingsDialog(false); });
    }

    void SubmitManualUtterance() {
        const std::string utterance = Trim(input_line_->text().toStdString());
        if (utterance.empty()) {
            return;
        }
        const std::string normalized = ToLower(utterance);
        if (normalized == "stop" || normalized == "stop speaking" || normalized == "quiet") {
            tts_.Stop();
            AppendChat("system", "Manual stop applied.");
            RefreshStatusIndicators();
            input_line_->clear();
            return;
        }
        input_line_->clear();
        RuntimeIntent intent{};
        intent.type = IntentFromUtterance(utterance);
        intent.utterance = utterance;
        intent.command = utterance;
        intent.source = "ui_text";
        AppendChat("user", utterance);
        ProcessTurn(intent);
    }

    void OnPollTick() {
        UpdateCameraPreview();

        if (startup_prompt_pending_) {
            startup_prompt_pending_ = false;
            RuntimeIntent intent{};
            intent.type = IntentType::kStartupReady;
            intent.command = "ready";
            intent.source = "internal";
            ProcessTurn(intent);
            return;
        }

        TranscriptEvent transcript_event{};
        if (sidecar_ok_ && speech_input_.TryConsumeLiveMicrophoneFinalizedEvent(tts_, transcript_event) &&
            !transcript_event.text.empty()) {
            RuntimeIntent intent{};
            intent.type = IntentFromUtterance(transcript_event.text);
            intent.utterance = transcript_event.text;
            intent.command = transcript_event.text;
            intent.source = "speech_live_mic";
            AppendChat("user", transcript_event.text);
            ProcessTurn(intent);
        }
        if (speech_input_.ConsumeSpeechStartEvent()) {
            tts_.Stop();
            last_speech_start_at_ = std::chrono::steady_clock::now();
        }
        mic_active_ = speech_input_.speech_active();
        vad_available_ = speech_input_.vad_available();

        if (sidecar_ok_) {
            std::vector<uint8_t> latest_frame_jpeg;
            if (camera_.TryEncodeLatestFrameJpeg(latest_frame_jpeg)) {
                Gesture detected_gesture{"none", 0.0};
                if (RequestGestureDetection("127.0.0.1", 8080, latest_frame_jpeg, detected_gesture)) {
                    gesture_active_ = detected_gesture.label != "none" && detected_gesture.confidence >= 0.6;
                }
            }
        }
        const bool sidecar_previously_ok = sidecar_ok_;
        sidecar_ok_ = sidecar_manager_.PollAndRestartIfNeeded([this](const std::string& message) {
            statusBar()->showMessage(QString::fromStdString(message), 3000);
            Log(message);
        });
        if (sidecar_ok_ && !sidecar_previously_ok) {
            speech_input_.OnSidecarRestarted();
            speech_input_.StartConversation();
        }
        if (sidecar_ok_) {
            const SidecarHealthStatus health = QueryPythonHealth("127.0.0.1", 8080);
            planner_mode_ = health.planner_mode;
            planner_llm_configured_ = health.planner_llm_configured;
            planner_fallback_active_ = health.planner_fallback_active;
        } else {
            planner_fallback_active_ = true;
        }
        if (!sidecar_ok_ && sidecar_manager_.runtime_missing()) {
            sidecar_status_->setText("sidecar: python runtime missing (see log)");
        }
        RefreshStatusIndicators();
    }

    void UpdateCameraPreview() {
#ifdef MIMOCA_HAS_OPENCV
        cv::Mat frame_bgr;
        if (!camera_.TryGetLatestFrameBgrCopy(frame_bgr) || frame_bgr.empty()) {
            return;
        }
        cv::Mat frame_rgb;
        cv::cvtColor(frame_bgr, frame_rgb, cv::COLOR_BGR2RGB);
        QImage image(frame_rgb.data, frame_rgb.cols, frame_rgb.rows, static_cast<int>(frame_rgb.step),
                     QImage::Format_RGB888);
        camera_label_->setPixmap(QPixmap::fromImage(image).scaled(camera_label_->size(), Qt::KeepAspectRatio,
                                                               Qt::SmoothTransformation));
#endif
    }

    void ProcessTurn(const RuntimeIntent& intent) {
        const CameraSnapshot camera_snapshot = camera_.GetLatestSnapshot();
        std::vector<Detection> turn_detections;
        std::vector<uint8_t> latest_frame_jpeg;
        const bool has_jpeg = sidecar_ok_ && camera_.TryEncodeLatestFrameJpeg(latest_frame_jpeg);
        if (has_jpeg) {
            RequestVisionDetections("127.0.0.1", 8080, latest_frame_jpeg, turn_detections);
        }

        std::string utterance = intent.utterance;
        Gesture turn_gesture = intent.gesture;
        if (const std::string detected_branch = ResolveBranchSelection(recipe_state_, utterance, turn_gesture);
            !detected_branch.empty()) {
            ApplyBranchSelection(recipe_state_, detected_branch, turn_gesture.label == "none" ? "utterance" : "gesture");
        }

        PlannerResponse response{};
        PlannerRoundTripStatus planner_status{};
        const TurnContext turn_context = BuildTurnContext(recipe_state_, utterance, turn_gesture, turn_detections, camera_snapshot);

        if (sidecar_ok_) {
            planner_status.attempted = true;
            planner_status.source = "python_sidecar";
            const auto planner_start = std::chrono::steady_clock::now();
            if (!RequestMockPlanner("127.0.0.1", 8080, turn_context, response)) {
                response = LocalRecipeFallback(recipe_state_, FallbackCommandForIntent(intent));
                planner_status.success = false;
                planner_status.used_fallback = true;
                planner_status.source = "local_fallback_after_sidecar_error";
            } else {
                planner_status.success = true;
            }
            const auto planner_end = std::chrono::steady_clock::now();
            planner_status.round_trip_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(planner_end - planner_start).count();
        } else {
            response = LocalRecipeFallback(recipe_state_, FallbackCommandForIntent(intent));
            planner_status.success = true;
            planner_status.used_fallback = true;
            planner_status.source = "local_fallback_sidecar_unavailable";
        }

        if (!response.assistant_text.empty()) {
            AppendChat("assistant", response.assistant_text);
        }
        if (response.speak && !response.assistant_text.empty()) {
            tts_.Speak(response.assistant_text);
        }
        if (response.advance_step) {
            AdvanceCurrentStep(recipe_state_);
        }

        debug_snapshot_.transcript = utterance.empty() ? debug_snapshot_.transcript : utterance;
        debug_snapshot_.gesture = turn_gesture;
        debug_snapshot_.detections = turn_context.detections;
        debug_snapshot_.recipe_id = recipe_state_.recipe.id;
        const RecipeStep* current = GetCurrentStep(recipe_state_);
        debug_snapshot_.step_id = current != nullptr ? current->id : "(none)";
        debug_snapshot_.branch_id = recipe_state_.selected_branch_by_point.empty()
                                        ? "(none)"
                                        : recipe_state_.selected_branch_by_point.begin()->second;
        debug_snapshot_.planner = planner_status;

        planner_ready_ = planner_status.success;
        if (debug_mode_enabled_) {
            debug_text_->setPlainText(DebugSnapshotText(debug_snapshot_));
        }
        RefreshStatusIndicators();
    }

    std::string DebugSnapshotText(const DebugSnapshot& snap) {
        std::ostringstream oss;
        oss << "transcript=" << snap.transcript << "\n";
        oss << "gesture=" << snap.gesture.label << " (" << snap.gesture.confidence << ")\n";
        oss << "recipe=" << snap.recipe_id << " step=" << snap.step_id << " branch=" << snap.branch_id << "\n";
        oss << "planner=" << snap.planner.source << " ok=" << (snap.planner.success ? "true" : "false")
            << " rt_ms=" << snap.planner.round_trip_ms;
        return oss.str();
    }

    void RefreshStatusIndicators() {
        const auto now = std::chrono::steady_clock::now();
        const bool speech_started_recently =
            last_speech_start_at_.has_value() &&
            now - *last_speech_start_at_ < std::chrono::milliseconds(kSpeechStartIndicatorHoldMs);
        std::string mic_label = "mic: ";
        if (!sidecar_ok_) {
            mic_label += "offline";
        } else if (!vad_available_) {
            mic_label += "listening (manual stop fallback)";
        } else if (speech_started_recently) {
            mic_label += "speech-start";
        } else {
            mic_label += mic_active_ ? "listening" : "idle";
        }
        mic_status_->setText(mic_label.c_str());
        tts_status_->setText(std::string("tts: ").append(tts_.IsSpeaking() ? "speaking" : "idle").c_str());
        gesture_status_->setText(std::string("gesture: ").append(gesture_active_ ? "active" : "idle").c_str());
        std::string planner_label = "planner: ";
        if (!sidecar_ok_) {
            planner_label += "offline fallback";
        } else if (planner_fallback_active_) {
            planner_label += "fallback active";
        } else if (planner_mode_ == "llm" && planner_llm_configured_) {
            planner_label += "llm configured";
        } else if (planner_mode_ == "llm") {
            planner_label += "llm needs key";
        } else {
            planner_label += planner_ready_ ? "mock ready" : "mock";
        }
        planner_status_->setText(planner_label.c_str());
        sidecar_status_->setText(std::string("sidecar: ").append(sidecar_ok_ ? "healthy" : "offline").c_str());
    }

    void AppendChat(const std::string& speaker, const std::string& text) {
        const QString stamp = QDateTime::currentDateTime().toString("HH:mm:ss");
        chat_history_->appendPlainText(QString("[%1] %2: %3").arg(stamp, QString::fromStdString(speaker),
                                                                   QString::fromStdString(text)));
    }

    QTimer poll_timer_;
    QLabel* camera_label_ = nullptr;
    QPlainTextEdit* chat_history_ = nullptr;
    QLineEdit* input_line_ = nullptr;
    QPushButton* planner_settings_btn_ = nullptr;
    QCheckBox* dev_toggle_ = nullptr;
    QPlainTextEdit* debug_text_ = nullptr;
    QLabel* mic_status_ = nullptr;
    QLabel* tts_status_ = nullptr;
    QLabel* gesture_status_ = nullptr;
    QLabel* planner_status_ = nullptr;
    QLabel* sidecar_status_ = nullptr;

    bool startup_failed_ = false;
    static constexpr int kSpeechStartIndicatorHoldMs = 900;
    bool sidecar_ok_ = false;
    bool debug_mode_enabled_ = false;
    bool startup_prompt_pending_ = false;
    bool mic_active_ = false;
    bool vad_available_ = true;
    bool gesture_active_ = false;
    bool planner_ready_ = false;
    bool planner_llm_configured_ = false;
    bool planner_fallback_active_ = false;
    std::string planner_mode_ = "llm";

    RecipeState recipe_state_{};
    AppConfig app_config_{};
    std::string app_config_path_;
    DebugSnapshot debug_snapshot_{};
    std::optional<std::chrono::steady_clock::time_point> last_speech_start_at_;
    TtsController tts_;
    SidecarSpeechInputAdapter speech_input_{"127.0.0.1", 8080};
    PythonSidecarManager sidecar_manager_{};
    CameraController camera_;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    if (window.StartupFailed()) {
        return 1;
    }
    window.show();
    return app.exec();
}
#else
int main() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::cout << "[MiMoCA] Startup at " << std::ctime(&now_time);
    Log("Windows-first prototype shell is running.");
    Log("Qt UI is disabled for this build; run on Windows with Qt6 to use the primary UI.");
    return 0;
}
#endif
