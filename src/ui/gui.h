#pragma once

#include <d3d11.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace app {

void render_gui();
void set_gui_hwnd(HWND hwnd);
// 记录主窗口的实际尺寸（启动时按工作区裁剪过），迷你模式返回时用它还原
void set_normal_window_size(int w, int h);

} // namespace app
