# OnePiece v2.0

基于 Interception 内核驱动的 Windows 鼠标连点工具，点击注入在驱动层完成。界面使用 ImGui + DirectX 11 渲染。

- **两种模式**：普通（左键固定频率连点）/ 自定义（最多 16 条按键动作，支持组合键，循环执行）
- **热键**：触发键默认 Button4、模式切换键默认 Button5，都用「按住组合 → 全部松开确认」录制
- **迷你小窗**：300×186 悬浮卡片，看状态、看数值、一键启停，可置顶可拖动
- **配置**：退出时写入 exe 同目录 `config.ini`，手改数值会自动收敛到合法区间

---

## 运行前

Windows 10 / 11（64 位），**以管理员身份运行**。

需先单独安装 [Interception 驱动](https://github.com/oblitum/Interception)（本仓库不含安装程序）：下载发布包取出 `install-interception.exe`，管理员命令行执行 `install-interception.exe /install`，然后**重启计算机**。卸载用 `/uninstall` 并重启。

---

## 项目结构

```
OnePiece/
├─ main.cpp                        程序入口
├─ app.rc                          图标 + 版本资源
├─ CMakeLists.txt                  构建脚本
├─ .github/workflows/build.yml     自动构建
├─ LICENSE / README.md
│
├─ src/
│  ├─ core/                        全局状态 + 后台线程
│  │  ├─ app_state.h / .cpp
│  │  └─ clicker.h    / .cpp
│  │
│  ├─ platform/                    Win32 与文件读写
│  │  ├─ config.h     / .cpp       config.ini
│  │  └─ d3d_window.h / .cpp       D3D11 窗口
│  │
│  └─ ui/                          全部 ImGui 界面
│     ├─ gui.h / gui.cpp
│     └─ assets/cover.ico
│
└─ external/                       第三方源码，原样内置
   ├─ imgui/                      Dear ImGui 1.92.8（含 backends/）
   └─ interception/               interception.h / .lib / .dll
```

分层是单向的：`ui` → `core` → `platform` → Win32，`platform` 不反向依赖上层。

---

## 编译

需要 CMake 3.20+ 与 Ninja，工具链二选一：MinGW-w64 / MSYS2 UCRT64（需 `windres`）或 MSVC（需 `rc.exe`，x64）。**必须用 Ninja 单配置生成器**——工程在 configure 阶段把 `interception.lib` / `.dll` 放进 `build/`，exe 也必须落在同一目录；Visual Studio 生成器会把 exe 放到 `build/Release/`，dll 与 exe 分家。用错生成器时 CMake 会在配置阶段直接报错。

**MSYS2 UCRT64**（日常使用）：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
                   mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja
export PATH="/c/msys64/ucrt64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

直接打开开始菜单的 **MSYS2 UCRT64** 终端可省掉 `export` 那步。**MSVC** 则打开 **x64 Native Tools Command Prompt for VS 2022**，在工程根目录执行同样的两条 cmake 命令。两套工具链的差异由 `CMakeLists.txt` 自动处理。

产物为 `build/OnePiece.exe`；`interception.dll` 在 configure 阶段就已复制进 `build/`，**exe 与 dll 必须放在一起**。

常见报错：`cmake: command not found` → 没进 UCRT64 环境；`cannot open output file ... Permission denied` → 上次编译的 exe 还在运行；`cannot find -l./interception.dll` → 删掉 `build/` 重新 configure，别在两套工具链之间复用同一个 build 目录。

---

## GitHub Actions

`.github/workflows/build.yml` 在 push / PR / `v*` 标签 / 手动触发时，用 MinGW 与 MSVC 双工具链并行编译，上传 `OnePiece-mingw` / `OnePiece-msvc` 产物。**只有打了 `v*` 标签且两条工具链都编译成功**，才会额外创建带两份 zip 的 Release；普通 push / PR / 手动触发只出 artifact，不会建 Release。

在 **Actions** 页最新一次运行底部的 **Artifacts** 下载解压即可。发版：

```bash
git tag v2.0
git push origin v2.0
```

标签对应的那次构建失败后，GitHub 不会因代码修好而自动重跑：把标签移到修复提交再强推（`git tag -f v2.0 && git push -f origin v2.0`），或换个新版本号发。

`external/interception/` 的 dll 与 lib 不是构建产物，必须一并提交，否则 CI 会因缺文件在配置阶段失败。

---

## 快速上手

1. 装好驱动并重启，右键 `OnePiece.exe` → **以管理员身份运行**。
2. 先在窗口外移动一次鼠标，驱动状态条变绿并显示「设备 ID x」。
3. 点底部**开始**或按触发键（默认 Button4）即开始连点。

---

## 界面

浅色卡片式布局，默认 480×820；屏幕工作区放不下时按宽高分别裁剪（下限 420×560）。顶部栏是运行状态（运行中绿点会呼吸 / 已停止红点）、置顶、迷你三个控件；下方统计区显示累计点击、运行时长（`HH:MM:SS`）和鼠标驱动状态。

### 迷你小窗

点**迷你**收成 300×186 悬浮小窗：顶部一条状态色条（运行绿 / 停止红）、标题行（状态 + 模式标签 + 展开）、两列数值（累计点击 / 运行时长）、底部一个随状态切换的「开 始 / 停 止」按钮。

- **按住标题行拖动**移动位置；**双击标题行**或点**展开**回到主界面（单击不展开，避免和拖动抢手势）
- 小窗只做状态与启停，改参数、录热键、编排动作要回主界面

---

## 首页

**模式**：普通（左键连点，四个时间参数可调）/ 自定义（自编动作序列）。切换时若正在连点会先停止。

**热键**：触发键（开始 / 停止，默认 Button4）、模式切换键（普通 ↔ 自定义，默认 Button5）。录制：点**录制新组合** → 按住目标组合（Ctrl / Shift / Alt + 鼠标中键 / 侧键 / F1–F24 / 字母数字，最多 4 个非修饰键）→ **全部松开**自动确认；**恢复默认**重置。鼠标侧键触发会吞掉该事件、不传给其他应用；纯键盘热键无法拦截，建议用含侧键的组合。

**普通模式参数**（默认值）：按住基础 50ms、按住随机 10ms、松开随机 20ms、循环间隔 500ms，时序为 `按下 → 保持 → 松开 → 随机等待 → 循环间隔 → 重复`。

**休息策略**（两模式通用）：每 N 次点击后休息 M 秒，默认 `0 / 3`（0 = 不休息）。`[defaults]` 段可手改，决定「恢复默认参数」按钮恢复到的目标值。

---

## 自定义模式

若干条「动作」按列表顺序执行，走完一轮等**循环间隔**（默认 100ms）后从第 1 条重来。每条动作含：按键（点格子后按住目标键录制）、按住、按住随、松开随。最多 16 条，`Esc` 取消录制，行尾 `x` 删除，另有 `+ 添加动作` / `清空全部`。清空后重启仍是空的，此时点开始会提示「没有动作，启动后不会产生点击」。

出厂默认两条：左键 110 / 10 / 20、`R` 30 / 5 / 20。每次动作计入「累计点击」，所以自定义模式下这个数字是**动作次数**而非鼠标点击数。

**按键范围**：全键表扫描——鼠标左 / 右 / 中键与 Button4 / 5、字母数字、F1–F24、小键盘、方向键、控制键、符号键，修饰键区分左右，`Esc` 保留给「取消录制」。一条动作支持修饰键 + 最多 4 个普通键（如 `Ctrl+Shift+R`、`Shift+右键`），按住后全部松开才确认；执行时按 `修饰键 → 主键 → 额外键` 按下、逆序松开，中途停止也会把键全部放开。键盘设备未识别时降级为 `SendInput`。

---

## 日志页 / 测试页

日志页按 INFO / 行为 / WARN / ERROR 分级显示，**清空**清屏，默认勾选**自动滚动**（取消后可翻历史）。测试页做三项运行环境自检，并可**开始检测**实时显示当前按下的按键，用来确认组合键是否被正确识别。

---

## 配置文件

退出时写入 exe 同目录 `config.ini`，下次启动自动读取；直接编辑该文件也会生效。分四段：`[params]` 当前参数与模式、`[hotkey]` 触发键 / 模式切换键、`[defaults]` 恢复默认的目标值、`[custom]` 自定义动作序列。下面只列关键字段：

```ini
[params]
press_base_ms     = 50
press_jitter_ms   = 10
release_jitter_ms = 20
loop_gap_ms       = 500
clicks_per_rest   = 0
rest_seconds      = 3
mode              = 0        ; 0=普通 1=自定义

[hotkey]
mouse_btn4      = 1         ; Button4 触发开始/停止
mode_mouse_btn5 = 1         ; Button5 切换模式

[custom]
count    = 2
loop_gap = 100
step0_vk             = 0    ; 0=鼠标左键；键盘键用 VK 码（'R'=82）
step0_mods           = 0    ; 1=Ctrl 2=Shift 4=Alt，可相加
step0_hold           = 110
step0_hold_jitter    = 10
step0_release_jitter = 20
```

数值读取时会夹到合法区间，手填 `99999999` 或负数不会让节奏失控。`[custom] count = 0` 表示用户主动清空的空序列，会原样保留；删掉整个 `[custom]` 段才会回填出厂默认的两条动作。

---

## 常见问题

**Q：启动时弹出"无法创建 Interception 上下文"。**  
A：确认驱动已 `/install`、安装后已重启、且以管理员身份运行。

**Q：状态条是橙色，点击开始没反应。**  
A：在窗口外动一下鼠标让驱动识别设备；若从未移动就点开始，会降级使用 `MOUSE(0)` 并记一条警告日志。

**Q：热键触发后目标程序也收到了按键。**  
A：鼠标侧键触发会被吞掉，纯键盘热键无法拦截，建议用含鼠标侧键的组合。

**Q：程序关闭后鼠标 / 键盘失灵。**  
A：驱动异常退出偶尔会这样，重新以管理员身份启动再正常退出，或重启计算机。

**Q：迷你小窗点一下没展开？**  
A：单击只用于拖动，双击标题行或点右上角「展开」按钮。小窗不能改参数。

---

## 许可

© 2026 萌萌_Rock. All rights reserved.
