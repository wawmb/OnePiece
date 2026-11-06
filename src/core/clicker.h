#pragma once

namespace app {

[[noreturn]] void macro_worker();
// 注意：不带 [[noreturn]]。驱动层接收失败时本线程会退避重试，
// 理论上不会返回，但保留可返回的签名，避免编译器误优化掉退出路径。
void interception_thread();
[[noreturn]] void hotkey_poller();

} // namespace app
