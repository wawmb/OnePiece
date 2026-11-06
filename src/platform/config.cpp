#include "config.h"
#include "core/app_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace app {

static std::wstring config_path() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* last_slash = wcsrchr(buf, L'\\');
    if (last_slash) *(last_slash + 1) = L'\0';
    wcscat_s(buf, L"config.ini");
    return buf;
}

static const wchar_t* SEC_PARAMS    = L"params";
static const wchar_t* SEC_HOTKEY    = L"hotkey";
static const wchar_t* SEC_DEFAULTS  = L"defaults";
static const wchar_t* SEC_CUSTOM    = L"custom";

// ini 是人可编辑的：手滑写成 99999999 或 -5 都要能兜住，
// 否则会把连点节奏变成"按住 27 小时"或者负数直接跳过等待。
static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void load_config() {
    const std::wstring path = config_path();
    const wchar_t* p = path.c_str();

    // 普通模式四参数：按住基础 / 按住随机 / 松开随机 / 循环间隔
    // 取值范围与界面输入框保持一致
    param_press_base_ms    .store(clamp_int(static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"press_base_ms",      50, p)),     1,  60000));
    param_press_jitter_ms  .store(clamp_int(static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"press_jitter_ms",    10, p)),     0,   5000));
    param_release_jitter_ms.store(clamp_int(static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"release_jitter_ms",  20, p)),     0,   5000));
    param_loop_gap_ms      .store(clamp_int(static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"loop_gap_ms",       500, p)),     0,  60000));
    param_clicks_per_rest  .store(clamp_int(static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"clicks_per_rest",     0, p)),     0, 100000));
    param_rest_seconds     .store(clamp_int(static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"rest_seconds",        3, p)),     1,   3600));

    // 运行模式：0=普通 1=自定义
    // 旧版本配置(0=普通 1=狂暴 2=自定义)一律折算到新模型
    {
        int mode = static_cast<int>(GetPrivateProfileIntW(SEC_PARAMS, L"mode", -1, p));
        if (mode < 0) {
            // 更早的版本只有 berserk_enabled 布尔开关
            mode = GetPrivateProfileIntW(SEC_PARAMS, L"berserk_enabled", 0, p) != 0 ? 1 : 0;
        }
        // 旧版的 1(狂暴) 与 2(自定义) 都归入自定义模式
        if (mode == 1 || mode == 2) mode = static_cast<int>(ClickMode::CUSTOM);
        if (mode != static_cast<int>(ClickMode::CUSTOM)) mode = static_cast<int>(ClickMode::NORMAL);
        g_click_mode.store(mode);
    }

    HotkeyConfig hk;
    hk.mouse_btn3 = GetPrivateProfileIntW(SEC_HOTKEY, L"mouse_btn3", 0, p) != 0;
    hk.mouse_btn4 = GetPrivateProfileIntW(SEC_HOTKEY, L"mouse_btn4", 1, p) != 0; // 默认 Button4
    hk.mouse_btn5 = GetPrivateProfileIntW(SEC_HOTKEY, L"mouse_btn5", 0, p) != 0;
    hk.key_ctrl   = GetPrivateProfileIntW(SEC_HOTKEY, L"key_ctrl",   0, p) != 0;
    hk.key_shift  = GetPrivateProfileIntW(SEC_HOTKEY, L"key_shift",  0, p) != 0;
    hk.key_alt    = GetPrivateProfileIntW(SEC_HOTKEY, L"key_alt",    0, p) != 0;
    {
        static const wchar_t* vkey_keys[4] = {L"vkey0", L"vkey1", L"vkey2", L"vkey3"};
        for (int i = 0; i < 4; ++i)
            hk.vkeys[i] = static_cast<uint8_t>(GetPrivateProfileIntW(SEC_HOTKEY, vkey_keys[i], 0, p));
    }

    // 模式切换热键(默认 Button5,与触发键默认的 Button4 错开)
    HotkeyConfig mk;
    mk.mouse_btn3 = GetPrivateProfileIntW(SEC_HOTKEY, L"mode_mouse_btn3", 0, p) != 0;
    mk.mouse_btn4 = GetPrivateProfileIntW(SEC_HOTKEY, L"mode_mouse_btn4", 0, p) != 0;
    mk.mouse_btn5 = GetPrivateProfileIntW(SEC_HOTKEY, L"mode_mouse_btn5", 1, p) != 0;
    mk.key_ctrl   = GetPrivateProfileIntW(SEC_HOTKEY, L"mode_key_ctrl",   0, p) != 0;
    mk.key_shift  = GetPrivateProfileIntW(SEC_HOTKEY, L"mode_key_shift",  0, p) != 0;
    mk.key_alt    = GetPrivateProfileIntW(SEC_HOTKEY, L"mode_key_alt",    0, p) != 0;
    {
        static const wchar_t* mode_vkey_keys[4] = {L"mode_vkey0", L"mode_vkey1", L"mode_vkey2", L"mode_vkey3"};
        for (int i = 0; i < 4; ++i)
            mk.vkeys[i] = static_cast<uint8_t>(GetPrivateProfileIntW(SEC_HOTKEY, mode_vkey_keys[i], 0, p));
    }
    { std::lock_guard<std::mutex> lk(g_hotkey_mutex); g_hotkey = hk; g_mode_switch_hotkey = mk; }

    // 用户自定义默认值(若 config 缺失则用 struct 内置默认)
    ParamDefaults d;
    d.press_base_ms     = static_cast<int>(GetPrivateProfileIntW(SEC_DEFAULTS, L"press_base_ms",     d.press_base_ms,     p));
    d.press_jitter_ms   = static_cast<int>(GetPrivateProfileIntW(SEC_DEFAULTS, L"press_jitter_ms",   d.press_jitter_ms,   p));
    d.release_jitter_ms = static_cast<int>(GetPrivateProfileIntW(SEC_DEFAULTS, L"release_jitter_ms", d.release_jitter_ms, p));
    d.loop_gap_ms       = static_cast<int>(GetPrivateProfileIntW(SEC_DEFAULTS, L"loop_gap_ms",       d.loop_gap_ms,       p));
    d.clicks_per_rest   = static_cast<int>(GetPrivateProfileIntW(SEC_DEFAULTS, L"clicks_per_rest",   d.clicks_per_rest,   p));
    d.rest_seconds      = static_cast<int>(GetPrivateProfileIntW(SEC_DEFAULTS, L"rest_seconds",      d.rest_seconds,      p));
    { std::lock_guard<std::mutex> lk(g_param_defaults_mutex); g_param_defaults = d; }

    // 自定义模式动作序列
    {
        CustomProfile cp;

        // 默认值给 -1 而不是 0，用来区分两种"没有动作"：
        //   -1 → ini 里根本没有 [custom] 段（首次运行 / 老配置）→ 保留出厂默认
        //    0 → 用户主动清空了动作序列 → 必须保持为空，重启不能把默认动作塞回来
        int n = static_cast<int>(GetPrivateProfileIntW(SEC_CUSTOM, L"count", -1, p));
        if (n > MAX_CUSTOM_STEPS) n = MAX_CUSTOM_STEPS;
        if (n < 0) {
            // 没有配置过：只应用循环间隔，动作保留出厂默认(左键 + R 键)
            int gap = clamp_int(static_cast<int>(
                          GetPrivateProfileIntW(SEC_CUSTOM, L"loop_gap",
                                                g_custom_profile.loop_gap_ms, p)),
                          0, 600000);
            std::lock_guard<std::mutex> lk(g_custom_mutex);
            g_custom_profile.loop_gap_ms = gap;
        } else {
            cp.count = n;
            // 一轮动作走完后的循环间隔
            cp.loop_gap_ms = clamp_int(static_cast<int>(
                                GetPrivateProfileIntW(SEC_CUSTOM, L"loop_gap", 100, p)),
                            0, 600000);

            for (int i = 0; i < n; ++i) {
                wchar_t k_vk[32], k_mods[32], k_ex[3][32], k_hold[32], k_hjit[32], k_rjit[32];
                swprintf(k_vk,   32, L"step%d_vk",              i);
                swprintf(k_mods, 32, L"step%d_mods",            i);
                swprintf(k_hold, 32, L"step%d_hold",            i);
                swprintf(k_hjit, 32, L"step%d_hold_jitter",     i);
                swprintf(k_rjit, 32, L"step%d_release_jitter",  i);
                for (int e = 0; e < 3; ++e)
                    swprintf(k_ex[e], 32, L"step%d_extra%d", i, e);

                cp.steps[i].vk                = static_cast<uint8_t>(GetPrivateProfileIntW(SEC_CUSTOM, k_vk,   0,  p));
                cp.steps[i].mods              = static_cast<uint8_t>(GetPrivateProfileIntW(SEC_CUSTOM, k_mods, 0,  p));
                cp.steps[i].hold_ms           = static_cast<int>(GetPrivateProfileIntW(SEC_CUSTOM, k_hold, 50, p));
                cp.steps[i].hold_jitter_ms    = static_cast<int>(GetPrivateProfileIntW(SEC_CUSTOM, k_hjit, 0,  p));
                cp.steps[i].release_jitter_ms = static_cast<int>(GetPrivateProfileIntW(SEC_CUSTOM, k_rjit, 0,  p));
                for (int e = 0; e < 3; ++e)
                    cp.steps[i].vkeys[e] = static_cast<uint8_t>(
                        GetPrivateProfileIntW(SEC_CUSTOM, k_ex[e], 0, p));

                // 只保留 Ctrl/Shift/Alt 三位，其余位一律清掉
                cp.steps[i].mods &= static_cast<uint8_t>(CMOD_CTRL | CMOD_SHIFT | CMOD_ALT);
                if (cp.steps[i].hold_ms           < 1)     cp.steps[i].hold_ms           = 1;
                if (cp.steps[i].hold_ms           > 60000) cp.steps[i].hold_ms           = 60000;
                if (cp.steps[i].hold_jitter_ms    < 0)     cp.steps[i].hold_jitter_ms    = 0;
                if (cp.steps[i].hold_jitter_ms    > 60000) cp.steps[i].hold_jitter_ms    = 60000;
                if (cp.steps[i].release_jitter_ms < 0)     cp.steps[i].release_jitter_ms = 0;
                if (cp.steps[i].release_jitter_ms > 60000) cp.steps[i].release_jitter_ms = 60000;
            }
            std::lock_guard<std::mutex> lk(g_custom_mutex);
            g_custom_profile = cp;
        }
    }
}

void save_config() {
    const std::wstring path = config_path();
    const wchar_t* p = path.c_str();

    auto write = [&](const wchar_t* sec, const wchar_t* key, int val) {
        wchar_t buf[32];
        _itow_s(val, buf, 10);
        WritePrivateProfileStringW(sec, key, buf, p);
    };
    // 传 nullptr 即删除该键：用于清理旧版本遗留字段
    auto erase = [&](const wchar_t* sec, const wchar_t* key) {
        WritePrivateProfileStringW(sec, key, nullptr, p);
    };

    // 旧版本(v1.3 及更早)的字段，升级到 v1.4 后不再使用
    {
        static const wchar_t* old_params[] = {
            L"loop_speed_ms", L"jitter_ms", L"berserk_enabled", L"berserk_mount_vk",
            L"berserk_lclick_ms", L"berserk_gap1_ms", L"berserk_mount_ms", L"berserk_gap2_ms",
        };
        for (const wchar_t* k : old_params) erase(SEC_PARAMS, k);

        static const wchar_t* old_defaults[] = {
            L"loop_speed_ms", L"jitter_ms", L"berserk_lclick_ms", L"berserk_gap1_ms",
            L"berserk_mount_ms", L"berserk_gap2_ms",
        };
        for (const wchar_t* k : old_defaults) erase(SEC_DEFAULTS, k);

        for (int i = 0; i < MAX_CUSTOM_STEPS; ++i) {
            wchar_t k_delay[32], k_jit[32];
            swprintf(k_delay, 32, L"step%d_delay",  i);
            swprintf(k_jit,   32, L"step%d_jitter", i);
            erase(SEC_CUSTOM, k_delay);
            erase(SEC_CUSTOM, k_jit);
        }
    }

    write(SEC_PARAMS, L"press_base_ms",     param_press_base_ms    .load());
    write(SEC_PARAMS, L"press_jitter_ms",   param_press_jitter_ms  .load());
    write(SEC_PARAMS, L"release_jitter_ms", param_release_jitter_ms.load());
    write(SEC_PARAMS, L"loop_gap_ms",       param_loop_gap_ms      .load());
    write(SEC_PARAMS, L"clicks_per_rest",   param_clicks_per_rest  .load());
    write(SEC_PARAMS, L"rest_seconds",      param_rest_seconds     .load());
    write(SEC_PARAMS, L"mode",              g_click_mode.load());

    HotkeyConfig hk, mk;
    { std::lock_guard<std::mutex> lk(g_hotkey_mutex); hk = g_hotkey; mk = g_mode_switch_hotkey; }
    write(SEC_HOTKEY, L"mouse_btn3", hk.mouse_btn3 ? 1 : 0);
    write(SEC_HOTKEY, L"mouse_btn4", hk.mouse_btn4 ? 1 : 0);
    write(SEC_HOTKEY, L"mouse_btn5", hk.mouse_btn5 ? 1 : 0);
    write(SEC_HOTKEY, L"key_ctrl",   hk.key_ctrl   ? 1 : 0);
    write(SEC_HOTKEY, L"key_shift",  hk.key_shift  ? 1 : 0);
    write(SEC_HOTKEY, L"key_alt",    hk.key_alt    ? 1 : 0);
    {
        static const wchar_t* vkey_keys[4] = {L"vkey0", L"vkey1", L"vkey2", L"vkey3"};
        for (int i = 0; i < 4; ++i)
            write(SEC_HOTKEY, vkey_keys[i], hk.vkeys[i]);
    }
    // 模式切换热键
    write(SEC_HOTKEY, L"mode_mouse_btn3", mk.mouse_btn3 ? 1 : 0);
    write(SEC_HOTKEY, L"mode_mouse_btn4", mk.mouse_btn4 ? 1 : 0);
    write(SEC_HOTKEY, L"mode_mouse_btn5", mk.mouse_btn5 ? 1 : 0);
    write(SEC_HOTKEY, L"mode_key_ctrl",   mk.key_ctrl   ? 1 : 0);
    write(SEC_HOTKEY, L"mode_key_shift",  mk.key_shift  ? 1 : 0);
    write(SEC_HOTKEY, L"mode_key_alt",    mk.key_alt    ? 1 : 0);
    {
        static const wchar_t* mode_vkey_keys[4] = {L"mode_vkey0", L"mode_vkey1", L"mode_vkey2", L"mode_vkey3"};
        for (int i = 0; i < 4; ++i)
            write(SEC_HOTKEY, mode_vkey_keys[i], mk.vkeys[i]);
    }

    // 用户自定义默认值
    ParamDefaults d;
    { std::lock_guard<std::mutex> lk(g_param_defaults_mutex); d = g_param_defaults; }
    write(SEC_DEFAULTS, L"press_base_ms",     d.press_base_ms);
    write(SEC_DEFAULTS, L"press_jitter_ms",   d.press_jitter_ms);
    write(SEC_DEFAULTS, L"release_jitter_ms", d.release_jitter_ms);
    write(SEC_DEFAULTS, L"loop_gap_ms",       d.loop_gap_ms);
    write(SEC_DEFAULTS, L"clicks_per_rest",   d.clicks_per_rest);
    write(SEC_DEFAULTS, L"rest_seconds",      d.rest_seconds);

    // 自定义模式动作序列
    CustomProfile cp;
    { std::lock_guard<std::mutex> lk(g_custom_mutex); cp = g_custom_profile; }
    write(SEC_CUSTOM, L"count",    cp.count);
    write(SEC_CUSTOM, L"loop_gap", cp.loop_gap_ms);
    for (int i = 0; i < cp.count && i < MAX_CUSTOM_STEPS; ++i) {
        wchar_t k_vk[32], k_mods[32], k_ex[3][32], k_hold[32], k_hjit[32], k_rjit[32];
        swprintf(k_vk,   32, L"step%d_vk",             i);
        swprintf(k_mods, 32, L"step%d_mods",           i);
        swprintf(k_hold, 32, L"step%d_hold",           i);
        swprintf(k_hjit, 32, L"step%d_hold_jitter",    i);
        swprintf(k_rjit, 32, L"step%d_release_jitter", i);
        for (int e = 0; e < 3; ++e)
            swprintf(k_ex[e], 32, L"step%d_extra%d", i, e);

        write(SEC_CUSTOM, k_vk,   cp.steps[i].vk);
        write(SEC_CUSTOM, k_mods, cp.steps[i].mods);
        write(SEC_CUSTOM, k_hold, cp.steps[i].hold_ms);
        write(SEC_CUSTOM, k_hjit, cp.steps[i].hold_jitter_ms);
        write(SEC_CUSTOM, k_rjit, cp.steps[i].release_jitter_ms);
        for (int e = 0; e < 3; ++e)
            write(SEC_CUSTOM, k_ex[e], cp.steps[i].vkeys[e]);
    }
}

} // namespace app
