cmake_minimum_required(VERSION 3.20)

# RC 语言用于编译 app.rc（图标 + 版本信息）。
# MSVC 用 rc.exe，MinGW 用 windres，两者都由 CMake 自动定位。
project(OnePiece CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==========================================
# 0. 生成器限制
#    lib / dll 在配置阶段被复制进 build 根目录，exe 也必须落在同一目录，
#    运行期才能找到 interception.dll。多配置生成器（Visual Studio）会把
#    exe 放进 build/<Config>/，dll 与 exe 分家，报错信息又晦涩。
#    与其让人对着链接错误排查，不如在这里直接说清楚。
# ==========================================
if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR
            "本工程仅支持单配置生成器。请改用 Ninja：\n"
            "    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
            "（不要使用 Visual Studio 生成器）")
endif()

# ==========================================
# 1. 定义本地的路径
# ==========================================
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
set(INTERCEPTION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/interception")

# 前置检查：缺资源时给出可读的报错，而不是一堆链接错误
foreach(_f
        "${CMAKE_CURRENT_SOURCE_DIR}/app.rc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/assets/cover.ico"
        "${INTERCEPTION_DIR}/interception.lib"
        "${INTERCEPTION_DIR}/interception.dll")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "缺少必要文件: ${_f}")
    endif()
endforeach()

# ==========================================
# 2. 使用本地路径收集源码
# ==========================================
set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# 3. 包含头文件路径
include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${INTERCEPTION_DIR}
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

# 4. 应用源码(按职责拆分)
set(APP_SOURCES
        src/core/app_state.cpp
        src/core/clicker.cpp
        src/ui/gui.cpp
        src/platform/config.cpp
        src/platform/d3d_window.cpp
)

# WIN32 -> 不显示控制台窗口
# ==========================================
# 5. 资源编译（app.rc）
#    windres 是 ANSI 程序：CMake 把源目录以 UTF-8 写进 build.ninja，
#    若源码路径含中文（如「新建文件夹」），windres 会按 GBK 解码成乱码，
#    报 "can't popen ... Bad file descriptor"。
#    这里改为「工作目录 + 相对路径」调用，让命令行参数不出现中文。
#    MSVC 的 rc.exe 内部用 UTF-16，不受影响，直接用源文件。
#
#    -c 65001：app.rc 是 UTF-8 且含中文版权信息，必须显式告诉 windres
#    按 UTF-8 解码（rc.exe 那边靠 app.rc 里的 #pragma code_page 等效声明）。
#    改 app.rc 编码时这里要一起改，否则版本信息会变乱码。
# ==========================================
if(MINGW)
    set(RC_OBJ "${CMAKE_CURRENT_BINARY_DIR}/app_res.obj")
    file(RELATIVE_PATH RC_OBJ_REL "${CMAKE_CURRENT_SOURCE_DIR}" "${RC_OBJ}")
    add_custom_command(
            OUTPUT  "${RC_OBJ}"
            COMMAND "${CMAKE_RC_COMPILER}" -c 65001 -O coff --use-temp-file
                    app.rc "${RC_OBJ_REL}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/assets/cover.ico"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Compiling app.rc (windres)"
            VERBATIM
    )
    set(APP_RES_SOURCES "${RC_OBJ}")
else()
    set(APP_RES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

add_executable(OnePiece WIN32
        main.cpp
        ${APP_RES_SOURCES}
        ${APP_SOURCES}
        ${IMGUI_SOURCES}
)

# windows.h 默认会定义 min / max 宏，把 std::min(...) 展开成非法语法，
# MSVC 报 C2589（MinGW 恰好不触发，容易漏掉）。全局关掉这两个宏。
# 源码里没有裸 min()/max() 调用，关掉不会有任何副作用。
target_compile_definitions(OnePiece PRIVATE NOMINMAX)

# 工程名 = 产物名 = OnePiece。
# 刻意不用 AutoClicker 这类直白名字：进程名 / 文件名会被按关键字直接匹配到。
# 改名时要同步的四处：本文件的 project() 与 add_executable()、
# app.rc 的 InternalName / OriginalFilename / ProductName、
# main.cpp 的窗口类与标题、README 里的产物名。

# ==========================================
# 6. 链接配置（MSVC 与 MinGW 的导入库形式不同）
#    - MSVC 只能链接 .lib 导入库，直接给 .dll 会报 LNK1107
#    - MinGW 的 ld 可以直接吃 .dll
#
#    lib / dll 一律先在配置阶段复制进 build 目录：
#      1) 链接命令里不出现源码目录的中文绝对路径（ninja 在 Windows 上
#         用 cmd.exe 包装命令，中文会被按 ANSI 解码成乱码，ld 找不到文件）；
#      2) 运行期 dll 天然与 exe 同级，不需要 POST_BUILD 再复制一次。
#
#    坑：target_link_libraries() 里给「相对路径」会被 CMake 当成库名，
#    MSVC 下自动补 .lib 后缀 —— interception.lib 会变成
#    interception.lib.lib，链接时报 LNK1181: cannot open input file。
#    只有给「绝对路径」才会被当成文件原样传递。这里给的是 build 目录内
#    的绝对路径，Ninja 生成器会再把它还原成相对名，命令行依旧无中文。
# ==========================================
configure_file("${INTERCEPTION_DIR}/interception.lib"
        "${CMAKE_CURRENT_BINARY_DIR}/interception.lib" COPYONLY)
configure_file("${INTERCEPTION_DIR}/interception.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/interception.dll" COPYONLY)

if(MSVC)
    target_link_libraries(OnePiece PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/interception.lib"
            d3d11
            dxgi
            d3dcompiler
            dwmapi
    )

    # 源码为 UTF-8 无 BOM，必须显式指定，否则 u8"中文" 会被按 GBK 解释成乱码
    target_compile_options(OnePiece PRIVATE /utf-8)

    # 静态链接 CRT，免装 VC 运行库
    set_property(TARGET OnePiece PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(OnePiece PRIVATE
            d3d11
            dxgi
            d3dcompiler
            dwmapi
    )

    # dll 作为「输入文件」直接透传给 ld。
    # 不能走 target_link_libraries：CMake 会把 ./interception.dll 当库名，
    # 生成 -l./interception.dll 导致 ld 报 "cannot find -l..."。
    # 用 -Wl, 前缀则原样传给 ld，且相对路径不含中文。
    target_link_options(OnePiece PRIVATE "-Wl,./interception.dll")

    # -static* 为 GCC 专属选项，仅在非 MSVC 分支使用
    target_link_options(OnePiece PRIVATE
            -static-libgcc -static-libstdc++ -static -lpthread)
endif()

# 运行期需要 dll 与 exe 同目录：上面 configure_file 已把 dll 放进 build 目录，
# 与 OnePiece.exe 天然同级，无需 POST_BUILD 再复制。

# 说明：src/ui/assets/cover.ico 只在编译期由 app.rc 嵌入 exe，
# 运行期不再读取任何图片资源，因此无需把 assets/ 复制到输出目录。
