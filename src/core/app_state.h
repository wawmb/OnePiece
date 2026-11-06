#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "interception.h"

namespace app {

// 共享运行时状态
extern std::atomic<bool>               is_macro_running;
extern std::atomic<InterceptionDevice> active_mouse_id;
extern std::atomic<InterceptionDevice> active_keyboard_id; // 键盘设备，供驱动级按键注入使用
extern InterceptionContext             g_context;

// 普通模式的四个参数
// 时序：按下 → 保持 (基础 + 0~按下随机) → 松开 → 等 (0~松开随机) + 循环间隔
extern std::atomic<int> param_press_base_ms;     // 按下后持续保持的基础时长
extern std::atomic<int> param_press_jitter_ms;   // 按住时长上额外加 0~该值
extern std::atomic<int> param_release_jitter_ms; // 松开后额外加 0~该值
extern std::atomic<int> param_loop_gap_ms;       // 循环间隔(每次点击之间的固定间隔)
extern std::atomic<int> param_clicks_per_rest;
extern std::atomic<int> param_rest_seconds;

// 运行模式
enum class ClickMode : int {
    NORMAL = 0,   // 普通：固定鼠标左键连点
    CUSTOM = 1,   // 自定义：用户编排的按键序列
};
extern std::atomic<int> g_click_mode;   // 存放 ClickMode 的整数值

inline bool is_custom_mode() { return g_click_mode.load() == (int)ClickMode::CUSTOM; }

// ---- 自定义模式 ----
// 一个动作 = 按下修饰键 + 同时按下若干普通键 → 保持 (基础 + 0~按住随机)
//            → 逆序松开 → 等 (0~松开随机)
// 多个动作顺序执行；走完一轮后等待 profile 的循环间隔，再从头开始。
constexpr int MAX_CUSTOM_STEPS = 16;

// 修饰键位掩码（可与任意主键组合，如 Ctrl+Shift+A）
enum : uint8_t {
    CMOD_CTRL  = 1u << 0,
    CMOD_SHIFT = 1u << 1,
    CMOD_ALT   = 1u << 2,
};

struct CustomStep {
    uint8_t vk               = 0;    // 主键；0 表示鼠标左键；1/2/4/5/6 为鼠标各键
    uint8_t mods             = 0;    // CMOD_* 掩码，0 = 无修饰键
    // 与主键同时按住的额外普通键（最多 3 个，0 = 不启用）。
    // 用于 S+D 这类双键组合；Ctrl/Shift/Alt 走 mods，这里只放普通键。
    uint8_t vkeys[3]         = {};
    int     hold_ms          = 50;   // 按下后持续保持的基础时长
    int     hold_jitter_ms   = 0;    // 在按住时长上额外加 0~该值
    int     release_jitter_ms= 0;    // 松开后额外加 0~该值
};

struct CustomProfile {
    int        count       = 0;
    int        loop_gap_ms = 100;   // 一轮动作走完后的间隔
    CustomStep steps[MAX_CUSTOM_STEPS] = {};
};

extern CustomProfile g_custom_profile;
extern std::mutex    g_custom_mutex;

// 普通模式的默认参数（界面「默认」按钮读取此处，也可手工改 config.ini 的 [defaults] 定制）
struct ParamDefaults {
    int press_base_ms      = 50;
    int press_jitter_ms    = 10;
    int release_jitter_ms  = 20;
    int loop_gap_ms        = 500;
    int clicks_per_rest    = 0;
    int rest_seconds       = 3;
};
extern ParamDefaults g_param_defaults;
extern std::mutex    g_param_defaults_mutex;

// 统计
extern std::atomic<unsigned long long> total_click_count;
extern std::atomic<unsigned long long> total_run_ms;
extern std::atomic<long long>          run_start_ms_epoch;

// 热键配置（由 g_hotkey_mutex 保护）
// 注意：这里所有成员一律默认 false。默认键位（触发键 Button5 / 模式切换键 Button4）
// 在 app_state.cpp 的 g_hotkey 初始化、config.cpp 的读取默认值、gui.cpp 的
// hotkey_default() 三处分别显式指定。
// 不要把 true 写进这里的默认值 —— 否则 hotkey_default() 里的 `HotkeyConfig h{}`
// 会带上这个 true，再叠加目标键就变成「Button4 + Button5」这种双键组合。
struct HotkeyConfig {
    bool    mouse_btn3 = false;
    bool    mouse_btn4 = false;
    bool    mouse_btn5 = false;
    bool    key_ctrl   = false;
    bool    key_shift  = false;
    bool    key_alt    = false;
    uint8_t vkeys[4]   = {};     // 最多 4 个额外虚拟键码，0 = 不启用
};

extern HotkeyConfig             g_hotkey;
extern std::mutex               g_hotkey_mutex;
extern std::atomic<bool>        g_hotkey_recording; // 录制模式中不触发切换

// 模式切换热键：按下即在「普通 ↔ 自定义」之间切换（默认 Button4）。
// 与 g_hotkey 共用 g_hotkey_mutex 保护。
extern HotkeyConfig             g_mode_switch_hotkey;
extern std::atomic<bool>        g_mode_switch_recording; // 录制模式中不响应任何热键
// 自定义动作按键录制中：同样需要屏蔽热键，避免录键时误触发开始/停止
extern std::atomic<bool>        g_action_key_recording;

// 日志
enum class LogLevel { INFO, BEHAVIOR, WARN, ERR };

struct LogEntry {
    std::string text;
    LogLevel    level;
};

extern std::mutex            g_log_mutex;
extern std::vector<LogEntry> g_log_lines;

void add_log(const std::string& msg, LogLevel level = LogLevel::INFO);

// 切换运行模式（普通 / 自定义）。
// 若当前正在运行，会先停止连点，避免循环中途参数体系突变产生异常节奏。
// 已是目标模式时不做任何事。
void set_click_mode(int mode);

// 依次循环切换：普通 → 自定义 → 普通
void cycle_click_mode();

} // namespace app
