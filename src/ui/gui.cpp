#include "gui.h"
#include "core/app_state.h"
#include "platform/config.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "imgui.h"

namespace app {

// ---- 调色板（浅色 / 类 Stripe 风格）----
static const ImU32 C_TEXT      = IM_COL32( 23,  29,  41, 255);
static const ImU32 C_TEXT_SUB  = IM_COL32(126, 137, 154, 255);
static const ImU32 C_TEXT_FAINT= IM_COL32(163, 173, 188, 255);
static const ImU32 C_BORDER    = IM_COL32(228, 234, 242, 255);
static const ImU32 C_CARD      = IM_COL32(255, 255, 255, 255);
static const ImU32 C_TRACK     = IM_COL32(243, 246, 250, 255);
static const ImU32 C_TRACK_D   = IM_COL32(233, 238, 246, 255);
static const ImU32 C_WHITE     = IM_COL32(255, 255, 255, 255);
static const ImU32 C_BLUE      = IM_COL32( 59, 110, 200, 255);
static const ImU32 C_BLUE_D    = IM_COL32( 41,  84, 168, 255);
static const ImU32 C_BLUE_L    = IM_COL32(238, 244, 254, 255);
static const ImU32 C_GREEN     = IM_COL32( 16, 138, 108, 255);
static const ImU32 C_GREEN_D   = IM_COL32( 11, 106,  82, 255);
static const ImU32 C_GREEN_L   = IM_COL32(236, 250, 245, 255);
static const ImU32 C_RED       = IM_COL32(214,  69,  69, 255);
static const ImU32 C_RED_D     = IM_COL32(179,  47,  47, 255);
static const ImU32 C_AMBER     = IM_COL32(199, 130,  14, 255);
static const ImU32 C_AMBER_L   = IM_COL32(255, 247, 228, 255);
static const ImU32 C_OK        = IM_COL32( 34, 160,  90, 255);

// 按钮悬浮态（比主色略亮）
static const ImU32 C_BLUE_H    = IM_COL32( 74, 126, 216, 255);
static const ImU32 C_GREEN_H   = IM_COL32( 24, 158, 124, 255);
static const ImU32 C_RED_H     = IM_COL32(228,  86,  86, 255);

// ImU32 → ImVec4，供 ImGui::PushStyleColor 使用
static inline ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// 窗口句柄(用于置顶切换)
static HWND  g_hwnd       = nullptr;
static bool  g_always_top = false;

// 迷你模式(悬浮小窗)
// 尺寸刻意做成竖版小卡片,而不是横向胶囊:
//   胶囊只能塞下一行字,信息密度低、拖动热区又和"单击展开"冲突;
//   竖窗可以排下"状态 / 数值 / 按钮"三层,与主界面的卡片语言一致。
static bool g_compact_mode = false;
static int g_normal_window_w = 480;   // 由 main.cpp 启动时按工作区裁剪后回填
static int g_normal_window_h = 820;
static const int   g_compact_window_w = 300;
static const int   g_compact_window_h = 186;
static const float g_compact_radius   = 14.0f;   // 普通圆角,不是胶囊(R ≠ h/2)

// ============================================================
// 卡片容器：先把内容画到上层通道，再把背景画到下层通道，
// 这样不必预先知道内容高度，也能让内容"浮"在卡片上。
// 同时负责内边距(indent)与外边距(dummy)，调用方只管往里塞控件。
//
// 宽度必须由调用方给出：组(Group)的包围盒只覆盖实际控件，
// 若按包围盒画背景，右对齐按钮多的时候卡片会缩窄。
// ============================================================
// 当前卡片的内边距与内容宽度（不在卡片内时分别为 0）
// 注意：ImGui 的 Indent() 只压缩左边界，GetContentRegionAvail() 也不含
// 右侧那份内边距，靠它推导会让子元素贴着卡片边框甚至溢出。
// 这里直接用卡片本身的几何算，最稳。
static float g_card_padx    = 0.0f;
static float g_card_inner_w = 0.0f;
static float g_card_left    = 0.0f;   // 卡片内容左边界（屏幕坐标）

// 卡片内可用内容宽度
static float card_w() { return g_card_inner_w; }

// 卡片内容右边界（屏幕坐标，用于右对齐）
static float card_right() {
    return g_card_left + g_card_inner_w;
}

// 把光标水平推到卡片内容右边界左侧 offset 处
static void align_right(float offset) {
    ImGui::SetCursorScreenPos(
        ImVec2(card_right() - offset, ImGui::GetCursorScreenPos().y));
}

class CardScope {
public:
    // w: 卡片外框宽度（含左右内边距）
    void begin(float w, float padx = 11.0f, float pady = 9.0f) {
        dl_   = ImGui::GetWindowDrawList();
        w_    = w;
        padx_ = padx;
        pady_ = pady;
        top_  = ImGui::GetCursorScreenPos().y;
        prev_padx_     = g_card_padx;
        prev_inner_w_  = g_card_inner_w;
        prev_left_     = g_card_left;
        g_card_padx    = padx_;
        g_card_inner_w = (w > padx_ * 2.0f) ? (w - padx_ * 2.0f) : w;
        dl_->ChannelsSplit(2);
        dl_->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::Indent(padx_);
        g_card_left = ImGui::GetCursorScreenPos().x;
        active_ = true;
    }
    void end(ImU32 fill = C_CARD, ImU32 border = C_BORDER) {
        if (!active_) return;
        ImGui::Unindent(padx_);
        g_card_padx    = prev_padx_;
        g_card_inner_w = prev_inner_w_;
        g_card_left    = prev_left_;
        ImGui::EndGroup();

        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const ImVec2 a(mn.x - padx_, top_ - pady_);
        const ImVec2 b(a.x + w_, mx.y + pady_);

        dl_->ChannelsSetCurrent(0);
        // 极轻投影，让卡片有"浮起"感
        dl_->AddRectFilled(ImVec2(a.x, a.y + 2.0f), ImVec2(b.x, b.y + 2.0f),
                           IM_COL32(15, 23, 42, 12), 13.0f);
        dl_->AddRectFilled(a, b, fill, 13.0f);
        dl_->AddRect(a, b, border, 13.0f, 0, 1.0f);
        dl_->ChannelsMerge();

        active_ = false;
        ImGui::Dummy(ImVec2(0.0f, 9.0f));
    }
private:
    ImDrawList* dl_     = nullptr;
    float       w_      = 0.0f;
    float       padx_   = 11.0f;
    float       pady_   = 9.0f;
    float       top_    = 0.0f;
    float       prev_padx_    = 0.0f;
    float       prev_inner_w_ = 0.0f;
    float       prev_left_    = 0.0f;
    bool        active_ = false;
};

// 小节标题：左侧短色条 + 标题 + 右侧弱化提示
static void section_title(const char* title, ImU32 accent, const char* trailing = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    const float bar_h = lh - 4.0f;

    dl->AddRectFilled(ImVec2(p.x, p.y + 2.5f),
                      ImVec2(p.x + 3.0f, p.y + 2.5f + bar_h), accent, 1.5f);

    ImGui::SetCursorScreenPos(ImVec2(p.x + 11.0f, p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    if (trailing) {
        ImVec2 ts = ImGui::CalcTextSize(trailing);
        dl->AddText(ImVec2(p.x + card_w() - ts.x, p.y + 2.0f),
                    C_TEXT_FAINT, trailing);
    }
    ImGui::Dummy(ImVec2(0.0f, 3.0f));
}

// 小圆点
static void dot(ImVec2 c, float r, ImU32 col) {
    ImGui::GetWindowDrawList()->AddCircleFilled(c, r, col);
}

// ============================================================
// 自绘小控件
// ============================================================

// 顶部/卡片内的文字按钮（悬浮变底色）
static bool tiny_button(const char* id, ImVec2 pos, ImVec2 size,
                        const char* text, bool active, ImU32 accent) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();

    ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);

    ImU32 bg = 0;
    if (active)            bg = C_BLUE_L;
    if (hovered)           bg = held ? C_TRACK_D : C_TRACK;
    if (active && hovered) bg = IM_COL32(224, 235, 251, 255);
    if (bg) dl->AddRectFilled(a, b, bg, 7.0f);

    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f,
                       pos.y + (size.y - ts.y) * 0.5f),
                active ? accent : C_TEXT_SUB, text);
    return clicked;
}

// 状态胶囊：圆点 + 文本，返回占用宽度
// pulse = true 时圆点呼吸，用于"正在连点"的实时反馈
static float status_pill(ImVec2 pos, const char* text, ImU32 dot_col, bool pulse) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float h = 26.0f;
    const float w = ts.x + 36.0f;

    ImVec2 a = pos, b(pos.x + w, pos.y + h);
    dl->AddRectFilled(a, b, C_TRACK, h * 0.5f);

    // 呼吸：光晕半径与透明度随正弦变化，静止时用固定值
    float glow_r = 6.5f;
    int   glow_a = 46;
    if (pulse) {
        const float k = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.4f);
        glow_r = 5.5f + 3.5f * k;
        glow_a = static_cast<int>(26.0f + 44.0f * k);
    }

    const ImVec2 dc(a.x + 13.0f, pos.y + h * 0.5f);
    dl->AddCircleFilled(dc, glow_r, (dot_col & 0x00FFFFFFu) | ((ImU32)glow_a << 24));
    dl->AddCircleFilled(dc, 4.0f, dot_col);

    dl->AddText(ImVec2(a.x + 23.0f, pos.y + (h - ts.y) * 0.5f), C_TEXT, text);
    return w;
}

// 主按钮（开始 / 停止），带禁用态
static bool solid_button(const char* label, ImVec2 size,
                         ImU32 base, ImU32 hover, ImU32 press, bool disabled) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        col4(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col4(press));
    ImGui::PushStyleColor(ImGuiCol_Text,          col4(C_WHITE));
    ImGui::BeginDisabled(disabled);
    const bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return clicked;
}

// 次级按钮：白底细边框
static bool ghost_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        col4(C_CARD));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(C_TRACK));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col4(C_TRACK_D));
    ImGui::PushStyleColor(ImGuiCol_Text,          col4(C_TEXT));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, col4(C_BORDER));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    return clicked;
}

// 统计卡：标题 + 大数字
static void stat_card(ImVec2 pos, ImVec2 size, const char* label,
                      const char* value, ImU32 accent) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);

    dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f), ImVec2(b.x, b.y + 2.0f),
                      IM_COL32(15, 23, 42, 12), 13.0f);
    dl->AddRectFilled(a, b, C_CARD, 13.0f);
    dl->AddRect(a, b, C_BORDER, 13.0f, 0, 1.0f);

    dl->AddText(ImVec2(a.x + 12.0f, a.y + 9.0f), C_TEXT_SUB, label);

    // 数字可能很长（点击到几十万、时长超过 100 小时），超出卡片宽度就逐级缩小字号，
    // 否则会压到卡片边框甚至溢出到另一张卡片上
    float fs = ImGui::GetFontSize() * 1.42f;
    const float avail = size.x - 24.0f;
    const float min_fs = ImGui::GetFontSize() * 0.86f;
    while (fs > min_fs) {
        const ImVec2 s = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, value);
        if (s.x <= avail) break;
        fs -= 1.0f;
    }

    const float ty = a.y + size.y - fs - 6.0f;
    dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + 12.0f, ty), accent, value);
}


// 小窗拖动状态
static bool g_compact_dragging = false;
static ImVec2 g_drag_offset(0, 0);

// 开始 / 停止的唯一入口：主界面底部按钮与迷你小窗共用，
// 免得两边各写一份、以后改了一边忘了另一边。
static void gui_start_macro() {
    if (active_mouse_id.load() == 0) {
        active_mouse_id.store(INTERCEPTION_MOUSE(0));
        add_log(u8"未检测到活跃鼠标,默认使用 MOUSE(0)", LogLevel::WARN);
    }
    // 自定义模式下没有动作 = 启动了也不会点，提前说清楚，别让人对着屏幕等
    if (g_click_mode.load() == static_cast<int>(ClickMode::CUSTOM)) {
        int step_count = 0;
        { std::lock_guard<std::mutex> lk(g_custom_mutex); step_count = g_custom_profile.count; }
        if (step_count == 0)
            add_log(u8"自定义模式没有动作，启动后不会产生点击", LogLevel::WARN);
    }
    is_macro_running.store(true);
    add_log(u8"[GUI] 开始", LogLevel::BEHAVIOR);
}

static void gui_stop_macro() {
    is_macro_running.store(false);
    add_log(u8"[GUI] 停止", LogLevel::BEHAVIOR);
}

static void toggle_pin() {
    g_always_top = !g_always_top;
    if (g_hwnd) {
        SetWindowPos(g_hwnd, g_always_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    add_log(g_always_top ? u8"窗口已置顶" : u8"窗口已取消置顶", LogLevel::INFO);
}

static void set_compact_mode(bool compact) {
    if (g_compact_mode == compact) return;
    g_compact_mode = compact;
    if (g_hwnd) {
        // 切换窗口样式:小窗模式下完全隐藏标题栏/边框,默认模式恢复为创建时的样式
        LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_POPUP | WS_BORDER);
        if (compact) {
            style |= WS_POPUP;
        } else {
            style |= (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME);
        }
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, style);

        RECT r; GetWindowRect(g_hwnd, &r);
        SetWindowPos(g_hwnd, nullptr,
                     r.left, r.top,
                     compact ? g_compact_window_w : g_normal_window_w,
                     compact ? g_compact_window_h : g_normal_window_h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        // 圆角:用 region 把四角裁成透明。半径固定 14,与主界面卡片圆角一致。
        // 注意以前这里传的是 h/2(胶囊),现在必须用固定值,否则又变成胶囊了。
        if (compact) {
            const int d = static_cast<int>(g_compact_radius * 2.0f);
            HRGN rgn = CreateRoundRectRgn(0, 0,
                g_compact_window_w + 1, g_compact_window_h + 1, d, d);
            SetWindowRgn(g_hwnd, rgn, TRUE);
        } else {
            SetWindowRgn(g_hwnd, nullptr, TRUE);
        }
    }
    add_log(compact ? u8"切换为迷你模式" : u8"已展开窗口", LogLevel::BEHAVIOR);
}

void set_gui_hwnd(HWND hwnd) { g_hwnd = hwnd; }

void set_normal_window_size(int w, int h) {
    if (w > 0) g_normal_window_w = w;
    if (h > 0) g_normal_window_h = h;
}

// 把毫秒格式化成 HH:MM:SS
static std::string format_hms(unsigned long long ms) {
    unsigned long long s = ms / 1000;
    unsigned long long h = s / 3600;
    unsigned long long m = (s / 60) % 60;
    unsigned long long sec = s % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", h, m, sec);
    return buf;
}

// 当前累计运行毫秒(含正在运行的本段)
static unsigned long long current_run_ms() {
    unsigned long long total = total_run_ms.load();
    long long start = run_start_ms_epoch.load();
    if (start > 0) {
        long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now > start) total += static_cast<unsigned long long>(now - start);
    }
    return total;
}

// ============================================================
// 热键工具(首页的触发键与模式切换键共用)
// ============================================================

// 键码 → 可读名称
static void vkey_name(uint8_t vk, char* buf, int bufsz) {
    if (vk >= VK_F1  && vk <= VK_F24) { snprintf(buf, bufsz, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= '0'    && vk <= '9')    { buf[0] = (char)vk; buf[1] = 0; return; }
    if (vk >= 'A'    && vk <= 'Z')    { buf[0] = (char)vk; buf[1] = 0; return; }
    if (vk == VK_TAB)   { snprintf(buf, bufsz, "Tab");   return; }
    if (vk == VK_SPACE) { snprintf(buf, bufsz, "Space"); return; }
    snprintf(buf, bufsz, "VK 0x%02X", vk);
}

// 组合键 → 描述字符串（用 parts 列表拼接，避免尾部 " + "）
static std::string hotkey_str(const HotkeyConfig& h) {
    std::vector<std::string> parts;
    if (h.key_ctrl)  parts.push_back("Ctrl");
    if (h.key_shift) parts.push_back("Shift");
    if (h.key_alt)   parts.push_back("Alt");
    for (uint8_t vk : h.vkeys) {
        // 0 只是"未使用"的占位，手改 ini 可能留下空洞，这里跳过而不是中断，
        // 否则 vkeys={0, 'R'} 这种会把 R 漏掉，显示与判定不一致
        if (vk == 0) continue;
        char nb[16]; vkey_name(vk, nb, sizeof(nb));
        parts.push_back(nb);
    }
    if (h.mouse_btn3) parts.push_back(u8"中键");
    if (h.mouse_btn4) parts.push_back("Button4");
    if (h.mouse_btn5) parts.push_back("Button5");
    if (parts.empty()) return u8"（未设置）";
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) s += " + ";
        s += parts[i];
    }
    return s;
}

// 扫描当前按住的所有键（录制用）
static HotkeyConfig scan_keys() {
    HotkeyConfig h{};
    h.mouse_btn3 = (GetAsyncKeyState(VK_MBUTTON)  & 0x8000) != 0;
    h.mouse_btn4 = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
    h.mouse_btn5 = (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
    h.key_ctrl   = (GetAsyncKeyState(VK_CONTROL)  & 0x8000) != 0;
    h.key_shift  = (GetAsyncKeyState(VK_SHIFT)    & 0x8000) != 0;
    h.key_alt    = (GetAsyncKeyState(VK_MENU)     & 0x8000) != 0;
    // 收集所有按下的非修饰键（最多 4 个）
    static const int candidates[] = {
        VK_TAB, VK_SPACE,
        VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,
        VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
        VK_F13,VK_F14,VK_F15,VK_F16,VK_F17,VK_F18,
        VK_F19,VK_F20,VK_F21,VK_F22,VK_F23,VK_F24,
    };
    int idx = 0;
    for (int vk : candidates) {
        if (idx >= 4) break;
        if (GetAsyncKeyState(vk) & 0x8000) h.vkeys[idx++] = static_cast<uint8_t>(vk);
    }
    for (int vk = '0'; vk <= 'Z' && idx < 4; ++vk) {
        // 0x3A~0x40 是标点（: ; < = > ? @），不属于热键常用键，跳过
        if (vk >= 0x3A && vk <= 0x40) continue;
        if (GetAsyncKeyState(vk) & 0x8000) h.vkeys[idx++] = static_cast<uint8_t>(vk);
    }
    return h;
}

static bool has_any(const HotkeyConfig& h) {
    bool has_vkey = false;
    for (uint8_t vk : h.vkeys) if (vk) { has_vkey = true; break; }
    return h.mouse_btn3 || h.mouse_btn4 || h.mouse_btn5 ||
           h.key_ctrl  || h.key_shift   || h.key_alt    || has_vkey;
}

// 两个组合是否完全一致（录制确认时做冲突检测）
static bool same_hotkey(const HotkeyConfig& a, const HotkeyConfig& b) {
    if (a.mouse_btn3 != b.mouse_btn3 || a.mouse_btn4 != b.mouse_btn4 ||
        a.mouse_btn5 != b.mouse_btn5) return false;
    if (a.key_ctrl != b.key_ctrl || a.key_shift != b.key_shift ||
        a.key_alt   != b.key_alt)   return false;
    for (int i = 0; i < 4; ++i)
        if (a.vkeys[i] != b.vkeys[i]) return false;
    return true;
}

// 各热键的默认值：0 = 触发键(Button4)，1 = 模式切换键(Button5)
static HotkeyConfig hotkey_default(int which) {
    HotkeyConfig h{};
    if (which == 0) h.mouse_btn4 = true;   // 触发键默认 Button4
    else            h.mouse_btn5 = true;   // 模式切换键默认 Button5
    return h;
}

// 默认值的可读名称（用于日志）
static const char* hotkey_default_name(int which) {
    return (which == 0) ? "Button4" : "Button5";
}

// ============================================================
// 热键卡片（显示当前组合 + 录制/恢复默认 + 录制状态机）
// which: 0 = 触发键（开始/停止），1 = 模式切换键（普通 ↔ 自定义）
// ============================================================
static void draw_hotkey_card(int which, const char* title, const char* hint, ImU32 accent) {
    // 两个区块的控件 label 相同(录制新组合/恢复默认/取消)，
    // ImGui 以 label 哈希生成控件 ID，不加作用域会互相撞 ID，
    // 表现为点击「模式切换键」的录制按钮实际命中「触发按键」的按钮。
    ImGui::PushID(which);

    // 当前值快照
    HotkeyConfig cur;
    { std::lock_guard<std::mutex> lk(g_hotkey_mutex);
      cur = (which == 0) ? g_hotkey : g_mode_switch_hotkey; }

    // 录制状态机（两个热键各自独立）
    enum class RecState { IDLE, WAITING, HOLDING };
    static RecState     s_state[2] = { RecState::IDLE, RecState::IDLE };
    static HotkeyConfig s_buf[2]   = {};
    std::atomic<bool>& rec_flag = (which == 0) ? g_hotkey_recording
                                               : g_mode_switch_recording;

    const char* id_suffix = (which == 0) ? "##hk_trigger" : "##hk_mode";
    const RecState st = s_state[which];
    const bool rec = (st != RecState::IDLE);

    CardScope card;
    card.begin(ImGui::GetContentRegionAvail().x);

    section_title(title, accent, hint);

    // ---- 按键展示框 + 操作按钮（同一行，省纵向空间）----
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 先算按钮占多宽，剩下的都留给按键框
        const float bh  = 38.0f;
        const float tp  = ImGui::GetStyle().FramePadding.x;
        const float gap = 8.0f;
        const float btn_w = rec
            ? ImGui::CalcTextSize(u8"取消").x + tp * 2.0f
            : ImGui::CalcTextSize(u8"录制新组合").x + tp * 2.0f
              + ImGui::CalcTextSize(u8"恢复默认").x + tp * 2.0f + gap;
        const float bw = card_w() - btn_w - gap;

        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 p2(p.x + bw, p.y + bh);

        const ImU32 bg = rec ? C_AMBER_L : C_TRACK;
        const ImU32 bd = rec ? IM_COL32(240, 205, 128, 255) : C_TRACK_D;
        const ImU32 fg = rec ? C_AMBER : C_TEXT;

        dl->AddRectFilled(p, p2, bg, 9.0f);
        dl->AddRect(p, p2, bd, 9.0f, 0, 1.0f);

        char text[160];
        if (st == RecState::IDLE)         snprintf(text, sizeof(text), "%s", hotkey_str(cur).c_str());
        else if (st == RecState::WAITING) snprintf(text, sizeof(text), u8"请按住组合键…");
        else                              snprintf(text, sizeof(text), "%s", hotkey_str(s_buf[which]).c_str());

        // 录制中：左侧呼吸小圆点
        if (rec) {
            const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f);
            dl->AddCircleFilled(ImVec2(p.x + 15.0f, p.y + bh * 0.5f), 4.5f,
                                IM_COL32(214, 130, 20, (int)(110.0f + 145.0f * pulse)));
        }

        // 组合键可能很长（Ctrl+Shift+Alt+Button4）：先缩字号，仍放不下就裁切，
        // 绝不压到右侧按钮上
        const float min_fs = ImGui::GetFontSize() * 0.82f;
        const float avail  = bw - (rec ? 34.0f : 20.0f);
        float fs = ImGui::GetFontSize() * 1.12f;
        while (fs > min_fs) {
            const ImVec2 s = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
            if (s.x <= avail) break;
            fs -= 1.0f;
        }
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);

        dl->PushClipRect(ImVec2(p.x + 2.0f, p.y), ImVec2(p2.x - 2.0f, p2.y), true);
        dl->AddText(ImGui::GetFont(), fs,
                    ImVec2(p.x + (bw - ts.x) * 0.5f + (rec ? 8.0f : 0.0f),
                           p.y + (bh - ts.y) * 0.5f),
                    fg, text);
        dl->PopClipRect();

        ImGui::Dummy(ImVec2(bw, bh));
        ImGui::SameLine(0.0f, gap);
    }

    // ---- 操作区（紧接在按键框右侧）----
    switch (st) {
    case RecState::IDLE: {
        if (solid_button(u8"录制新组合", ImVec2(0, 38),
                         accent, (which == 0) ? C_BLUE_H : C_GREEN_H,
                         (which == 0) ? C_BLUE_D : C_GREEN_D, false)) {
            s_buf[which] = {};
            rec_flag.store(true);
            s_state[which] = RecState::WAITING;
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (ghost_button(u8"恢复默认", ImVec2(0, 38))) {
            HotkeyConfig def = hotkey_default(which);
            { std::lock_guard<std::mutex> lk(g_hotkey_mutex);
              if (which == 0) g_hotkey = def;
              else            g_mode_switch_hotkey = def; }
            add_log(std::string(title) + u8" 已恢复默认（" +
                    hotkey_default_name(which) + u8"）", LogLevel::BEHAVIOR);
        }
        break;
    }
    case RecState::WAITING:
    case RecState::HOLDING: {
        {
            char label[64];
            snprintf(label, sizeof(label), u8"取消%s", id_suffix);
            if (ghost_button(label, ImVec2(0, 38))) {
                rec_flag.store(false);
                s_state[which] = RecState::IDLE;
            }
        }
        // 提示放在框下面一行：只在录制时出现，不影响常态布局高度
        ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
        if (st == RecState::WAITING) ImGui::TextUnformatted(u8"按下要用的组合键，全部松开后自动确认");
        else                         ImGui::TextUnformatted(u8"松开全部按键即确认");
        ImGui::PopStyleColor();

        if (st == RecState::WAITING) {
            HotkeyConfig now = scan_keys();
            if (has_any(now)) {
                s_buf[which] = now;
                s_state[which] = RecState::HOLDING;
            }
        } else {
            HotkeyConfig now = scan_keys();
            if (has_any(now)) {
                // 合并按住期间新出现的键
                if (now.mouse_btn3) s_buf[which].mouse_btn3 = true;
                if (now.mouse_btn4) s_buf[which].mouse_btn4 = true;
                if (now.mouse_btn5) s_buf[which].mouse_btn5 = true;
                if (now.key_ctrl)   s_buf[which].key_ctrl   = true;
                if (now.key_shift)  s_buf[which].key_shift  = true;
                if (now.key_alt)    s_buf[which].key_alt    = true;
                for (uint8_t vk : now.vkeys) {
                    if (vk == 0) continue;
                    bool found = false;
                    for (uint8_t rv : s_buf[which].vkeys) if (rv == vk) { found = true; break; }
                    if (!found) {
                        for (uint8_t& rv : s_buf[which].vkeys) { if (rv == 0) { rv = vk; break; } }
                    }
                }
            }

            // 松开所有键 → 确认
            if (!has_any(now)) {
                std::string preview = hotkey_str(s_buf[which]);
                // 冲突检测：不允许与另一个热键完全相同
                HotkeyConfig other;
                { std::lock_guard<std::mutex> lk(g_hotkey_mutex);
                  other = (which == 0) ? g_mode_switch_hotkey : g_hotkey; }
                if (same_hotkey(s_buf[which], other)) {
                    add_log(std::string(title) +
                            u8" 与另一热键完全相同，已放弃修改", LogLevel::WARN);
                } else {
                    { std::lock_guard<std::mutex> lk(g_hotkey_mutex);
                      if (which == 0) g_hotkey = s_buf[which];
                      else            g_mode_switch_hotkey = s_buf[which]; }
                    add_log(std::string(title) + u8" 已更新 → " + preview, LogLevel::BEHAVIOR);
                }
                rec_flag.store(false);
                s_state[which] = RecState::IDLE;
            }
        }
        break;
    }
    }

    card.end();
    ImGui::PopID();
}

// ============================================================
// 模式选择卡（两张并排，整块可点）
// ============================================================
struct ModeCardStyle {
    ImU32 accent;    // 选中态填充 / 描边
    ImU32 soft;      // 未选中态描边
    ImU32 text;      // 未选中态标题色
};

static void draw_mode_card(float card_w, const char* title, const char* desc,
                           bool selected, int mode_value, const ModeCardStyle& st) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(card_w, 62.0f);
    const ImVec2 p2(p.x + sz.x, p.y + sz.y);

    ImU32 fill, border, title_col, desc_col;
    if (selected) {
        fill      = st.accent;
        border    = st.accent;
        title_col = IM_COL32(255, 255, 255, 255);
        desc_col  = IM_COL32(255, 255, 255, 210);
    } else {
        fill      = C_WHITE;
        border    = C_BORDER;
        title_col = st.text;
        desc_col  = C_TEXT_FAINT;
    }

    dl->AddRectFilled(p, p2, fill, 10.0f);
    dl->AddRect(p, p2, border, 10.0f, 0, 1.0f);

    ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) * 0.5f, p.y + 13.0f), title_col, title);
    ImVec2 ds = ImGui::CalcTextSize(desc);
    dl->AddText(ImVec2(p.x + (sz.x - ds.x) * 0.5f, p.y + 37.0f), desc_col, desc);

    // 透明按钮捕获点击
    ImGui::SetCursorScreenPos(p);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(1, 1, 1, 0.08f)
                                                           : ImVec4(0, 0, 0, 0.025f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0, 0, 0, 0.06f));
    char id[32];
    snprintf(id, sizeof(id), "##mode_%d", mode_value);
    if (ImGui::Button(id, sz)) {
        set_click_mode(mode_value);
    }
    if (ImGui::IsItemHovered() && !selected) {
        dl->AddRect(p, p2, st.accent, 10.0f, 0, 1.4f);
    }
    ImGui::PopStyleColor(3);
}


// ============================================================
// 自定义动作的按键：录制方式采集，支持几乎所有键
// vk = 0 是一个哨兵值，表示鼠标左键（其余为真实虚拟键码）
// ============================================================

static void action_key_name(uint8_t vk, char* buf, int bufsz) {
    if (vk == 0) { snprintf(buf, bufsz, u8"鼠标左键"); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, bufsz, "F%d", vk - VK_F1 + 1); return; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = static_cast<char>(vk);
        buf[1] = 0;
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, bufsz, "Num%d", vk - VK_NUMPAD0);
        return;
    }

    static const struct { uint8_t vk; const char* name; } kNames[] = {
        { VK_LBUTTON,  u8"鼠标左键" },
        { VK_RBUTTON,  u8"鼠标右键" },
        { VK_MBUTTON,  u8"鼠标中键" },
        { VK_XBUTTON1, "Mouse Button4" },
        { VK_XBUTTON2, "Mouse Button5" },
        { VK_BACK,     "Backspace" },
        { VK_TAB,      "Tab" },
        { VK_RETURN,   "Enter" },
        { VK_SHIFT,    "Shift" },
        { VK_CONTROL,  "Ctrl" },
        { VK_MENU,     "Alt" },
        { VK_PAUSE,    "Pause" },
        { VK_CAPITAL,  "CapsLock" },
        { VK_ESCAPE,   "Esc" },
        { VK_SPACE,    "Space" },
        { VK_PRIOR,    "PageUp" },
        { VK_NEXT,     "PageDown" },
        { VK_END,      "End" },
        { VK_HOME,     "Home" },
        { VK_LEFT,     u8"←" },
        { VK_UP,       u8"↑" },
        { VK_RIGHT,    u8"→" },
        { VK_DOWN,     u8"↓" },
        { VK_SNAPSHOT, "PrintScreen" },
        { VK_INSERT,   "Insert" },
        { VK_DELETE,   "Delete" },
        { VK_LWIN,     "Left Win" },
        { VK_RWIN,     "Right Win" },
        { VK_APPS,     "Menu" },
        { VK_MULTIPLY, "Num *" },
        { VK_ADD,      "Num +" },
        { VK_SUBTRACT, "Num -" },
        { VK_DECIMAL,  "Num ." },
        { VK_DIVIDE,   "Num /" },
        { VK_NUMLOCK,  "NumLock" },
        { VK_SCROLL,   "ScrollLock" },
        { VK_OEM_1,      u8";" },
        { VK_OEM_PLUS,   u8"=" },
        { VK_OEM_COMMA,  u8"," },
        { VK_OEM_MINUS,  u8"-" },
        { VK_OEM_PERIOD, u8"." },
        { VK_OEM_2,      u8"/" },
        { VK_OEM_3,      u8"`" },
        { VK_OEM_4,      u8"[" },
        { VK_OEM_5,      u8"\\" },
        { VK_OEM_6,      u8"]" },
        { VK_OEM_7,      u8"'" },
        { VK_LSHIFT,   "Left Shift" },
        { VK_RSHIFT,   "Right Shift" },
        { VK_LCONTROL, "Left Ctrl" },
        { VK_RCONTROL, "Right Ctrl" },
        { VK_LMENU,    "Left Alt" },
        { VK_RMENU,    "Right Alt" },
    };
    for (const auto& e : kNames) {
        if (e.vk == vk) { snprintf(buf, bufsz, "%s", e.name); return; }
    }
    snprintf(buf, bufsz, "VK 0x%02X", vk);
}

// ============================================================
// 自定义动作的按键录制（与上方热键录制同一套思路）
// 逐帧累积按下的键，直到「全部松开」才确认。这样才能录到
// S+D 这种先后按下、同时按住的组合，而不是抓到第一个键就跑。
// ============================================================
struct StepCapture {
    uint8_t mods      = 0;      // CMOD_* 累积
    uint8_t mouse_vk  = 0;      // 0 = 鼠标左键
    bool    has_mouse = false;
    uint8_t keys[4]   = {};     // 普通键，最多 4 个
    int     key_count = 0;

    // 是否录到了「真正的主键」（只有修饰键不算，不能作为一条动作）
    bool has_key() const { return has_mouse || key_count > 0; }
    void clear()        { *this = StepCapture{}; }
};

// 把当前按住的键并入缓冲。返回值 = 本帧是否有任何键（含修饰键）处于按下状态，
// 用于判断「全部松开」这个提交时机。注意不能用缓冲区是否为空来判断 ——
// 缓冲区是累积的，按键松开后内容依然还在。
static bool accumulate_step_keys(StepCapture& buf) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool any_down = false;

    // 修饰键：左右任一侧都认
    if (down(VK_CONTROL) || down(VK_LCONTROL) || down(VK_RCONTROL)) { buf.mods |= CMOD_CTRL;  any_down = true; }
    if (down(VK_SHIFT)   || down(VK_LSHIFT)   || down(VK_RSHIFT))   { buf.mods |= CMOD_SHIFT; any_down = true; }
    if (down(VK_MENU)    || down(VK_LMENU)    || down(VK_RMENU))    { buf.mods |= CMOD_ALT;   any_down = true; }

    // 鼠标键（只认一个，作主键）。左键用历史哨兵 0
    if (!buf.has_mouse) {
        uint8_t mk = 0;
        bool hit = true;
        if (down(VK_LBUTTON))       mk = 0;
        else if (down(VK_RBUTTON))  mk = VK_RBUTTON;
        else if (down(VK_MBUTTON))  mk = VK_MBUTTON;
        else if (down(VK_XBUTTON1)) mk = VK_XBUTTON1;
        else if (down(VK_XBUTTON2)) mk = VK_XBUTTON2;
        else                        hit = false;
        if (hit) { buf.mouse_vk = mk; buf.has_mouse = true; }
    }
    if (down(VK_LBUTTON) || down(VK_RBUTTON) || down(VK_MBUTTON) ||
        down(VK_XBUTTON1) || down(VK_XBUTTON2)) any_down = true;

    // 普通键：从 8 开始（跳过鼠标键 1~6），Esc 留给「取消录制」
    for (int vk = 8; vk < 255; ++vk) {
        if (vk == VK_ESCAPE) continue;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_LMENU || vk == VK_RMENU) continue;
        if (!down(vk)) continue;
        any_down = true;
        if (buf.key_count >= 4) continue;   // 满了也要继续扫，保证 any_down 准确
        bool dup = false;
        for (int i = 0; i < buf.key_count; ++i)
            if (buf.keys[i] == vk) { dup = true; break; }
        if (!dup) buf.keys[buf.key_count++] = static_cast<uint8_t>(vk);
    }
    return any_down;
}

// 录制过程中的实时预览
static void step_capture_label(const StepCapture& b, char* buf, int bufsz) {
    std::string s;
    if (b.mods & CMOD_CTRL)  s += "Ctrl+";
    if (b.mods & CMOD_SHIFT) s += "Shift+";
    if (b.mods & CMOD_ALT)   s += "Alt+";

    std::string keys;
    char kname[32];
    if (b.has_mouse) {
        action_key_name(b.mouse_vk, kname, sizeof(kname));
        keys += kname;
    }
    for (int i = 0; i < b.key_count; ++i) {
        action_key_name(b.keys[i], kname, sizeof(kname));
        if (!keys.empty()) keys += "+";
        keys += kname;
    }
    s += keys;
    if (s.empty()) s = u8"（等待按键）";
    snprintf(buf, bufsz, "%s", s.c_str());
}

// 一条动作的显示名：修饰键 + 主键 + 额外键，如 Ctrl+Shift+S+D
static void action_step_label(const CustomStep& st, char* buf, int bufsz) {
    std::string s;
    if (st.mods & CMOD_CTRL)  s += "Ctrl+";
    if (st.mods & CMOD_SHIFT) s += "Shift+";
    if (st.mods & CMOD_ALT)   s += "Alt+";

    char kname[32];
    action_key_name(st.vk, kname, sizeof(kname));
    s += kname;

    for (uint8_t v : st.vkeys) {
        if (v == 0) continue;
        action_key_name(v, kname, sizeof(kname));
        s += "+";
        s += kname;
    }
    snprintf(buf, bufsz, "%s", s.c_str());
}


// ============================================================
// 主渲染
// ============================================================
void render_gui() {
    const int  click_mode = g_click_mode.load();
    const bool custom     = (click_mode == static_cast<int>(ClickMode::CUSTOM));

    // 自定义动作按键录制状态（跨帧保持）
    static int  s_rec_row          = -1;     // 正在录制的行号，-1 = 未录制
    static bool s_rec_wait_release = false;  // 先等左键松开，避免把「点按钮」录成鼠标左键
    static bool s_rec_drawn        = false;  // 本帧是否绘制过录制器
    static std::chrono::steady_clock::time_point s_rec_t0{};  // 录制开始时刻（超时保护）
    static StepCapture s_rec_buf;            // 逐帧累积的按键
    static bool        s_rec_holding = false; // 已捕获到按键，等全部松开后确认

    // 上一帧没绘制录制器（切走模式 / 切到别的 Tab / 进入迷你模式）→ 自动释放，
    // 否则 g_action_key_recording 会一直是 true，热键全被屏蔽
    if (s_rec_row >= 0 && !s_rec_drawn) {
        s_rec_row = -1;
        s_rec_wait_release = false;
        s_rec_holding = false;
        s_rec_buf.clear();
        g_action_key_recording.store(false);
    }
    s_rec_drawn = false;

    // ============================================================
    // 迷你小窗（不是胶囊）
    //   竖版卡片，从上到下四层：
    //     ① 顶部 3px 状态色条（运行绿 / 停止红）
    //     ② 标题行：状态点 + 状态文字 + 模式标签 + 展开按钮
    //     ③ 双数值：累计点击 / 运行时长
    //     ④ 主按钮：开始或停止（随状态自适应）
    //   拖动标题行 = 移动窗口；双击标题行 = 展开主界面。
    //   尺寸与 g_compact_window_w/h 一一对应：44 + 74 + 68 = 186
    // ============================================================
    if (g_compact_mode) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags cflags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##mini", nullptr, cflags);
        ImGui::PopStyleVar(3);

        ImDrawList* dl       = ImGui::GetWindowDrawList();
        const ImVec2 p0      = ImGui::GetWindowPos();
        const ImVec2 sz      = ImGui::GetWindowSize();
        const float  W       = sz.x;
        const float  H       = sz.y;
        const bool   running = is_macro_running.load();
        const float  R       = g_compact_radius;

        // 布局常量
        const float PAD    = 14.0f;
        const float HDR_H  = 44.0f;   // 标题行（含底部 3px 状态条）
        const float STAT_H = 74.0f;   // 数值区
        const float BTN_H  = 38.0f;   // 主按钮
        const float stat_y = p0.y + HDR_H;
        const float foot_y = stat_y + STAT_H;
        const float cy     = p0.y + (HDR_H - 3.0f) * 0.5f;   // 标题行视觉中心
        const ImU32 stat_c = running ? C_OK : C_RED;

        // ---- 窗体底色 + 标题区淡染 + 状态色条 ----
        // 停止态标题区用中性灰白（顶条仍是红色以示"没在跑"），
        // 运行态才让整块泛绿——静止时安静，跑起来一眼就能看见。
        dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), C_CARD, R);
        dl->AddRectFilled(p0, ImVec2(p0.x + W, stat_y),
                          running ? IM_COL32(236, 250, 245, 255)
                                  : IM_COL32(247, 249, 252, 255),
                          R, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(ImVec2(p0.x, stat_y - 3.0f), ImVec2(p0.x + W, stat_y), stat_c);
        // 内缩 0.5px：1px 线以 p0 为中线会被 region 裁掉一半
        dl->AddRect(ImVec2(p0.x + 0.5f, p0.y + 0.5f),
                    ImVec2(p0.x + W - 0.5f, p0.y + H - 0.5f), C_BORDER, R, 0, 1.0f);

        // ---- 标题行内容：状态点 + 状态文字 + 模式标签 ----
        {
            const char*  st_txt = running ? u8"运行中" : u8"已停止";
            const float  lh     = ImGui::GetTextLineHeight();
            const ImVec2 dc(p0.x + PAD + 3.0f, cy);

            // 运行中让光晕呼吸，静止时用固定值（和主界面状态胶囊同款）
            float glow_r = 6.5f;
            int   glow_a = 46;
            if (running) {
                const float k = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.4f);
                glow_r = 5.5f + 3.5f * k;
                glow_a = static_cast<int>(26.0f + 44.0f * k);
            }
            dl->AddCircleFilled(dc, glow_r, (stat_c & 0x00FFFFFFu) | ((ImU32)glow_a << 24));
            dl->AddCircleFilled(dc, 4.0f, stat_c);

            const float tx = dc.x + 12.0f;
            dl->AddText(ImVec2(tx, cy - lh * 0.5f), C_TEXT, st_txt);

            // 模式标签：白底细边小胶囊——只是一枚标签，不是整个窗口的形态
            const char*  mtxt = custom ? u8"自定义" : u8"普通";
            const ImVec2 ms   = ImGui::CalcTextSize(mtxt);
            const float  ch   = 20.0f;
            const ImVec2 a(tx + ImGui::CalcTextSize(st_txt).x + 9.0f, cy - ch * 0.5f);
            const ImVec2 b(a.x + ms.x + 14.0f, a.y + ch);
            dl->AddRectFilled(a, b, C_CARD, ch * 0.5f);
            dl->AddRect(a, b, C_BORDER, ch * 0.5f, 0, 1.0f);
            dl->AddText(ImVec2(a.x + 7.0f, cy - ms.y * 0.5f), C_TEXT_SUB, mtxt);
        }

        // ---- 展开按钮（标题行右侧）----
        const float exp_w = 46.0f;
        const float exp_h = 24.0f;
        const ImVec2 exp_pos(p0.x + W - PAD - exp_w, cy - exp_h * 0.5f);
        bool want_expand = false;
        ImGui::SetCursorScreenPos(exp_pos);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
        if (ghost_button(u8"展开", ImVec2(exp_w, exp_h))) want_expand = true;
        ImGui::PopStyleVar();

        // ---- 拖动热区：标题行去掉展开按钮那块 ----
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##dragbar", ImVec2(W - PAD - exp_w - 6.0f, HDR_H));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        if (ImGui::IsItemActive() && g_hwnd) {
            // 双击标题行 = 展开（单击不再展开：和拖动抢手势，误触太多）
            if (!g_compact_dragging && ImGui::IsMouseDoubleClicked(0)) {
                want_expand = true;
            } else {
                const ImVec2 d = ImGui::GetMouseDragDelta(0);
                if (!g_compact_dragging && d.x == 0.0f && d.y == 0.0f) {
                    POINT cp; GetCursorPos(&cp);
                    RECT  r;  GetWindowRect(g_hwnd, &r);
                    g_drag_offset = ImVec2((float)(cp.x - r.left), (float)(cp.y - r.top));
                }
                if (ImGui::IsMouseDragging(0, 3.0f)) {
                    g_compact_dragging = true;
                    POINT cp; GetCursorPos(&cp);
                    SetWindowPos(g_hwnd, nullptr,
                                 cp.x - (int)g_drag_offset.x,
                                 cp.y - (int)g_drag_offset.y,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        if (ImGui::IsItemDeactivated()) g_compact_dragging = false;

        // ---- 数值区：两列居中，中缝一条细线 ----
        {
            const float col_w = (W - PAD * 2.0f) * 0.5f;
            const std::string cnt = std::to_string(total_click_count.load());
            const std::string dur = format_hms(current_run_ms());

            auto draw_col = [&](float cx, const char* cap, const char* val, ImU32 accent) {
                const ImVec2 cs = ImGui::CalcTextSize(cap);
                dl->AddText(ImVec2(cx - cs.x * 0.5f, stat_y + 12.0f), C_TEXT_SUB, cap);

                // 数字可能很长（几十万次 / 上百小时），超宽就逐级缩字号，别压到中缝上
                float fs = ImGui::GetFontSize() * 1.5f;
                const float avail  = col_w - 20.0f;
                const float min_fs = ImGui::GetFontSize() * 0.8f;
                while (fs > min_fs) {
                    const ImVec2 s = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, val);
                    if (s.x <= avail) break;
                    fs -= 1.0f;
                }
                const ImVec2 vs = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, val);
                dl->AddText(ImGui::GetFont(), fs,
                            ImVec2(cx - vs.x * 0.5f, stat_y + STAT_H - fs - 12.0f), accent, val);
            };

            const float cx1 = p0.x + PAD + col_w * 0.5f;
            const float cx2 = p0.x + PAD + col_w * 1.5f;
            draw_col(cx1, u8"累计点击", cnt.c_str(), C_BLUE);
            draw_col(cx2, u8"运行时长", dur.c_str(), C_GREEN);

            dl->AddLine(ImVec2(p0.x + PAD + col_w, stat_y + 14.0f),
                        ImVec2(p0.x + PAD + col_w, stat_y + STAT_H - 14.0f), C_BORDER, 1.0f);
            dl->AddLine(ImVec2(p0.x, foot_y - 0.5f), ImVec2(p0.x + W, foot_y - 0.5f),
                        C_BORDER, 1.0f);
        }

        // ---- 主按钮：只留一个键，按状态自适应 ----
        {
            ImGui::SetCursorScreenPos(ImVec2(p0.x + PAD, foot_y + 16.0f));
            const ImVec2 bs(W - PAD * 2.0f, BTN_H);
            const bool clicked = running
                ? solid_button(u8"停 止", bs, C_RED,   C_RED_H,   C_RED_D,   false)
                : solid_button(u8"开 始", bs, C_GREEN, C_GREEN_H, C_GREEN_D, false);
            if (clicked) {
                if (running) gui_stop_macro();
                else         gui_start_macro();
            }
            if (ImGui::IsItemHovered()) {
                HotkeyConfig trig;
                { std::lock_guard<std::mutex> lk(g_hotkey_mutex); trig = g_hotkey; }
                ImGui::SetTooltip("%s", (std::string(u8"热键 ") + hotkey_str(trig)).c_str());
            }
        }

        ImGui::End();
        if (want_expand) set_compact_mode(false);
        return;
    }

    // ============================================================
    // 主窗口
    // ============================================================
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImDrawList* dl       = ImGui::GetWindowDrawList();
    const ImVec2 wp      = ImGui::GetWindowPos();
    const ImVec2 wsz     = ImGui::GetWindowSize();
    const bool   running = is_macro_running.load();

    const float HEADER_H = 56.0f;
    const float PAD_X    = 14.0f;
    const float inner_w  = wsz.x - PAD_X * 2.0f;

    // ---------------- 顶部标题栏 ----------------
    {
        const ImVec2 p0(wp.x, wp.y);
        const ImVec2 p1(wp.x + wsz.x, wp.y + HEADER_H);
        dl->AddRectFilled(p0, p1, C_CARD);
        dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f), C_BORDER, 1.0f);

        // 标题
        const float  title_fs = ImGui::GetFontSize() * 1.28f;
        const ImVec2 tsz      = ImGui::GetFont()->CalcTextSizeA(title_fs, FLT_MAX, 0.0f, u8"OnePiece");
        dl->AddText(ImGui::GetFont(), title_fs,
                    ImVec2(p0.x + PAD_X + 1.0f, p0.y + (HEADER_H - title_fs) * 0.5f - 1.0f),
                    C_TEXT, u8"OnePiece");

        // 版本徽标
        {
            const char*  ver = "v2.0";
            const ImVec2 vs  = ImGui::CalcTextSize(ver);
            const ImVec2 a(p0.x + PAD_X + 1.0f + tsz.x + 9.0f, p0.y + HEADER_H * 0.5f - 9.0f);
            const ImVec2 b(a.x + vs.x + 12.0f, a.y + 18.0f);
            dl->AddRectFilled(a, b, C_TRACK, 9.0f);
            dl->AddText(ImVec2(a.x + 6.0f, a.y + (18.0f - vs.y) * 0.5f), C_TEXT_SUB, ver);
        }

        // 右侧：状态胶囊 + 置顶 + 迷你
        const float bh = 26.0f;
        const float by = p0.y + (HEADER_H - bh) * 0.5f;
        float rx = p1.x - PAD_X;

        rx -= 46.0f;
        if (tiny_button("##to_mini", ImVec2(rx, by), ImVec2(46, bh), u8"迷你", false, C_BLUE))
            set_compact_mode(true);

        rx -= 8.0f;
        rx -= 52.0f;
        if (tiny_button("##pin_top", ImVec2(rx, by), ImVec2(52, bh),
                        g_always_top ? u8"已置顶" : u8"置顶", g_always_top, C_BLUE))
            toggle_pin();

        rx -= 12.0f;
        {
            const char* st_txt = running ? u8"运行中" : u8"已停止";
            rx -= ImGui::CalcTextSize(st_txt).x + 36.0f;
            status_pill(ImVec2(rx, by), st_txt, running ? C_OK : C_RED, running);
        }
    }

    // ---------------- 统计区 ----------------
    float y = HEADER_H + 12.0f;

    {
        ImGui::SetCursorPos(ImVec2(PAD_X, y));
        const float  gap = 10.0f;
        const float  cw  = (inner_w - gap) * 0.5f;
        const ImVec2 sp  = ImGui::GetCursorScreenPos();

        const std::string cnt  = std::to_string(total_click_count.load());
        const std::string dur  = format_hms(current_run_ms());

        stat_card(sp, ImVec2(cw, 68.0f), u8"累计点击", cnt.c_str(), C_BLUE);
        stat_card(ImVec2(sp.x + cw + gap, sp.y), ImVec2(cw, 68.0f),
                  u8"运行时长", dur.c_str(), C_GREEN);

        y += 68.0f + 10.0f;
    }

    // ---------------- 鼠标驱动状态条 ----------------
    {
        const InterceptionDevice mid = active_mouse_id.load();
        const ImVec2 a(wp.x + PAD_X, wp.y + y);
        const ImVec2 b(a.x + inner_w, a.y + 32.0f);

        dl->AddRectFilled(a, b, mid ? C_TRACK : C_AMBER_L, 9.0f);
        dl->AddCircleFilled(ImVec2(a.x + 15.0f, a.y + 16.0f), 4.0f, mid ? C_OK : C_AMBER);

        char txt[192];
        if (mid == 0) snprintf(txt, sizeof(txt), u8"未识别到鼠标驱动 —— 请在窗口外随意动一下鼠标");
        else          snprintf(txt, sizeof(txt), u8"鼠标驱动已就绪   ·   设备 ID %d", (int)mid);

        dl->AddText(ImVec2(a.x + 27.0f, a.y + (32.0f - ImGui::GetTextLineHeight()) * 0.5f),
                    mid ? C_TEXT_SUB : C_AMBER, txt);

        y += 32.0f + 12.0f;
    }

    // ---------------- 标签页内容 ----------------
    const float bottom_h  = 82.0f;
    float content_h = wsz.y - y - bottom_h;
    if (content_h < 120.0f) content_h = 120.0f;

    ImGui::SetCursorPos(ImVec2(PAD_X, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 6));
    ImGui::BeginChild("##content", ImVec2(inner_w, content_h), false);
    ImGui::PopStyleVar();

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
        // ========================= 首页 =========================
        if (ImGui::BeginTabItem(u8"首页")) {
            ImGui::Dummy(ImVec2(0, 2));

            const float full_w = ImGui::GetContentRegionAvail().x;

            // ---- 工作模式 ----
            {
                CardScope card;
                card.begin(full_w);
                section_title(u8"工作模式", C_BLUE, u8"运行中亦可切换");

                const float sp  = ImGui::GetStyle().ItemSpacing.x;
                const float mcw = (card_w() - sp) * 0.5f;

                const ModeCardStyle st_normal = { C_BLUE,  C_BORDER, C_TEXT };
                const ModeCardStyle st_custom = { C_GREEN, C_BORDER, C_TEXT };

                draw_mode_card(mcw, u8"普通", u8"左键固定频率连点",
                               click_mode == (int)ClickMode::NORMAL,
                               (int)ClickMode::NORMAL, st_normal);
                ImGui::SameLine(0.0f, sp);
                draw_mode_card(mcw, u8"自定义", u8"自由编排按键序列",
                               click_mode == (int)ClickMode::CUSTOM,
                               (int)ClickMode::CUSTOM, st_custom);
                card.end();
            }

            // ---- 触发按键 ----
            draw_hotkey_card(0, u8"触发按键", u8"开始 / 停止", C_BLUE);

            // ---- 模式切换键 ----
            draw_hotkey_card(1, u8"模式切换键", u8"普通 / 自定义", C_GREEN);

            // ---- 参数 ----
            if (click_mode == (int)ClickMode::NORMAL) {
                // ======== 普通模式参数 ========
                CardScope card;
                card.begin(full_w);
                section_title(u8"连点参数", C_BLUE, u8"单位 ms");

                // 单个参数行：左标签 + 右输入框
                auto param_row = [](const char* label, const char* id,
                                    std::atomic<int>& param, int lo, int hi,
                                    int step, int step_fast,
                                    const char* hint, const char* log_tag) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine();
                    align_right(106.0f);
                    ImGui::SetNextItemWidth(106.0f);
                    int v = param.load();
                    if (ImGui::InputInt(id, &v, step, step_fast)) {
                        if (v < lo) v = lo;
                        if (v > hi) v = hi;
                        param.store(v);
                        add_log(std::string(u8"[参数] ") + log_tag + u8" → " +
                                std::to_string(v) + " ms", LogLevel::BEHAVIOR);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
                };

                param_row(u8"按住时长", "##p_base",  param_press_base_ms,     1, 60000, 5, 20,
                          u8"左键按下后保持不动的时间", u8"按住基础时长");
                param_row(u8"按住随机", "##p_pjit",  param_press_jitter_ms,   0, 5000,  1, 10,
                          u8"在按住时长上再随机加 0~该值", u8"按住随机");
                param_row(u8"松开随机", "##p_rjit",  param_release_jitter_ms, 0, 5000,  1, 10,
                          u8"松开后随机等 0~该值 再进入下一轮", u8"松开随机");
                param_row(u8"循环间隔", "##p_gap",   param_loop_gap_ms,       0, 60000, 10, 50,
                          u8"每次点击之间的固定间隔", u8"循环间隔");

                // 实时时序预览
                {
                    const int base = param_press_base_ms.load();
                    const int pjit = param_press_jitter_ms.load();
                    const int rjit = param_release_jitter_ms.load();
                    const int gap  = param_loop_gap_ms.load();
                    char seq[224];
                    snprintf(seq, sizeof(seq),
                             u8"一轮时序：按下 → 保持 %d~%dms → 松开 → 等 0~%dms → 间隔 %dms",
                             base, base + pjit, rjit, gap);

                    ImGui::Dummy(ImVec2(0, 3));
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(seq);
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }

                // 恢复默认
                {
                    ParamDefaults d;
                    { std::lock_guard<std::mutex> lk(g_param_defaults_mutex); d = g_param_defaults; }

                    ImGui::Dummy(ImVec2(0, 4));
                    if (ghost_button(u8"恢复默认参数", ImVec2(0, 30))) {
                        param_press_base_ms    .store(d.press_base_ms);
                        param_press_jitter_ms  .store(d.press_jitter_ms);
                        param_release_jitter_ms.store(d.release_jitter_ms);
                        param_loop_gap_ms      .store(d.loop_gap_ms);
                        param_clicks_per_rest  .store(d.clicks_per_rest);
                        param_rest_seconds     .store(d.rest_seconds);
                        add_log(u8"[参数] 已恢复默认值", LogLevel::BEHAVIOR);
                    }
                    if (ImGui::IsItemHovered()) {
                        char tip[224];
                        snprintf(tip, sizeof(tip),
                                 u8"默认：按住 %d / 按住随机 %d / 松开随机 %d / 间隔 %d ms",
                                 d.press_base_ms, d.press_jitter_ms,
                                 d.release_jitter_ms, d.loop_gap_ms);
                        ImGui::SetTooltip("%s", tip);
                    }
                }

                card.end();
            } else {
                // ======== 自定义模式参数：动作序列编辑器 ========
                CardScope card;
                card.begin(full_w);
                section_title(u8"动作序列", C_GREEN, u8"走完一轮自动重头循环");

                {
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(u8"点「按键」格后按住目标键即可录制；"
                                           u8"后三列＝按住/按住随机/松开随机(ms)");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 2));
                }

                CustomProfile prof;
                { std::lock_guard<std::mutex> lk(g_custom_mutex); prof = g_custom_profile; }

                bool changed   = false;
                int  remove_at = -1;

                // 本帧绘制过录制器（用于上面的自动释放判断）
                s_rec_drawn = true;

                if (prof.count == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_AMBER));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(u8"还没有动作，点下面的「添加动作」开始配置");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 2));
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(u8"注意：空序列下启动不会产生任何点击");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 2));
                }

                for (int i = 0; i < prof.count; ++i) {
                    ImGui::PushID(i);
                    CustomStep& st = prof.steps[i];
                    const bool rec_this = (s_rec_row == i);

                    // 序号（固定两位宽，保证各行按键格左边缘对齐）
                    ImGui::AlignTextToFramePadding();
                    {
                        char no[8];
                        snprintf(no, sizeof(no), "%2d", i + 1);
                        ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                        ImGui::TextUnformatted(no);
                        ImGui::PopStyleColor();
                    }
                    ImGui::SameLine();

                    // 按键格 = 录制按钮
                    {
                        char kname[96];
                        action_step_label(st, kname, sizeof(kname));
                        char label[128];
                        if (rec_this) snprintf(label, sizeof(label), u8"按键…##key");
                        else          snprintf(label, sizeof(label), "%s##key", kname);

                        if (rec_this) {
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
                            ImGui::PushStyleColor(ImGuiCol_Button,        col4(C_AMBER_L));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(C_AMBER_L));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col4(C_AMBER_L));
                            ImGui::PushStyleColor(ImGuiCol_Text,          col4(C_AMBER));
                        }
                        if (ImGui::Button(label, ImVec2(112.0f, 0.0f))) {
                            s_rec_row          = i;
                            s_rec_wait_release = true;   // 先等这一下左键松开
                            s_rec_holding      = false;
                            s_rec_buf.clear();
                            s_rec_t0           = std::chrono::steady_clock::now();
                            g_action_key_recording.store(true);
                        }
                        if (rec_this) {
                            ImGui::PopStyleColor(4);
                            ImGui::PopStyleVar();
                        }
                        if (ImGui::IsItemHovered() && !rec_this)
                            ImGui::SetTooltip(u8"点击后按住目标键即可录制；可多个键、可带 Ctrl/Shift/Alt（Esc 取消）");
                    }

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(54.0f);
                    if (ImGui::DragInt("##hold", &st.hold_ms, 1.0f, 1, 60000, "%d",
                                       ImGuiSliderFlags_AlwaysClamp)) changed = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"按住时长 (ms)");

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(50.0f);
                    if (ImGui::DragInt("##hjit", &st.hold_jitter_ms, 1.0f, 0, 60000, "%d",
                                       ImGuiSliderFlags_AlwaysClamp)) changed = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"按住随机 (ms)，在上面时长上再加 0~该值");

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(50.0f);
                    if (ImGui::DragInt("##rjit", &st.release_jitter_ms, 1.0f, 0, 60000, "%d",
                                       ImGuiSliderFlags_AlwaysClamp)) changed = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"松开随机 (ms)，松开后再随机等 0~该值");

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button,        col4(C_CARD));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(253, 235, 235, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(250, 220, 220, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text,          col4(C_RED));
                    if (ImGui::Button(u8"×##del", ImVec2(26.0f, 0.0f))) remove_at = i;
                    ImGui::PopStyleColor(4);

                    ImGui::PopID();
                }

                // ---- 按键录制捕获 ----
                if (s_rec_row >= prof.count) {          // 目标行已被删掉
                    s_rec_row = -1;
                    s_rec_wait_release = false;
                    g_action_key_recording.store(false);
                }
                if (s_rec_row >= 0) {
                    const bool timed_out =
                        std::chrono::steady_clock::now() - s_rec_t0 >
                        std::chrono::seconds(15);
                    if (timed_out) {
                        // 挂太久会把热键一直屏蔽掉，超时自动放弃
                        s_rec_row = -1;
                        s_rec_wait_release = false;
                        s_rec_holding = false;
                        s_rec_buf.clear();
                        g_action_key_recording.store(false);
                        add_log(u8"[自定义] 录制超时已取消（15 秒内未按键）", LogLevel::WARN);
                    } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        s_rec_row = -1;
                        s_rec_wait_release = false;
                        s_rec_holding = false;
                        s_rec_buf.clear();
                        g_action_key_recording.store(false);
                        add_log(u8"[自定义] 已取消按键录制", LogLevel::INFO);
                    } else if (s_rec_wait_release) {
                        // 等这一下点击的左键彻底松开，否则会把「点录制按钮」录成鼠标左键
                        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) s_rec_wait_release = false;
                    } else {
                        // 逐帧累积，直到所有键都松开才确认（这样 S+D 这种先后按下的组合也能录全）
                        const bool any_down = accumulate_step_keys(s_rec_buf);

                        if (any_down) {
                            if (s_rec_buf.has_key()) s_rec_holding = true;
                            s_rec_t0 = std::chrono::steady_clock::now(); // 有按键就重置超时
                        } else if (s_rec_holding) {
                            // ---- 全部松开 → 提交 ----
                            uint8_t list[4];
                            int n = 0;
                            if (s_rec_buf.has_mouse) list[n++] = s_rec_buf.mouse_vk;
                            for (int i = 0; i < s_rec_buf.key_count && n < 4; ++i)
                                list[n++] = s_rec_buf.keys[i];

                            if (n > 0) {
                                CustomStep& dst = prof.steps[s_rec_row];
                                dst.vk   = list[0];
                                dst.mods = s_rec_buf.mods;
                                for (int i = 0; i < 3; ++i)
                                    dst.vkeys[i] = (i + 1 < n) ? list[i + 1] : 0;
                                changed = true;

                                char nm[64];
                                action_step_label(dst, nm, sizeof(nm));
                                add_log(std::string(u8"[自定义] 第 ") + std::to_string(s_rec_row + 1) +
                                        u8" 条按键 → " + nm, LogLevel::BEHAVIOR);
                            }

                            s_rec_row = -1;
                            s_rec_wait_release = false;
                            s_rec_holding = false;
                            s_rec_buf.clear();
                            g_action_key_recording.store(false);
                        }
                    }
                }

                // 录制中提示（实时回显已捕获的键）
                if (s_rec_row >= 0) {
                    char cur[80];
                    step_capture_label(s_rec_buf, cur, sizeof(cur));
                    ImGui::Dummy(ImVec2(0, 2));
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_AMBER));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::Text(u8"正在录制第 %d 条：%s", s_rec_row + 1, cur);
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(u8"按住要用的键（可多个），全部松开自动确认；Esc 取消");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }

                // 删除放到循环外处理，避免边遍历边改索引
                if (remove_at >= 0) {
                    for (int k = remove_at; k < prof.count - 1; ++k)
                        prof.steps[k] = prof.steps[k + 1];
                    prof.count--;
                    prof.steps[prof.count] = CustomStep{};
                    changed = true;
                    add_log(u8"[自定义] 已删除一条动作", LogLevel::BEHAVIOR);
                }

                // ---- 添加 / 清空 ----
                ImGui::Dummy(ImVec2(0, 2));
                if (prof.count < MAX_CUSTOM_STEPS) {
                    if (solid_button(u8"＋ 添加动作", ImVec2(0, 30),
                                     C_GREEN, C_GREEN_H, C_GREEN_D, false)) {
                        prof.steps[prof.count] = CustomStep{};
                        prof.count++;
                        changed = true;
                        add_log(u8"[自定义] 已添加一条动作", LogLevel::BEHAVIOR);
                    }
                    ImGui::SameLine();
                }
                if (prof.count > 0) {
                    if (ghost_button(u8"清空全部", ImVec2(0, 30))) {
                        prof = CustomProfile{};
                        changed = true;
                        add_log(u8"[自定义] 已清空动作序列", LogLevel::BEHAVIOR);
                    }
                }

                // ---- 循环间隔 ----
                ImGui::Dummy(ImVec2(0, 4));
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(u8"循环间隔");
                    ImGui::SameLine();
                    align_right(106.0f);
                    ImGui::SetNextItemWidth(106.0f);
                    int gap = prof.loop_gap_ms;
                    if (ImGui::InputInt("##c_gap", &gap, 10, 50)) {
                        if (gap < 0)      gap = 0;
                        if (gap > 600000) gap = 600000;
                        prof.loop_gap_ms = gap;
                        changed = true;
                        add_log(u8"[参数] 自定义循环间隔 → " + std::to_string(gap) + " ms",
                                LogLevel::BEHAVIOR);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"一轮动作全部走完后的等待时间 (ms)");
                }

                // ---- 一轮耗时预览 ----
                {
                    int sum_min = 0, sum_max = 0;
                    for (int i = 0; i < prof.count; ++i) {
                        const CustomStep& s = prof.steps[i];
                        sum_min += s.hold_ms;
                        sum_max += s.hold_ms + s.hold_jitter_ms + s.release_jitter_ms;
                    }
                    char tip[224];
                    snprintf(tip, sizeof(tip),
                             u8"共 %d 条动作，一轮约 %d~%dms，再加循环间隔 %dms（最多 %d 条）",
                             prof.count, sum_min, sum_max, prof.loop_gap_ms, MAX_CUSTOM_STEPS);
                    ImGui::Dummy(ImVec2(0, 3));
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(tip);
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }

                if (changed) {
                    std::lock_guard<std::mutex> lk(g_custom_mutex);
                    g_custom_profile = prof;
                }

                card.end();
            }

            // ---- 休息策略 ----
            {
                CardScope card;
                card.begin(full_w);
                section_title(u8"休息策略", C_BLUE, u8"两种模式通用");

                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(u8"点击多少次后休息");
                    ImGui::SameLine();
                    align_right(106.0f);
                    ImGui::SetNextItemWidth(106.0f);
                    int per_rest = param_clicks_per_rest.load();
                    if (ImGui::InputInt("##q_rest", &per_rest, 1, 10)) {
                        if (per_rest < 0) per_rest = 0;
                        if (per_rest > 100000) per_rest = 100000;
                        param_clicks_per_rest.store(per_rest);
                        add_log(u8"[参数] 每轮点击数 → " + std::to_string(per_rest), LogLevel::BEHAVIOR);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"设为 0 表示不休息，持续点击");
                }

                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(u8"休息时长 (秒)");
                    ImGui::SameLine();
                    align_right(106.0f);
                    ImGui::SetNextItemWidth(106.0f);
                    int rest_s = param_rest_seconds.load();
                    if (ImGui::InputInt("##q_rest_s", &rest_s, 1, 5)) {
                        if (rest_s < 1)    rest_s = 1;
                        if (rest_s > 3600) rest_s = 3600;
                        param_rest_seconds.store(rest_s);
                        add_log(u8"[参数] 休息时长 → " + std::to_string(rest_s) + " s", LogLevel::BEHAVIOR);
                    }
                }

                card.end();
            }

            ImGui::EndTabItem();
        }

        // ========================= 日志 =========================
        if (ImGui::BeginTabItem(u8"日志")) {
            static bool show_info     = true;
            static bool show_behavior = true;
            static bool show_warn     = true;
            static bool show_err      = true;
            static bool auto_scroll   = true;

            ImGui::Checkbox("INFO",   &show_info);     ImGui::SameLine();
            ImGui::Checkbox(u8"行为", &show_behavior); ImGui::SameLine();
            ImGui::Checkbox("WARN",   &show_warn);     ImGui::SameLine();
            ImGui::Checkbox("ERROR",  &show_err);      ImGui::SameLine();
            ImGui::Checkbox(u8"自动滚动", &auto_scroll);

            // 5 个勾选框已经占满一行，清空按钮换到下一行右对齐，否则会盖住「自动滚动」
            float clear_w = ImGui::CalcTextSize(u8"清空").x + ImGui::GetStyle().FramePadding.x * 2 + 16.0f;
            ImGui::NewLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - clear_w);
            if (ghost_button(u8"清空", ImVec2(clear_w, 0))) {
                std::lock_guard<std::mutex> g(g_log_mutex);
                g_log_lines.clear();
            }

            ImGui::Dummy(ImVec2(0, 2));

            // 先在锁内拷一份快照，再在锁外渲染：
            // 避免整帧（上千条文本）都占着日志锁，卡住连点线程写日志
            std::vector<LogEntry> snapshot;
            {
                std::lock_guard<std::mutex> g(g_log_mutex);
                snapshot = g_log_lines;
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(C_CARD));
            ImGui::BeginChild("##log_scroll", ImVec2(0, 0), true);
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
                for (const auto& entry : snapshot) {
                    switch (entry.level) {
                        case LogLevel::INFO:
                            if (!show_info) continue;
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.39f, 0.45f, 1.0f));
                            break;
                        case LogLevel::BEHAVIOR:
                            if (!show_behavior) continue;
                            ImGui::PushStyleColor(ImGuiCol_Text, col4(C_BLUE));
                            break;
                        case LogLevel::WARN:
                            if (!show_warn) continue;
                            ImGui::PushStyleColor(ImGuiCol_Text, col4(C_AMBER));
                            break;
                        case LogLevel::ERR:
                            if (!show_err) continue;
                            ImGui::PushStyleColor(ImGuiCol_Text, col4(C_RED));
                            break;
                    }
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w());
                    ImGui::TextUnformatted(entry.text.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
                ImGui::PopStyleVar();
                // 勾选"自动滚动"就始终贴底；取消后可以自由回滚查看历史
                if (auto_scroll) ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::EndTabItem();
        }

        // ========================= 测试 =========================
        if (ImGui::BeginTabItem(u8"测试")) {
            ImGui::Dummy(ImVec2(0, 2));

            const float full_w = ImGui::GetContentRegionAvail().x;

            // ---- 运行环境自检 ----
            {
                CardScope card;
                card.begin(full_w);
                section_title(u8"运行环境自检", C_BLUE);

                struct Check { const char* label; bool ok; };
                Check checks[] = {
                    { u8"Interception 驱动上下文", g_context != nullptr },
                    { u8"活跃鼠标已识别",          active_mouse_id.load() != 0 },
                    { u8"触发热键已配置",          [&]{
                        HotkeyConfig hk;
                        { std::lock_guard<std::mutex> lk(g_hotkey_mutex); hk = g_hotkey; }
                        return has_any(hk);
                    }() },
                };

                ImDrawList* cdl = ImGui::GetWindowDrawList();
                for (const auto& c : checks) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    const float lh = ImGui::GetTextLineHeight();
                    ImU32 col = c.ok ? C_OK : C_RED;
                    cdl->AddCircleFilled(ImVec2(p.x + 4.0f, p.y + lh * 0.5f), 7.0f,
                                         (col & 0x00FFFFFFu) | (40u << 24));
                    cdl->AddCircleFilled(ImVec2(p.x + 4.0f, p.y + lh * 0.5f), 4.0f, col);
                    cdl->AddText(ImVec2(p.x + 17.0f, p.y), C_TEXT, c.label);

                    const char* tag = c.ok ? u8"正常" : u8"异常";
                    ImVec2 ts = ImGui::CalcTextSize(tag);
                    cdl->AddText(ImVec2(p.x + card_w() - ts.x, p.y), col, tag);
                    ImGui::Dummy(ImVec2(card_w(), lh));
                }
                card.end();
            }

            // ---- 按键激活检测 ----
            {
                CardScope card;
                card.begin(full_w);

                static bool key_scanning = false;
                section_title(u8"按键检测", C_GREEN, key_scanning ? u8"检测中" : u8"未开始");

                if (!key_scanning) {
                    if (solid_button(u8"开始检测", ImVec2(0, 30),
                                     C_GREEN, C_GREEN_H, C_GREEN_D, false)) key_scanning = true;
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                    ImGui::TextUnformatted(u8"点击后实时显示当前按下的按键");
                    ImGui::PopStyleColor();
                } else {
                    if (ghost_button(u8"停止检测", ImVec2(0, 30))) key_scanning = false;
                }

                if (key_scanning) {
                    ImGui::Dummy(ImVec2(0, 4));

                    std::vector<std::string> active_keys;

                    if (GetAsyncKeyState(VK_LBUTTON)  & 0x8000) active_keys.push_back(u8"左键");
                    if (GetAsyncKeyState(VK_RBUTTON)  & 0x8000) active_keys.push_back(u8"右键");
                    if (GetAsyncKeyState(VK_MBUTTON)  & 0x8000) active_keys.push_back(u8"中键");
                    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) active_keys.push_back("Button4");
                    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) active_keys.push_back("Button5");

                    if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) active_keys.push_back("L-Ctrl");
                    if (GetAsyncKeyState(VK_RCONTROL) & 0x8000) active_keys.push_back("R-Ctrl");
                    if (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) active_keys.push_back("L-Shift");
                    if (GetAsyncKeyState(VK_RSHIFT)   & 0x8000) active_keys.push_back("R-Shift");
                    if (GetAsyncKeyState(VK_LMENU)    & 0x8000) active_keys.push_back("L-Alt");
                    if (GetAsyncKeyState(VK_RMENU)    & 0x8000) active_keys.push_back("R-Alt");

                    if (GetAsyncKeyState(VK_RETURN) & 0x8000) active_keys.push_back("Enter");
                    if (GetAsyncKeyState(VK_SPACE)  & 0x8000) active_keys.push_back("Space");
                    if (GetAsyncKeyState(VK_TAB)    & 0x8000) active_keys.push_back("Tab");
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) active_keys.push_back("Esc");
                    if (GetAsyncKeyState(VK_BACK)   & 0x8000) active_keys.push_back("Backspace");
                    if (GetAsyncKeyState(VK_DELETE) & 0x8000) active_keys.push_back("Delete");
                    if (GetAsyncKeyState(VK_INSERT) & 0x8000) active_keys.push_back("Insert");
                    if (GetAsyncKeyState(VK_HOME)   & 0x8000) active_keys.push_back("Home");
                    if (GetAsyncKeyState(VK_END)    & 0x8000) active_keys.push_back("End");
                    if (GetAsyncKeyState(VK_PRIOR)  & 0x8000) active_keys.push_back("PgUp");
                    if (GetAsyncKeyState(VK_NEXT)   & 0x8000) active_keys.push_back("PgDn");
                    if (GetAsyncKeyState(VK_UP)     & 0x8000) active_keys.push_back(u8"↑");
                    if (GetAsyncKeyState(VK_DOWN)   & 0x8000) active_keys.push_back(u8"↓");
                    if (GetAsyncKeyState(VK_LEFT)   & 0x8000) active_keys.push_back(u8"←");
                    if (GetAsyncKeyState(VK_RIGHT)  & 0x8000) active_keys.push_back(u8"→");

                    for (int vk = VK_F1; vk <= VK_F24; ++vk) {
                        if (GetAsyncKeyState(vk) & 0x8000) {
                            char buf[8]; snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
                            active_keys.push_back(buf);
                        }
                    }

                    for (int vk = '0'; vk <= 'Z'; ++vk) {
                        // 跳过 0x3A~0x40 这段标点，否则 ": ; < = > ? @" 会冒出来
                        if (vk >= 0x3A && vk <= 0x40) continue;
                        if (GetAsyncKeyState(vk) & 0x8000) {
                            char buf[2] = { (char)vk, 0 };
                            active_keys.push_back(buf);
                        }
                    }

                    static const struct { int vk; const char* name; } numpad[] = {
                        {VK_NUMPAD0,"Num0"},{VK_NUMPAD1,"Num1"},{VK_NUMPAD2,"Num2"},
                        {VK_NUMPAD3,"Num3"},{VK_NUMPAD4,"Num4"},{VK_NUMPAD5,"Num5"},
                        {VK_NUMPAD6,"Num6"},{VK_NUMPAD7,"Num7"},{VK_NUMPAD8,"Num8"},
                        {VK_NUMPAD9,"Num9"},{VK_MULTIPLY,"Num*"},{VK_ADD,"Num+"},
                        {VK_SUBTRACT,"Num-"},{VK_DECIMAL,"Num."},{VK_DIVIDE,"Num/"},
                    };
                    for (const auto& n : numpad) {
                        if (GetAsyncKeyState(n.vk) & 0x8000)
                            active_keys.push_back(n.name);
                    }

                    // 显示框
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    const float bh = 64.0f;
                    const float bw = card_w();
                    ImDrawList* kdl = ImGui::GetWindowDrawList();
                    kdl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), C_TRACK, 9.0f);
                    kdl->AddRect(p, ImVec2(p.x + bw, p.y + bh), C_TRACK_D, 9.0f, 0, 1.0f);

                    ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f, p.y + 8.0f));
                    if (active_keys.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
                        ImGui::TextUnformatted(u8"（无按键按下）");
                        ImGui::PopStyleColor();
                    } else {
                        std::string line;
                        for (size_t i = 0; i < active_keys.size(); ++i) {
                            if (i > 0) line += "  ";
                            line += active_keys[i];
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, col4(C_BLUE));
                        ImGui::PushTextWrapPos(p.x + bw - 10.0f);
                        ImGui::TextWrapped("%s", line.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                    }
                    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bh));
                    ImGui::Dummy(ImVec2(bw, 0));
                }

                card.end();
            }

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // ---------------- 底部操作栏 ----------------
    {
        const float gap = 10.0f;
        const float bw  = (inner_w - gap) * 0.5f;
        const ImVec2 bs(bw, 46.0f);
        const float bar_y = wsz.y - bottom_h + 9.0f;

        ImGui::SetCursorPos(ImVec2(PAD_X, bar_y));

        // 悬停提示用：当前触发键（省得回首页确认热键是什么）
        HotkeyConfig trig;
        { std::lock_guard<std::mutex> lk(g_hotkey_mutex); trig = g_hotkey; }
        const std::string trig_tip = std::string(u8"热键 ") + hotkey_str(trig) + u8" 也能开始 / 停止";

        if (solid_button(u8"开 始", bs, C_GREEN, C_GREEN_H, C_GREEN_D, running)) {
            gui_start_macro();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", trig_tip.c_str());

        ImGui::SameLine(0.0f, gap);
        if (solid_button(u8"停 止", bs, C_RED, C_RED_H, C_RED_D, !running)) {
            gui_stop_macro();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", trig_tip.c_str());

        // 版权
        const char* copyright = u8"© 2026 萌萌_Rock. All rights reserved.";
        const ImVec2 cs = ImGui::CalcTextSize(copyright);
        ImGui::SetCursorPos(ImVec2((wsz.x - cs.x) * 0.5f, bar_y + 46.0f + 7.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, col4(C_TEXT_FAINT));
        ImGui::TextUnformatted(copyright);
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

} // namespace app
