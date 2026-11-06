#include "clicker.h"
#include "app_state.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace app {

// ---- 自定义动作的按键分类 ----
// vk 里有一小段属于「鼠标按键」，走鼠标通道注入；其余走键盘通道。
// 0 是历史哨兵值，等价于 VK_LBUTTON（左键），保留以兼容旧配置。
static bool is_mouse_vk(uint8_t vk) {
    return vk == 0 || vk == VK_LBUTTON || vk == VK_RBUTTON ||
           vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

// 鼠标键码 + 按下/松开 → Interception 的 stroke.state
static unsigned short mouse_state_for(uint8_t vk, bool down) {
    switch (vk) {
        case 0:
        case VK_LBUTTON:
            return down ? INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN
                        : INTERCEPTION_MOUSE_LEFT_BUTTON_UP;
        case VK_RBUTTON:
            return down ? INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN
                        : INTERCEPTION_MOUSE_RIGHT_BUTTON_UP;
        case VK_MBUTTON:
            return down ? INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN
                        : INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP;
        case VK_XBUTTON1:
            return down ? INTERCEPTION_MOUSE_BUTTON_4_DOWN
                        : INTERCEPTION_MOUSE_BUTTON_4_UP;
        case VK_XBUTTON2:
            return down ? INTERCEPTION_MOUSE_BUTTON_5_DOWN
                        : INTERCEPTION_MOUSE_BUTTON_5_UP;
        default:
            return 0;
    }
}

[[noreturn]] void macro_worker() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    // 每次点击重新读取参数,改参数后立即生效
    auto sample_press_ms = [&]() {
        const int base = param_press_base_ms.load();
        const int jitter_max = param_press_jitter_ms.load();
        if (jitter_max <= 0) return base;
        std::uniform_int_distribution<int> d(0, jitter_max);
        return base + d(gen);
    };

    int clicks_this_round = 0;
    bool was_running = false;
    // 已请求运行但还没识别到鼠标设备时的提示去重（否则 50ms 刷一条）
    bool mouse_warned = false;

    auto now_ms = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    // 中断感知 sleep:在 is_macro_running 变 false 时尽快返回
    auto interruptible_sleep = [](int total_ms) {
        const int step = 10;
        for (int t = 0; t < total_ms && is_macro_running.load(); t += step) {
            // 最后一段可能不足 step。这里刻意不用 std::min：windows.h 的 min 宏
            // 在 MSVC 下会把它展开成非法语法（error C2589），改用手算更省事也更稳。
            const int chunk = (t + step <= total_ms) ? step : (total_ms - t);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        }
    };

    // 通过 SendInput 发送虚拟键码(Interception 只过滤鼠标,不影响键盘)
    // 优先使用 Interception 驱动级注入，无法注入时降级到 SendInput
    auto send_vkey = [](uint8_t vk, bool down) {
        const InterceptionDevice kbd = active_keyboard_id.load();
        if (kbd != 0) {
            // 驱动级键盘注入：用 scan code
            UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
            WORD scan_low = static_cast<WORD>(scan & 0xFF);
            bool e0 = (scan >> 8) == 0xE0;
            InterceptionKeyStroke ks{};
            ks.code  = scan_low;
            ks.state = static_cast<unsigned short>(
                (down ? INTERCEPTION_KEY_DOWN : INTERCEPTION_KEY_UP) |
                (e0   ? INTERCEPTION_KEY_E0   : 0));
            interception_send(g_context, kbd, (InterceptionStroke*)&ks, 1);
            return;
        }
        // 降级：SendInput
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        WORD scan_low = static_cast<WORD>(scan & 0xFF);
        bool extended = (scan >> 8) == 0xE0;
        if (scan_low == 0) {
            in.ki.wVk     = vk;
            in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        } else {
            in.ki.wScan   = scan_low;
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
            if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        SendInput(1, &in, sizeof(in));
    };

    // 按下/松开一个键：鼠标键走驱动鼠标通道，其余走键盘通道
    auto key_down = [&send_vkey](InterceptionDevice mouse, uint8_t v) {
        if (is_mouse_vk(v)) {
            InterceptionMouseStroke s{};
            s.state = mouse_state_for(v, true);
            interception_send(g_context, mouse, (InterceptionStroke*)&s, 1);
        } else {
            send_vkey(v, true);
        }
    };
    auto key_up = [&send_vkey](InterceptionDevice mouse, uint8_t v) {
        if (is_mouse_vk(v)) {
            InterceptionMouseStroke s{};
            s.state = mouse_state_for(v, false);
            interception_send(g_context, mouse, (InterceptionStroke*)&s, 1);
        } else {
            send_vkey(v, false);
        }
    };

    while (true) {
        const bool running = is_macro_running.load();
        const InterceptionDevice mouse = active_mouse_id.load();

        if (running && mouse != 0) {
            mouse_warned = false;
            const int cmod = g_click_mode.load();

            if (!was_running) {
                add_log(cmod == (int)ClickMode::CUSTOM ? u8"自定义循环已开启"
                                                       : u8"连点循环已开启",
                        LogLevel::INFO);
                was_running = true;
                clicks_this_round = 0;
                run_start_ms_epoch.store(now_ms());
            }

            // ============ 自定义模式（用户编排的动作序列）============
            if (cmod == (int)ClickMode::CUSTOM) {
                // 每轮取一次快照，避免执行期间被 GUI 改动读到半截数据
                CustomProfile prof;
                { std::lock_guard<std::mutex> lk(g_custom_mutex); prof = g_custom_profile; }

                if (prof.count <= 0) {
                    // 还没配置动作：空转等待，别忙循环
                    interruptible_sleep(100);
                    continue;
                }

                for (int i = 0; i < prof.count; ++i) {
                    if (!is_macro_running.load()) break;
                    const CustomStep& st = prof.steps[i];

                    // 按住时长 = 基础 + 0~按住随机
                    int hold = st.hold_ms;
                    if (st.hold_jitter_ms > 0) {
                        std::uniform_int_distribution<int> d(0, st.hold_jitter_ms);
                        hold += d(gen);
                    }

                    // ---- 1) 先按下修饰键（组合键用）----
                    // 用左修饰键的键码注入，避免 VK_SHIFT 这类通用码在扫描码映射上产生歧义
                    static const struct { uint8_t mask; uint8_t vk; } kModKeys[] = {
                        { CMOD_CTRL,  VK_LCONTROL },
                        { CMOD_SHIFT, VK_LSHIFT   },
                        { CMOD_ALT,   VK_LMENU    },
                    };
                    constexpr int kModCount = 3;
                    if (st.mods) {
                        for (const auto& m : kModKeys)
                            if (st.mods & m.mask) key_down(mouse, m.vk);
                    }

                    // ---- 2) 主键 + 额外键依次按下（S+D 这类多键同时按住）----
                    key_down(mouse, st.vk);
                    for (uint8_t v : st.vkeys)
                        if (v != 0) key_down(mouse, v);

                    // ---- 3) 保持 ----
                    interruptible_sleep(hold);

                    // ---- 4) 逆序松开 ----
                    // 这一段必须无条件执行：即使中途被停止，也不能让任何键卡在按下状态
                    for (int k = 2; k >= 0; --k)
                        if (st.vkeys[k] != 0) key_up(mouse, st.vkeys[k]);
                    key_up(mouse, st.vk);

                    if (st.mods) {
                        for (int k = kModCount - 1; k >= 0; --k)
                            if (st.mods & kModKeys[k].mask) key_up(mouse, kModKeys[k].vk);
                    }

                    total_click_count.fetch_add(1);
                    clicks_this_round++;

                    // 松开后的随机延迟
                    if (st.release_jitter_ms > 0) {
                        std::uniform_int_distribution<int> d(0, st.release_jitter_ms);
                        interruptible_sleep(d(gen));
                    }
                }

                // 一轮动作走完 → 循环间隔
                if (prof.loop_gap_ms > 0) interruptible_sleep(prof.loop_gap_ms);

                // 休息策略
                const int per_rest = param_clicks_per_rest.load();
                const int rest_sec = param_rest_seconds.load();
                if (per_rest > 0 && clicks_this_round >= per_rest) {
                    add_log("已完成 " + std::to_string(per_rest) + " 次动作,休息 " +
                            std::to_string(rest_sec) + " 秒", LogLevel::INFO);
                    interruptible_sleep(rest_sec * 1000);
                    clicks_this_round = 0;
                }
                continue;
            }

            // ============ 普通模式（固定鼠标左键）============
            // 按下 → 保持(基础 + 0~按下随机) → 松开
            {
                InterceptionMouseStroke click_down = {};
                click_down.state = INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN;
                interception_send(g_context, mouse, (InterceptionStroke*)&click_down, 1);

                interruptible_sleep(sample_press_ms());

                InterceptionMouseStroke click_up = {};
                click_up.state = INTERCEPTION_MOUSE_LEFT_BUTTON_UP;
                interception_send(g_context, mouse, (InterceptionStroke*)&click_up, 1);

                // 按下期间被停止（含切换模式）：本轮不计入，直接回到循环顶部
                if (!is_macro_running.load()) continue;

                total_click_count.fetch_add(1);
                clicks_this_round++;
            }

            // 休息策略 / 否则 松开随机延迟 + 循环间隔
            {
                const int per_rest = param_clicks_per_rest.load();
                const int rest_sec = param_rest_seconds.load();
                if (per_rest > 0 && clicks_this_round >= per_rest) {
                    add_log("已完成 " + std::to_string(per_rest) + " 次点击,休息 " +
                            std::to_string(rest_sec) + " 秒", LogLevel::INFO);
                    interruptible_sleep(rest_sec * 1000);
                    clicks_this_round = 0;
                } else {
                    int rel = 0;
                    const int rel_max = param_release_jitter_ms.load();
                    if (rel_max > 0) {
                        std::uniform_int_distribution<int> d(0, rel_max);
                        rel = d(gen);
                    }
                    interruptible_sleep(rel + param_loop_gap_ms.load());
                }
            }
        } else {
            if (was_running) {
                // 把这一段运行时长合入累计值,然后清空 start
                const long long start = run_start_ms_epoch.load();
                if (start > 0) {
                    total_run_ms.fetch_add(static_cast<unsigned long long>(now_ms() - start));
                }
                run_start_ms_epoch.store(0);
                add_log("连点循环已停止", LogLevel::INFO);
                was_running = false;
            } else if (running && mouse == 0 && !mouse_warned) {
                // 用热键启动时不会走界面的 "兜底 MOUSE(0)" 分支，
                // 此时设备还没识别就会一直空转。明确说一句，别让人以为程序卡死了。
                add_log(u8"尚未识别到鼠标设备，请先在窗口外移动一下鼠标", LogLevel::WARN);
                mouse_warned = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void interception_thread() {
    InterceptionDevice device;
    InterceptionStroke stroke;

    // 同时过滤键盘(只记录设备 ID,所有键盘事件原样转发)
    interception_set_filter(g_context, interception_is_keyboard,
                            INTERCEPTION_FILTER_KEY_DOWN | INTERCEPTION_FILTER_KEY_UP);

    while (true) {
        device = interception_wait(g_context);
        if (interception_receive(g_context, device, &stroke, 1) <= 0) {
            // 设备被拔出 / 驱动偶发返回异常：退避后继续，绝不退出本线程。
            // 一旦这里 return，之后所有鼠标键盘事件都没人转发，输入会整个卡死。
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ---- 键盘事件:记录设备 ID 后直接转发 ----
        if (interception_is_keyboard(device)) {
            if (active_keyboard_id.load() == 0)
                active_keyboard_id.store(device);
            interception_send(g_context, device, &stroke, 1);
            continue;
        }

        // ---- 鼠标事件 ----
        if (interception_is_mouse(device)) {
            auto& mouse_stroke = reinterpret_cast<InterceptionMouseStroke&>(stroke);
            active_mouse_id.store(device);

            // 触发键与模式切换键的通用判定工具
            auto mouse_hit = [&mouse_stroke](const HotkeyConfig& h) {
                if (h.mouse_btn3 && (mouse_stroke.state & INTERCEPTION_MOUSE_BUTTON_3_DOWN)) return true;
                if (h.mouse_btn4 && (mouse_stroke.state & INTERCEPTION_MOUSE_BUTTON_4_DOWN)) return true;
                if (h.mouse_btn5 && (mouse_stroke.state & INTERCEPTION_MOUSE_BUTTON_5_DOWN)) return true;
                return false;
            };
            // 组合键的键盘部分必须同时按住
            auto kb_ok = [](const HotkeyConfig& h) {
                if (h.key_ctrl  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
                if (h.key_shift && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
                if (h.key_alt   && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
                for (uint8_t vk : h.vkeys)
                    if (vk != 0 && !(GetAsyncKeyState(vk) & 0x8000)) return false;
                return true;
            };

            const bool any_recording = g_hotkey_recording.load() ||
                                       g_mode_switch_recording.load() ||
                                       g_action_key_recording.load();
            const bool side_button_down =
                mouse_stroke.state & (INTERCEPTION_MOUSE_BUTTON_3_DOWN |
                                      INTERCEPTION_MOUSE_BUTTON_4_DOWN |
                                      INTERCEPTION_MOUSE_BUTTON_5_DOWN);

            if (!any_recording && side_button_down) {
                HotkeyConfig hk, mk;
                { std::lock_guard<std::mutex> lk(g_hotkey_mutex); hk = g_hotkey; mk = g_mode_switch_hotkey; }

                // 1) 触发键: 切换 开始/停止
                if (mouse_hit(hk) && kb_ok(hk)) {
                    bool now = !is_macro_running.load();
                    is_macro_running.store(now);
                    add_log(std::string(u8"[热键] 连点 → ") +
                            (now ? u8"开启" : u8"关闭"), LogLevel::BEHAVIOR);
                    continue; // 吞掉此次按下
                }

                // 2) 模式切换键: 普通 ↔ 自定义（运行中会先停止）
                if (mouse_hit(mk) && kb_ok(mk)) {
                    cycle_click_mode();
                    continue; // 吞掉此次按下
                }
            }
        }
        interception_send(g_context, device, &stroke, 1);
    }
}

[[noreturn]] void hotkey_poller() {
    // 轮询处理纯键盘（或无鼠标侧键）组合：
    // - 触发键（不含鼠标侧键时生效）
    // - 模式切换键（不含鼠标侧键时生效）
    // 含鼠标侧键的组合由 interception_thread 负责。
    bool trig_was_active = false;
    bool mode_was_active = false;

    // 组合键的键盘部分是否全部按住
    auto all_kb_down = [](const HotkeyConfig& h) {
        if (h.key_ctrl  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
        if (h.key_shift && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
        if (h.key_alt   && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
        for (uint8_t vk : h.vkeys)
            if (vk != 0 && !(GetAsyncKeyState(vk) & 0x8000)) return false;
        return true;
    };
    auto has_mouse_side = [](const HotkeyConfig& h) {
        return h.mouse_btn3 || h.mouse_btn4 || h.mouse_btn5;
    };
    auto has_kb_part = [](const HotkeyConfig& h) {
        bool has_vkey = false;
        for (uint8_t vk : h.vkeys) if (vk) { has_vkey = true; break; }
        return h.key_ctrl || h.key_shift || h.key_alt || has_vkey;
    };

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        if (g_hotkey_recording.load() || g_mode_switch_recording.load() ||
            g_action_key_recording.load()) {
            trig_was_active = false;
            mode_was_active = false;
            continue;
        }

        HotkeyConfig hk, mk;
        { std::lock_guard<std::mutex> lk(g_hotkey_mutex); hk = g_hotkey; mk = g_mode_switch_hotkey; }

        // ---- 触发键(纯键盘组合) ----
        bool trig_all_down = false;
        if (!has_mouse_side(hk) && has_kb_part(hk)) {
            trig_all_down = all_kb_down(hk);
            // 上升沿触发
            if (trig_all_down && !trig_was_active) {
                bool now = !is_macro_running.load();
                is_macro_running.store(now);
                add_log(std::string(u8"[热键] 连点 → ") +
                        (now ? u8"开启" : u8"关闭"), LogLevel::BEHAVIOR);
            }
        }
        trig_was_active = trig_all_down;

        // ---- 模式切换键(纯键盘组合) ----
        // 触发键组合按住的帧跳过模式键判断，避免两个组合重叠时同帧双触发
        if (!trig_all_down && !has_mouse_side(mk) && has_kb_part(mk)) {
            bool mode_all_down = all_kb_down(mk);
            if (mode_all_down && !mode_was_active) {
                cycle_click_mode();
            }
            mode_was_active = mode_all_down;
        } else {
            mode_was_active = false;
        }
    }
}

} // namespace app
