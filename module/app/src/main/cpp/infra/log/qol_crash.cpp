// qol_crash.cpp —— 崩溃现场捕获实现。
//
// 约束：崩溃处理器只允许调用异步信号安全接口。因此本文件
//   - 只用 open/read/write/lseek/close/sigaction/raise/prctl 与栈上缓冲；
//   - 不调用 malloc、printf 系列、std::string；
//   - 时间格式化在 init 阶段预热一次（localtime_r 首次调用会触发时区数据分配）。
//
// 文件分段顺序是离线符号化脚本的契约，禁止调整：
//   header / regs / --- maps --- / --- log tail ---
// header 段内 identity 行（ident.*，signal 行之后、寄存器之前）与 regs 段尾的
// btN 行（FP 链回溯，寄存器之后、maps 之前）是段内扩展行：符号化脚本按
// key=value 解析，四段顺序不变。

#include "infra/log/qol_crash.h"

#include "infra/log/qol_log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>  // rename（截断前保留上一份 crash 文件）
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/uio.h>  // process_vm_readv：崩溃处理器内安全读 FP 链（aarch64）
#endif

namespace {

constexpr int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
constexpr size_t kCrashSignalCount = sizeof(kCrashSignals) / sizeof(kCrashSignals[0]);
constexpr size_t kAltStackBytes = 64 * 1024;
constexpr size_t kTailBytes = 4096;
constexpr size_t kPathCap = 256;
constexpr size_t kLineCap = 1024;

char g_alt_stack[kAltStackBytes];
int g_crash_fd = -1;
char g_log_path[kPathCap] = {};
struct sigaction g_old_actions[kCrashSignalCount] = {};
std::atomic<int> g_capturing{0};
bool g_installed = false;

// ---- identity / config 静态缓冲（seqlock 防撕裂） ----
//
// 启动期由 qol_crash_set_identity / qol_crash_set_config 写入一次；崩溃处理器读取
// 时不加锁、不分配：写侧把 seq 翻为奇数再写数据、写完翻回偶数；读侧前后两次读
// seq，不一致或为奇数就整段丢弃，绝不输出半截内容。

constexpr size_t kIdentityCap = 1024;
constexpr size_t kConfigCap = 512;

char g_identity[kIdentityCap] = {};
char g_config[kConfigCap] = {};
std::atomic<uint32_t> g_identity_seq{0};
std::atomic<uint32_t> g_config_seq{0};

// ---- 异步信号安全的格式化（栈上缓冲，不使用 snprintf） ----

size_t put_raw(char* buf, size_t cap, size_t off, const char* s) {
    if (s == nullptr) return off;
    while (*s != '\0' && off + 1 < cap) buf[off++] = *s++;
    buf[off] = '\0';
    return off;
}

size_t put_dec(char* buf, size_t cap, size_t off, uint64_t value) {
    char digits[24];
    size_t len = 0;
    do {
        digits[len++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && len < sizeof(digits));
    while (len > 0 && off + 1 < cap) buf[off++] = digits[--len];
    buf[off] = '\0';
    return off;
}

size_t put_hex(char* buf, size_t cap, size_t off, uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    off = put_raw(buf, cap, off, "0x");
    char digits[16];
    size_t len = 0;
    do {
        digits[len++] = kDigits[value & 0xf];
        value >>= 4;
    } while (value != 0 && len < sizeof(digits));
    while (len > 0 && off + 1 < cap) buf[off++] = digits[--len];
    buf[off] = '\0';
    return off;
}

void write_all(int fd, const char* data, size_t len) {
    if (fd < 0 || data == nullptr) return;
    size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n <= 0) return;
        written += static_cast<size_t>(n);
    }
}

void emit(int fd, const char* text) {
    if (text == nullptr) return;
    write_all(fd, text, std::strlen(text));
}

const char* signal_name(int signo) {
    switch (signo) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        default: return "SIG?";
    }
}

const char* code_name(int signo, int code) {
    if (signo == SIGSEGV) {
        switch (code) {
            case SEGV_MAPERR: return "SEGV_MAPERR";
            case SEGV_ACCERR: return "SEGV_ACCERR";
            default: break;
        }
    }
    if (signo == SIGBUS) {
        switch (code) {
            case BUS_ADRALN: return "BUS_ADRALN";
            case BUS_ADRERR: return "BUS_ADRERR";
            case BUS_OBJERR: return "BUS_OBJERR";
            default: break;
        }
    }
    if (signo == SIGFPE) {
        switch (code) {
            case FPE_INTDIV: return "FPE_INTDIV";
            case FPE_INTOVF: return "FPE_INTOVF";
            case FPE_FLTDIV: return "FPE_FLTDIV";
            default: break;
        }
    }
    return "?";
}

void write_kv_hex(int fd, const char* key, uint64_t value) {
    char line[64];
    size_t n = put_raw(line, sizeof(line), 0, key);
    n = put_raw(line, sizeof(line), n, "=");
    n = put_hex(line, sizeof(line), n, value);
    n = put_raw(line, sizeof(line), n, "\n");
    write_all(fd, line, n);
}

// ---- identity / config 缓冲读写 ----

void set_static_text(char* buf, size_t cap, std::atomic<uint32_t>& seq, const char* text) {
    if (text == nullptr) text = "";
    size_t keep = std::strlen(text);
    const bool truncated = keep > cap - 16;
    if (truncated) keep = cap - 16;  // 预留 16 字节给 "...(truncated)" 与 NUL
    seq.fetch_add(1, std::memory_order_acq_rel);  // 偶 -> 奇：标记写入开始
    std::memcpy(buf, text, keep);
    buf[keep] = '\0';
    if (truncated) {
        static const char kTruncated[] = "...(truncated)";
        std::memcpy(buf + keep, kTruncated, sizeof(kTruncated));  // 含末尾 NUL
    }
    seq.fetch_add(1, std::memory_order_acq_rel);  // 奇 -> 偶：写入完成
}

// 崩溃处理器专用读：seqlock 两阶段校验。读到写入中途（seq 为奇数或前后不一致）
// 返回 false，调用方写 ident.unstable=1，保证不输出半截内容。
bool read_static_text(const char* buf, const std::atomic<uint32_t>& seq, char* out, size_t cap) {
    const uint32_t before = seq.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    size_t len = 0;
    while (len + 1 < cap && buf[len] != '\0') ++len;
    std::memcpy(out, buf, len);
    out[len] = '\0';
    std::atomic_thread_fence(std::memory_order_acquire);  // 数据读取不得晚于 seq 复查生效
    const uint32_t after = seq.load(std::memory_order_acquire);
    return after == before;
}

// identity/config 输出在 header 段内（signal 行之后、寄存器之前）：符号化脚本把
// 该区域所有 key=value 行收进 header 字典，四段顺序契约不受影响。identity 行文本
// 由 Kotlin 侧生成并自带 "ident." 前缀，这里原样整段写出，不加前缀。
void write_identity(int fd) {
    bool unstable_reported = false;
    char line[kIdentityCap];
    if (!read_static_text(g_identity, g_identity_seq, line, sizeof(line))) {
        emit(fd, "ident.unstable=1\n");
        unstable_reported = true;
    } else if (line[0] == '\0') {
        emit(fd, "ident.device=(not set)\n");
    } else {
        const size_t len = std::strlen(line);
        write_all(fd, line, len);
        if (line[len - 1] != '\n') emit(fd, "\n");
    }

    char cfg[kConfigCap];
    if (!read_static_text(g_config, g_config_seq, cfg, sizeof(cfg))) {
        if (!unstable_reported) emit(fd, "ident.unstable=1\n");
    } else if (cfg[0] == '\0') {
        emit(fd, "ident.config=(not set)\n");
    } else {
        char out[kConfigCap + 24];
        size_t n = put_raw(out, sizeof(out), 0, "ident.config=");
        n = put_raw(out, sizeof(out), n, cfg);
        n = put_raw(out, sizeof(out), n, "\n");
        write_all(fd, out, n);
    }
}

// ---- 寄存器 ----
//
// 设备只跑 arm64/arm32；x86_64 分支仅供 host 单测验证写盘路径。

void write_regs(int fd, void* uc_ptr) {
    if (uc_ptr == nullptr) return;
    const auto* uc = static_cast<const ucontext_t*>(uc_ptr);
#if defined(__aarch64__)
    const mcontext_t& m = uc->uc_mcontext;
    write_kv_hex(fd, "fault_address", static_cast<uint64_t>(m.fault_address));
    write_kv_hex(fd, "pc", static_cast<uint64_t>(m.pc));
    write_kv_hex(fd, "sp", static_cast<uint64_t>(m.sp));
    write_kv_hex(fd, "pstate", static_cast<uint64_t>(m.pstate));
    for (int i = 0; i < 31; ++i) {
        char key[8];
        size_t n = put_raw(key, sizeof(key), 0, "x");
        n = put_dec(key, sizeof(key), n, static_cast<uint64_t>(i));
        key[n] = '\0';
        write_kv_hex(fd, key, static_cast<uint64_t>(m.regs[i]));
    }
#elif defined(__arm__)
    const mcontext_t& m = uc->uc_mcontext;
    write_kv_hex(fd, "fault_address", static_cast<uint64_t>(m.fault_address));
    write_kv_hex(fd, "pc", static_cast<uint64_t>(m.arm_pc));
    write_kv_hex(fd, "sp", static_cast<uint64_t>(m.arm_sp));
    write_kv_hex(fd, "lr", static_cast<uint64_t>(m.arm_lr));
    write_kv_hex(fd, "fp", static_cast<uint64_t>(m.arm_fp));
    write_kv_hex(fd, "cpsr", static_cast<uint64_t>(m.arm_cpsr));
    write_kv_hex(fd, "r0", static_cast<uint64_t>(m.arm_r0));
    write_kv_hex(fd, "r1", static_cast<uint64_t>(m.arm_r1));
    write_kv_hex(fd, "r2", static_cast<uint64_t>(m.arm_r2));
    write_kv_hex(fd, "r3", static_cast<uint64_t>(m.arm_r3));
    write_kv_hex(fd, "r4", static_cast<uint64_t>(m.arm_r4));
    write_kv_hex(fd, "r5", static_cast<uint64_t>(m.arm_r5));
    write_kv_hex(fd, "r6", static_cast<uint64_t>(m.arm_r6));
    write_kv_hex(fd, "r7", static_cast<uint64_t>(m.arm_r7));
    write_kv_hex(fd, "r8", static_cast<uint64_t>(m.arm_r8));
    write_kv_hex(fd, "r9", static_cast<uint64_t>(m.arm_r9));
    write_kv_hex(fd, "r10", static_cast<uint64_t>(m.arm_r10));
#elif defined(__x86_64__)
    const greg_t* g = uc->uc_mcontext.gregs;
    write_kv_hex(fd, "pc", static_cast<uint64_t>(g[REG_RIP]));
    write_kv_hex(fd, "sp", static_cast<uint64_t>(g[REG_RSP]));
    write_kv_hex(fd, "fp", static_cast<uint64_t>(g[REG_RBP]));
    write_kv_hex(fd, "rax", static_cast<uint64_t>(g[REG_RAX]));
    write_kv_hex(fd, "rbx", static_cast<uint64_t>(g[REG_RBX]));
    write_kv_hex(fd, "rcx", static_cast<uint64_t>(g[REG_RCX]));
    write_kv_hex(fd, "rdx", static_cast<uint64_t>(g[REG_RDX]));
    write_kv_hex(fd, "rsi", static_cast<uint64_t>(g[REG_RSI]));
    write_kv_hex(fd, "rdi", static_cast<uint64_t>(g[REG_RDI]));
#else
    (void)fd;
    (void)uc;
#endif
}

// ---- FP 链回溯 ----
//
// arm64 AAPCS 帧记录：[fp] = 调用者 FP，[fp+8] = 本帧返回地址。栈向下生长，所以
// 合法的调用者 FP 必然**大于**当前 FP——链必须递增，递减/相等即链已损坏。
//
// 读内存统一走 process_vm_readv（对未映射地址只返回错误，不触发二次 fault），且
// 每帧先校验：FP 在崩溃线程的栈映射内、返回地址落在可执行映射内；任一不满足就
// 截断，宁可少帧也不输出伪造帧。处理器运行在 SIGSEGV 路径上，二次 fault 会中断
// 后续 --- maps --- / --- log tail --- 段。键名 btN 由 symbolize_crash.py 与寄存器
// 同路径做 maps 解析；符号名由离线符号化负责。

#if defined(__aarch64__) && defined(__linux__)

constexpr int kBacktraceMaxFrames = 64;
constexpr int kMaxExecRanges = 128;

struct AddressRange {
    uintptr_t start;
    uintptr_t end;  // 不含
};

// 崩溃线程的栈映射（约束 FP 链）与全部可执行映射（校验返回地址）。
struct AddressSpace {
    AddressRange stack{0, 0};
    AddressRange exec[kMaxExecRanges];
    int exec_count = 0;
};

bool parse_hex_u64(const char** cursor, const char* end, uintptr_t* value) {
    const char* p = *cursor;
    uintptr_t v = 0;
    bool any = false;
    while (p < end) {
        const char c = *p;
        uintptr_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uintptr_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uintptr_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uintptr_t>(c - 'A' + 10);
        } else {
            break;
        }
        v = (v << 4) | digit;
        any = true;
        ++p;
    }
    if (!any) return false;
    *cursor = p;
    *value = v;
    return true;
}

// maps 行：`<start>-<end> <perms> <offset> <dev> <inode> <path>`。
// 只取 start/end/perms：包含 sp 的那条是崩溃线程栈，`..x` 的是可执行映射。
void parse_maps_line(const char* begin, const char* end, uintptr_t sp, AddressSpace* space) {
    const char* p = begin;
    uintptr_t start = 0;
    uintptr_t stop = 0;
    if (!parse_hex_u64(&p, end, &start)) return;
    if (p >= end || *p != '-') return;
    ++p;
    if (!parse_hex_u64(&p, end, &stop)) return;
    if (start >= stop) return;
    if (p >= end || *p != ' ') return;
    while (p < end && *p == ' ') ++p;
    if (end - p < 4) return;
    const bool executable = p[2] == 'x';
    if (start <= sp && sp < stop) {
        space->stack.start = start;
        space->stack.end = stop;
    }
    if (executable && space->exec_count < kMaxExecRanges) {
        space->exec[space->exec_count++] = AddressRange{start, stop};
    }
}

// 崩溃处理器内解析 /proc/self/maps（无 malloc、无 stdio）：按 '\n' 切行，
// 跨 read 分片的残行搬到缓冲头部继续拼。
void read_address_space(uintptr_t sp, AddressSpace* space) {
    const int maps_fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps_fd < 0) return;
    char buf[2048];
    size_t carry = 0;
    for (;;) {
        const ssize_t n = ::read(maps_fd, buf + carry, sizeof(buf) - carry);
        if (n <= 0) break;
        const size_t avail = carry + static_cast<size_t>(n);
        size_t line_begin = 0;
        for (size_t i = 0; i < avail; ++i) {
            if (buf[i] != '\n') continue;
            parse_maps_line(buf + line_begin, buf + i, sp, space);
            line_begin = i + 1;
        }
        carry = avail - line_begin;
        if (carry >= sizeof(buf) - 1) {
            carry = 0;  // 单行超过缓冲：丢弃该行，不解析半截内容
        } else if (carry > 0) {
            std::memmove(buf, buf + line_begin, carry);
        }
    }
    ::close(maps_fd);
}

// 帧记录必须完整落在崩溃线程栈内，读 [fp, fp+16) 才不会跨映射边界。
bool stack_holds_fp(const AddressSpace& space, uintptr_t fp) {
    const uintptr_t span = space.stack.end - space.stack.start;
    if (span < 2 * sizeof(uintptr_t)) return true;  // maps 解析失败或映射异常：不做栈约束
    if (fp < space.stack.start) return false;
    return fp <= space.stack.end - 2 * sizeof(uintptr_t);
}

bool is_executable(const AddressSpace& space, uintptr_t addr) {
    if (space.exec_count == 0) return true;  // maps 解析失败：不做代码段校验
    for (int i = 0; i < space.exec_count; ++i) {
        if (addr >= space.exec[i].start && addr < space.exec[i].end) return true;
    }
    return false;
}

void write_frame_line(int fd, int frame, uintptr_t value) {
    char key[8];
    size_t n = put_raw(key, sizeof(key), 0, "bt");
    n = put_dec(key, sizeof(key), n, static_cast<uint64_t>(frame));
    key[n] = '\0';
    write_kv_hex(fd, key, static_cast<uint64_t>(value));
}

// 从 x29 起沿 FP 链回溯，直到链尾/损坏/达到 kBacktraceMaxFrames。
// 帧记录缺失（叶函数沿用调用者的 FP、或 FP 已被复用）时，用 x30 记录一层返回地址。
void write_backtrace_aarch64(int fd, const ucontext_t& uc) {
    const uintptr_t sp = static_cast<uintptr_t>(uc.uc_mcontext.sp);
    const uintptr_t lr = static_cast<uintptr_t>(uc.uc_mcontext.regs[30]);
    uintptr_t fp = static_cast<uintptr_t>(uc.uc_mcontext.regs[29]);

    AddressSpace space;
    read_address_space(sp, &space);
    const pid_t pid = ::getpid();

    int frame = 0;
    while (frame < kBacktraceMaxFrames) {
        if (fp == 0) break;
        if ((fp & (sizeof(void*) - 1)) != 0) break;       // FP 必须 8 字节对齐
        if (fp > UINTPTR_MAX - 2 * sizeof(void*)) break;  // 读 16 字节不得地址回绕
        if (!stack_holds_fp(space, fp)) break;
        unsigned long pair[2] = {0, 0};
        struct iovec local {};
        local.iov_base = pair;
        local.iov_len = sizeof(pair);
        struct iovec remote {};
        remote.iov_base = reinterpret_cast<void*>(fp);
        remote.iov_len = sizeof(pair);
        if (::process_vm_readv(pid, &local, 1, &remote, 1, 0) <= 0) break;
        const uintptr_t ret = static_cast<uintptr_t>(pair[1]);
        const uintptr_t next_fp = static_cast<uintptr_t>(pair[0]);
        if (!is_executable(space, ret)) break;  // 返回地址不在代码段：链已损坏
        write_frame_line(fd, frame, ret);
        ++frame;
        if (next_fp == 0) break;                       // 链尾
        if (next_fp <= fp) break;                      // 栈向下生长：调用者 FP 必须更大
        if (!stack_holds_fp(space, next_fp)) break;     // 跳到栈外：链已损坏
        fp = next_fp;
    }
    if (frame == 0 && lr != 0 && is_executable(space, lr)) write_frame_line(fd, 0, lr);
}
#endif

void write_backtrace(int fd, const ucontext_t* uc) {
#if defined(__aarch64__) && defined(__linux__)
    if (uc != nullptr) {
        write_backtrace_aarch64(fd, *uc);
        return;
    }
#endif
    (void)uc;
    emit(fd, "backtrace=unavailable\n");
}

// ---- 附属信息 ----

void write_thread_and_frame(int fd) {
    char line[kLineCap];
    size_t n = put_raw(line, sizeof(line), 0, "pid=");
    n = put_dec(line, sizeof(line), n, static_cast<uint64_t>(::getpid()));
    n = put_raw(line, sizeof(line), n, " tid=");
    n = put_dec(line, sizeof(line), n, static_cast<uint64_t>(::gettid()));
    char name[16] = {};
#if defined(__linux__)
    const int rc = ::prctl(PR_GET_NAME, name, 0, 0, 0);
    (void)rc;
#endif
    n = put_raw(line, sizeof(line), n, " tid_name=");
    n = put_raw(line, sizeof(line), n, name[0] != '\0' ? name : "?");
    n = put_raw(line, sizeof(line), n, " frame=");
    const int64_t frame = qol_log_last_known_frame();
    if (frame < 0) {
        n = put_raw(line, sizeof(line), n, "-1");
    } else {
        n = put_dec(line, sizeof(line), n, static_cast<uint64_t>(frame));
    }
    n = put_raw(line, sizeof(line), n, "\n");
    write_all(fd, line, n);
}

void write_timestamp(int fd) {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
    char stamp[32];
    qol_log_format_ts(stamp, sizeof(stamp),
                      static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
    char line[64];
    size_t n = put_raw(line, sizeof(line), 0, "crash_at=");
    n = put_raw(line, sizeof(line), n, stamp);
    n = put_raw(line, sizeof(line), n, "\n");
    write_all(fd, line, n);
}

void write_maps(int fd) {
    const int maps_fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps_fd < 0) return;
    char buf[1024];
    for (;;) {
        const ssize_t n = ::read(maps_fd, buf, sizeof(buf));
        if (n <= 0) break;
        write_all(fd, buf, static_cast<size_t>(n));
    }
    ::close(maps_fd);
}

// 复制 qol.log 尾部：日志每行一次 write()，无用户态缓冲，尾部就是崩溃前现场。
void write_log_tail(int fd) {
    if (g_log_path[0] == '\0') return;
    const int log_fd = ::open(g_log_path, O_RDONLY | O_CLOEXEC);
    if (log_fd < 0) return;
    const off_t size = ::lseek(log_fd, 0, SEEK_END);
    if (size > 0) {
        const off_t start = size > static_cast<off_t>(kTailBytes)
                                ? size - static_cast<off_t>(kTailBytes)
                                : 0;
        (void)::lseek(log_fd, start, SEEK_SET);
        char buf[1024];
        for (;;) {
            const ssize_t n = ::read(log_fd, buf, sizeof(buf));
            if (n <= 0) break;
            write_all(fd, buf, static_cast<size_t>(n));
        }
    }
    ::close(log_fd);
}

// 返回 false 表示已有其他线程在写（递归崩溃 / 多线程同时崩）。
bool capture(int signo, const siginfo_t* si, void* uc) {
    if (g_capturing.exchange(1, std::memory_order_acq_rel) != 0) return false;
    const int fd = g_crash_fd;
    if (fd < 0) return false;

    emit(fd, "=== qol crash ===\n");
    write_timestamp(fd);
    write_thread_and_frame(fd);

    char line[kLineCap];
    size_t n = put_raw(line, sizeof(line), 0, "signal=");
    n = put_dec(line, sizeof(line), n, static_cast<uint64_t>(signo));
    n = put_raw(line, sizeof(line), n, " ");
    n = put_raw(line, sizeof(line), n, signal_name(signo));
    n = put_raw(line, sizeof(line), n, " code=");
    const int code = si != nullptr ? si->si_code : 0;
    n = put_dec(line, sizeof(line), n, static_cast<uint64_t>(code < 0 ? 0 : code));
    n = put_raw(line, sizeof(line), n, " (");
    n = put_raw(line, sizeof(line), n, code_name(signo, code));
    n = put_raw(line, sizeof(line), n, ") fault=");
    const uintptr_t fault = si != nullptr ? reinterpret_cast<uintptr_t>(si->si_addr) : 0;
    n = put_hex(line, sizeof(line), n, static_cast<uint64_t>(fault));
    n = put_raw(line, sizeof(line), n, "\n");
    write_all(fd, line, n);

    write_identity(fd);

    write_regs(fd, uc);

    write_backtrace(fd, static_cast<const ucontext_t*>(uc));

    emit(fd, "--- maps ---\n");
    write_maps(fd);
    emit(fd, "--- log tail ---\n");
    write_log_tail(fd);
    emit(fd, "=== end ===\n");
    return true;
}

void restore_and_reraise(int signo) {
    for (size_t i = 0; i < kCrashSignalCount; ++i) {
        if (kCrashSignals[i] != signo) continue;
        (void)::sigaction(signo, &g_old_actions[i], nullptr);
        break;
    }
    sigset_t unblock;
    ::sigemptyset(&unblock);
    ::sigaddset(&unblock, signo);
    (void)::sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
    (void)::raise(signo);
    // 原处理器返回后仍走到这里：按惯例以 128+signo 退出，避免死循环。
    ::_exit(128 + signo);
}

void crash_handler(int signo, siginfo_t* si, void* uc) {
    (void)capture(signo, si, uc);
    restore_and_reraise(signo);
}

}  // namespace

void qol_crash_init(const char* log_dir, const char* log_path) {
    if (g_installed) return;
    if (log_dir == nullptr || log_dir[0] == '\0') return;

    // qol-crash.log 每次进程启动截断：一个进程只可能崩一次，保留最新现场。
    // 截断前先把上一份现场改名 .prev；失败（含 ENOENT）一律忽略，不得阻断初始化。
    char crash_path[kPathCap];
    size_t n = put_raw(crash_path, sizeof(crash_path), 0, log_dir);
    n = put_raw(crash_path, sizeof(crash_path), n, "/qol-crash.log");
    if (n >= sizeof(crash_path) - 1) return;
    char prev_path[kPathCap];
    const size_t crash_len = std::strlen(crash_path);
    if (crash_len + sizeof(".prev") <= sizeof(prev_path)) {
        std::memcpy(prev_path, crash_path, crash_len);
        std::memcpy(prev_path + crash_len, ".prev", sizeof(".prev"));
        (void)::rename(crash_path, prev_path);
    }
    g_crash_fd = ::open(crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (g_crash_fd < 0) {
        QOL_LOG_WARN(QolDomain::kPlatform, "crash capture disabled: cannot open %s errno=%d",
                     crash_path, errno);
        return;
    }

    if (log_path != nullptr) {
        size_t m = 0;
        while (log_path[m] != '\0' && m + 1 < sizeof(g_log_path)) {
            g_log_path[m] = log_path[m];
            ++m;
        }
        g_log_path[m] = '\0';
    }

    // 预热时区数据，避免崩溃处理器里第一次调用 localtime_r 触发分配。
    char warmup[32];
    qol_log_format_ts(warmup, sizeof(warmup), 0);

    // 备用信号栈：栈溢出导致的 SIGSEGV 在主栈上无法继续执行处理器。
    stack_t alt{};
    alt.ss_sp = g_alt_stack;
    alt.ss_size = sizeof(g_alt_stack);
    alt.ss_flags = 0;
    if (::sigaltstack(&alt, nullptr) != 0) return;

    for (size_t i = 0; i < kCrashSignalCount; ++i) {
        struct sigaction action {};
        action.sa_sigaction = crash_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigemptyset(&action.sa_mask);
        if (::sigaction(kCrashSignals[i], &action, &g_old_actions[i]) != 0) return;
    }
    g_installed = true;
}

void qol_crash_set_identity(const char* text) {
    set_static_text(g_identity, sizeof(g_identity), g_identity_seq, text);
}

void qol_crash_set_config(const char* text) {
    set_static_text(g_config, sizeof(g_config), g_config_seq, text);
}

#if !defined(__ANDROID__)
bool qol_crash_simulate(int signo, int code, uintptr_t fault_addr) {
    siginfo_t si {};
    si.si_signo = signo;
    si.si_code = code;
    si.si_addr = reinterpret_cast<void*>(fault_addr);
    ucontext_t uc {};
    (void)::getcontext(&uc);
    return capture(signo, &si, &uc);
}
#endif
