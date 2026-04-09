#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <vector>

#ifdef _WIN32
#include <sapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef MIMOCA_HAS_OPENCV
#include <opencv2/core.hpp>
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
};

struct Recipe {
    std::string id;
    std::string name;
    std::vector<RecipeStep> steps;
    std::unordered_map<std::string, size_t> step_index_by_id;
};

struct RecipeState {
    Recipe recipe;
    size_t current_step_index;
};

struct TranscriptEvent {
    std::string text;
    bool is_final;
};

struct GestureCommand {
    bool parsed = false;
    Gesture gesture{"none", 0.0};
};

void Log(const std::string& message);

#ifdef _WIN32
class TtsController {
   public:
    TtsController() = default;

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
            Log("TTS start.");
            const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
                Log("TTS unavailable: COM initialization failed.");
                return;
            }

            ISpVoice* voice = nullptr;
            const HRESULT create_hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                                       reinterpret_cast<void**>(&voice));
            if (FAILED(create_hr) || voice == nullptr) {
                Log("TTS unavailable: cannot create SAPI voice.");
                if (SUCCEEDED(init_hr)) {
                    CoUninitialize();
                }
                return;
            }

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
            if (SUCCEEDED(init_hr)) {
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
    std::thread speech_thread_;
    std::atomic<bool> stop_requested_{false};
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

bool ExtractJsonFieldBool(const std::string& json, const std::string& field, bool default_value = false) {
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

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool ContainsText(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
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
        if (!step.id.empty() && !step.instruction.empty()) {
            out_recipe.steps.push_back(step);
        }

        cursor = step_end + 1;
    }

    out_recipe.step_index_by_id.clear();
    for (size_t i = 0; i < out_recipe.steps.size(); ++i) {
        out_recipe.step_index_by_id[out_recipe.steps[i].id] = i;
    }

    if (out_recipe.id.empty() || out_recipe.steps.empty()) {
        Log("Recipe parse produced empty data.");
        return false;
    }

    Log("Loaded recipe '" + out_recipe.name + "' with " + std::to_string(out_recipe.steps.size()) + " steps.");
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
    if (current == nullptr || current->next_step_id.empty()) {
        return nullptr;
    }

    const auto it = state.recipe.step_index_by_id.find(current->next_step_id);
    if (it == state.recipe.step_index_by_id.end()) {
        return nullptr;
    }

    return &state.recipe.steps[it->second];
}

bool AdvanceCurrentStep(RecipeState& state) {
    const RecipeStep* current = GetCurrentStep(state);
    if (current == nullptr || current->next_step_id.empty()) {
        return false;
    }

    const auto it = state.recipe.step_index_by_id.find(current->next_step_id);
    if (it == state.recipe.step_index_by_id.end()) {
        return false;
    }

    state.current_step_index = it->second;
    return true;
}

TurnContext BuildTurnContext(const RecipeState& state,
                             const std::string& utterance,
                             const Gesture& gesture,
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
        "",
        utterance,
        current != nullptr ? current->instruction : "",
        next != nullptr ? next->instruction : "",
        gesture,
        std::vector<Detection>{Detection{"cutting_board", 0.88, {0.2, 0.2, 0.7, 0.8}}},
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

    if (ContainsText(normalized, "option_a")) {
        response.assistant_text = "Selected option A branch.";
        response.new_branch_id = "option_a";
        return response;
    }

    if (ContainsText(normalized, "option_b")) {
        response.assistant_text = "Selected option B branch.";
        response.new_branch_id = "option_b";
        return response;
    }

    response.assistant_text = "Say 'current' or 'next'.";
    return response;
}

bool IsSupportedGestureLabel(const std::string& label) {
    return label == "next" || label == "repeat" || label == "option_a" || label == "option_b" || label == "none";
}

GestureCommand ParseGestureCommand(const std::string& line) {
    const std::string prefix = "gesture ";
    if (line.rfind(prefix, 0) != 0) {
        return GestureCommand{};
    }

    std::istringstream input(line.substr(prefix.size()));
    std::string label;
    input >> label;
    label = ToLower(label);
    if (!IsSupportedGestureLabel(label)) {
        Log("Unsupported gesture label '" + label + "'. Use: next | repeat | option_a | option_b | none");
        return GestureCommand{};
    }

    double confidence = 0.95;
    if (input >> confidence) {
        if (confidence < 0.0) {
            confidence = 0.0;
        }
        if (confidence > 1.0) {
            confidence = 1.0;
        }
    }

    GestureCommand command{};
    command.parsed = true;
    command.gesture = Gesture{label, confidence};
    return command;
}

class SpeechInputAdapter {
   public:
    virtual ~SpeechInputAdapter() = default;
    virtual bool TryConsumeConsoleLine(const std::string& line, TranscriptEvent& out_event) = 0;
    virtual std::string Name() const = 0;
};

class MockTranscriptInputAdapter final : public SpeechInputAdapter {
   public:
    bool TryConsumeConsoleLine(const std::string& line, TranscriptEvent& out_event) override {
        const std::string partial_prefix = "say-partial ";
        const std::string final_prefix = "say-final ";

        if (line.rfind(partial_prefix, 0) == 0) {
            out_event = TranscriptEvent{Trim(line.substr(partial_prefix.size())), false};
            return true;
        }

        if (line.rfind(final_prefix, 0) == 0) {
            out_event = TranscriptEvent{Trim(line.substr(final_prefix.size())), true};
            return true;
        }

        return false;
    }

    std::string Name() const override {
        return "mock-transcript-stdin";
    }
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
    MockTranscriptInputAdapter speech_input;
    CameraController camera;
    camera.Start(0);
    Log("Speech input adapter: " + speech_input.Name());

    Log("Type 'current', 'next', 'camera', 'gesture <label> [confidence]', 'stop',");
    Log("'say-partial <text>', 'say-final <text>', or 'exit'.");
    std::string line;
    while (std::getline(std::cin, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        if (line == "exit") {
            break;
        }

        if (line == "stop") {
            tts.Stop();
            continue;
        }

        if (line == "camera") {
            const CameraSnapshot snapshot = camera.GetLatestSnapshot();
            std::cout << "[MiMoCA] Camera status: " << snapshot.status_message << '\n';
            std::cout << "  started: " << (snapshot.camera_started ? "true" : "false") << '\n';
            std::cout << "  frame_available: " << (snapshot.frame_available ? "true" : "false") << '\n';
            if (snapshot.frame_available) {
                std::cout << "  latest_frame: " << snapshot.width << "x" << snapshot.height << '\n';
                std::cout << "  captured_at: " << snapshot.capture_timestamp << '\n';
                std::cout << "  frame_count: " << snapshot.frame_count << '\n';
            }
            continue;
        }

        std::string utterance;
        std::string command = line;
        Gesture turn_gesture{"none", 0.0};
        TranscriptEvent transcript_event{};
        if (speech_input.TryConsumeConsoleLine(line, transcript_event)) {
            if (transcript_event.text.empty()) {
                Log("Speech event ignored: empty transcript.");
                continue;
            }

            if (transcript_event.is_final) {
                utterance = transcript_event.text;
                command = utterance;
                Log("Speech final transcript: '" + utterance + "'");
            } else {
                Log("Speech partial transcript: '" + transcript_event.text + "'");
                continue;
            }
        } else if (line == "current") {
            utterance = "what is the current step?";
        } else if (line == "next") {
            utterance = "what next?";
        } else if (const GestureCommand gesture_command = ParseGestureCommand(line); gesture_command.parsed) {
            turn_gesture = gesture_command.gesture;
            command = "gesture_" + turn_gesture.label;
            Log("Mock gesture injected: label='" + turn_gesture.label + "', confidence=" +
                std::to_string(turn_gesture.confidence));
        } else {
            Log("Unknown command. Use: current | next | gesture <label> [confidence] | stop | "
                "say-partial <text> | say-final <text> | exit");
            continue;
        }

        PlannerResponse response{};
        const CameraSnapshot camera_snapshot = camera.GetLatestSnapshot();
        if (sidecar_ok) {
            if (!RequestMockPlanner("127.0.0.1", 8080, BuildTurnContext(recipe_state, utterance, turn_gesture, camera_snapshot),
                                    response)) {
                Log("Planner call failed; using local recipe fallback for this turn.");
                response = LocalRecipeFallback(recipe_state, command);
            }
        } else {
            response = LocalRecipeFallback(recipe_state, command);
        }

        PrintPlannerResponse(response);
        if (response.speak && !response.assistant_text.empty()) {
            tts.Speak(response.assistant_text);
        }
        if (response.advance_step) {
            if (AdvanceCurrentStep(recipe_state)) {
                const RecipeStep* after = GetCurrentStep(recipe_state);
                if (after != nullptr) {
                    Log("Advanced to step: " + after->id);
                }
            } else {
                Log("No next step to advance to.");
            }
        }
    }

    camera.Stop();
    tts.Stop();
    Log("TTS gap: microphone-driven interruption is not wired yet; use 'stop' to cancel speech manually.");
    Log("Exiting MiMoCA prototype.");
    return 0;
}
