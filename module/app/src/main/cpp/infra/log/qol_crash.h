// qol_crash.h —— 崩溃现场捕获（预开 fd + 异步信号安全写入）。
//
// 定位：native 崩溃（SIGSEGV/SIGBUS/SIGILL/SIGFPE）时，把「信号 + 全部寄存器 +
// /proc/self/maps + qol.log 尾部」写进 <log_dir>/qol-crash.log，然后恢复原处理器并
// 重新抛出信号，交还系统 tombstone / 游戏自身处理器。
//
// 不做什么：不 fork、不 exec、不展开调用栈。全线程栈需要 ptrace 型 dumper（另一件事），
// 这里只保证崩溃不再无声无息，并且寄存器 + maps 足以离线还原 `lib+offset`。
//
// 不接管 SIGABRT：ART 的 FATAL EXCEPTION 走 abort，接管会干扰系统自身的崩溃记录。
#pragma once

#include <cstdint>

// 安装捕获。log_dir 用于创建 qol-crash.log，log_path 是已打开的 qol.log 路径，
// 崩溃时复制其尾部作为「崩溃前最后几行」。幂等；可在任意时刻任意线程调用。
void qol_crash_init(const char* log_dir, const char* log_path);

#if !defined(__ANDROID__)
// Host-only 测试缝：用真实 ucontext（getcontext）模拟一次崩溃，走与信号处理器
// 完全相同的写盘路径。返回写入是否完成。
bool qol_crash_simulate(int signo, int code, uintptr_t fault_addr);
#endif
