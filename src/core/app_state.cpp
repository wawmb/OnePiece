#include "app_state.h"

#include <chrono>
#include <ctime>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace app {

std::atomic<bool>               is_macro_running{false};
std::atomic<InterceptionDevice> active_mouse_id{0};
std::atomic<InterceptionDevice> active_keyboard_id{0};
InterceptionContext             g_context = nullptr;

std::atomic<int> param_press_base_ms{50};
std::atomic<int> param_press_jitter_ms{10};
std::atomic<int> param_release_jitter_ms{20};
std::atomic<int> param_loop_gap_ms{500};
std::atomic<int> param_clicks_per_rest{0};
std::atomic<int> param_rest_seconds{3};

std::atomic<int>     g_click_mode{static_cast<int>(ClickMode::NORMAL)};

// 自定义模式的出厂默认：左键 → R 键，各带按住/松开随机
CustomProfile g_custom_profile = []() {
    CustomProfile p{};
    p.loop_gap_ms = 100;

    p.steps[0].vk                = 0;        // 鼠标左键
    p.steps[0].hold_ms           = 110;
    p.steps[0].hold_jitter_ms    = 10;
    p.steps[0].release_jitter_ms = 20;

    p.steps[1].vk                = 'R';
    p.steps[1].hold_ms           = 30;
    p.steps[1].hold_jitter_ms    = 5;
    p.steps[1].release_jitter_ms = 20;

    p.count = 2;
    return p;
}();
std::mutex    g_custom_mutex;

ParamDefaults g_param_defaults{};
std::mutex    g_param_defaults_mutex;

std::atomic<unsigned long long> total_click_count{0};
std::atomic<unsigned long long> total_run_ms{0};
std::atomic<long long>          run_start_ms_epoch{0};

HotkeyConfig             g_hotkey = []() {
    // 触发键（启动/停止）出厂默认：Button4（鼠标侧键）
    HotkeyConfig h{};
    h.mouse_btn4 = true;
    return h;
}();
HotkeyConfig             g_mode_switch_hotkey = []() {
    // 模式切换键出厂默认：Button5（与触发键的 Button4 错开）
    HotkeyConfig h{};
    h.mouse_btn5 = true;
    return h;
}();
std::mutex               g_hotkey_mutex;
std::atomic<bool>        g_hotkey_recording{false};
std::atomic<bool>        g_mode_switch_recording{false};
std::atomic<bool>        g_action_key_recording{false};

std::mutex               g_log_mutex;
std::vector<LogEntry>    g_log_lines;

void add_log(const std::string& msg, LogLevel level) {
    // 时间戳精确到毫秒: [HH:MM:SS.mmm]
    auto now_tp = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now_tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now_tp.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_s(&tm_buf, &now_t);
    char ts[24];
    snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03lld] ",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<long long>(ms.count()));

    std::lock_guard<std::mutex> g(g_log_mutex);
    g_log_lines.push_back({std::string(ts) + msg, level});
    if (g_log_lines.size() > 1000) {
        g_log_lines.erase(g_log_lines.begin(), g_log_lines.begin() + 200);
    }
}

void set_click_mode(int mode) {
    if (g_click_mode.load() == mode) return;

    // 运行中切模式：先停，避免循环用旧节奏继续跑再突然换成另一套参数
    const bool was_running = is_macro_running.load();
    if (was_running) is_macro_running.store(false);

    g_click_mode.store(mode);

    const char* name = (mode == static_cast<int>(ClickMode::CUSTOM)) ? u8"自定义模式"
                                                                    : u8"普通模式";
    std::string msg = std::string(u8"[模式] 已切换 → ") + name;
    if (was_running) msg += u8"（运行中，已停止）";
    add_log(msg, LogLevel::BEHAVIOR);
}

void cycle_click_mode() {
    // 只有两种模式：取反即可
    set_click_mode(g_click_mode.load() == static_cast<int>(ClickMode::CUSTOM)
                   ? static_cast<int>(ClickMode::NORMAL)
                   : static_cast<int>(ClickMode::CUSTOM));
}

} // namespace app
