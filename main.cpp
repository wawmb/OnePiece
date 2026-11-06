#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <thread>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "core/app_state.h"
#include "core/clicker.h"
#include "platform/config.h"
#include "platform/d3d_window.h"
#include "ui/gui.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // ---- Interception 初始化 ----
    app::g_context = interception_create_context();
    if (!app::g_context) {
        MessageBoxW(nullptr,
                    L"无法创建 Interception 上下文。\n请确认:\n"
                    L"1. 已通过 install-interception.exe /install 安装内核驱动\n"
                    L"2. 系统已重启\n"
                    L"3. 程序以管理员身份运行",
                    L"OnePiece - 启动失败", MB_ICONERROR);
        return 1;
    }
    interception_set_filter(app::g_context, interception_is_mouse, INTERCEPTION_FILTER_MOUSE_ALL);

    app::load_config();

    // ---- 启动后台线程 ----
    std::thread(app::macro_worker).detach();
    std::thread(app::interception_thread).detach();
    std::thread(app::hotkey_poller).detach();
    app::add_log("OnePiece v2.0 已启动", app::LogLevel::INFO);

    // ---- 创建窗口 ----
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, app::wnd_proc, 0L, 0L,
                       hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"OnePiece", nullptr };
    RegisterClassExW(&wc);

    // 期望尺寸；若超出当前工作区（高 DPI / 小屏）则收窄，避免按钮被挤出屏幕
    int win_w = 480, win_h = 820;
    {
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            const int max_w = (wa.right - wa.left) - 24;
            const int max_h = (wa.bottom - wa.top) - 24;
            if (win_w > max_w) win_w = max_w;
            if (win_h > max_h) win_h = max_h;
            if (win_w < 420) win_w = 420;
            if (win_h < 560) win_h = 560;
        }
    }

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"OnePiece v2.0",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                              100, 60, win_w, win_h,
                              nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"窗口创建失败。", L"OnePiece - 启动失败", MB_ICONERROR);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    if (!app::create_device_d3d(hwnd)) {
        app::cleanup_device_d3d();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    app::set_gui_hwnd(hwnd);
    app::set_normal_window_size(win_w, win_h);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // 中文字体
    ImFontConfig fc;
    fc.OversampleH = 2;
    fc.OversampleV = 2;
    const char* font_path = R"(C:\Windows\Fonts\msyh.ttc)";
    DWORD attr = GetFileAttributesA(font_path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        font_path = R"(C:\Windows\Fonts\simhei.ttf)"; // 备选
    }

    // 常用中文范围之外，再补上箭头（→ ← ↑ ↓ ↔）与几何符号，
    // 否则界面上的「普通 ↔ 自定义」「按下 → 保持」会显示成问号。
    static ImVector<ImWchar> glyph_ranges;
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        static const ImWchar extra[] = { 0x2190, 0x21FF, 0x25A0, 0x25FF, 0 };
        builder.AddRanges(extra);
        builder.BuildRanges(&glyph_ranges);
    }
    ImFont* ui_font = io.Fonts->AddFontFromFileTTF(font_path, 17.0f, &fc, glyph_ranges.Data);
    if (!ui_font) {
        // 极少数精简系统既没有 msyh.ttc 也没有 simhei.ttf。
        // 此时退回内置字体，界面能开但中文会是方块 —— 明确提示，别让人以为程序坏了。
        io.Fonts->AddFontDefault();
        MessageBoxW(nullptr,
                    L"未找到中文字体（msyh.ttc / simhei.ttf），\n界面中文将无法正常显示。",
                    L"OnePiece - 字体缺失", MB_ICONWARNING);
    }

    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding    = ImVec2(12, 10);
    style.FramePadding     = ImVec2(9, 6);
    style.ItemSpacing      = ImVec2(9, 7);
    style.ItemInnerSpacing = ImVec2(6, 5);
    style.FrameRounding    = 7.0f;
    style.GrabRounding     = 7.0f;
    style.TabRounding      = 7.0f;
    style.ScrollbarRounding= 8.0f;
    style.ScrollbarSize    = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 0.0f;
    style.ChildRounding    = 8.0f;
    style.CellPadding      = ImVec2(6, 5);

    // 统一浅色控件配色（白底 + 细边框 + 蓝色强调）
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.09f, 0.11f, 0.16f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.58f, 0.64f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.973f, 0.980f, 0.988f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);
    c[ImGuiCol_Border]                = ImVec4(0.894f, 0.918f, 0.949f, 1.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.953f, 0.965f, 0.980f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.925f, 0.945f, 0.969f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.898f, 0.925f, 0.961f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.953f, 0.965f, 0.980f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.925f, 0.945f, 0.969f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.894f, 0.918f, 0.949f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.933f, 0.957f, 0.996f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.902f, 0.937f, 0.996f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.867f, 0.914f, 0.992f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.894f, 0.918f, 0.949f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.231f, 0.431f, 0.784f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.161f, 0.329f, 0.659f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.231f, 0.431f, 0.784f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.925f, 0.945f, 0.969f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]   = ImVec4(0.231f, 0.431f, 0.784f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.800f, 0.827f, 0.863f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.710f, 0.749f, 0.800f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.620f, 0.667f, 0.729f, 1.00f);
    c[ImGuiCol_NavCursor]             = ImVec4(0.231f, 0.431f, 0.784f, 1.00f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(app::d3d_device(), app::d3d_context());

    // ---- 主循环 ----
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (app::g_resize_width != 0 && app::g_resize_height != 0) {
            app::cleanup_render_target();
            app::d3d_swap_chain()->ResizeBuffers(0, app::g_resize_width, app::g_resize_height,
                                                  DXGI_FORMAT_UNKNOWN, 0);
            app::g_resize_width = app::g_resize_height = 0;
            app::create_render_target();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app::render_gui();

        ImGui::Render();
        constexpr float clear_color[4] = { 0.96f, 0.96f, 0.97f, 1.0f };
        ID3D11RenderTargetView* rtv = app::d3d_main_rtv();
        app::d3d_context()->OMSetRenderTargets(1, &rtv, nullptr);
        app::d3d_context()->ClearRenderTargetView(rtv, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        app::d3d_swap_chain()->Present(1, 0);
    }

    app::save_config();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    app::cleanup_device_d3d();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    interception_destroy_context(app::g_context);
    return 0;
}
