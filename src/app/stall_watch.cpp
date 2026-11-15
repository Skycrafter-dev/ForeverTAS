#include "app/stall_watch.h"

#include "diagnostics/diagnostics_log.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace forevertas::app {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPing = std::chrono::milliseconds(25);
constexpr auto kStalled = std::chrono::milliseconds(40);
constexpr auto kSample = std::chrono::milliseconds(10);
// a long freeze is sampled for its first two seconds or so: enough to say where
constexpr std::size_t kMaximumSamples = 150;
// FOREVERTAS_STALL_MS reports shorter ones (looking for small hitches)
std::chrono::milliseconds reported{100};
constexpr int kFrames = 48;

std::atomic<bool> running{false};
std::atomic<bool> pending{false};
std::atomic<std::int64_t> pingSent{0};
std::thread watcher;

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
}

using Sample = std::vector<std::uintptr_t>;

#if defined(_WIN32)

HANDLE guiThread = nullptr;

// The GUI thread's return addresses, walked while it is suspended: no
// allocation, no locks it could hold (x64 unwind data only).
// ntdll, where the loader runs: a thread stopped there may hold the lock
// the unwinder needs, so it is not walked.
bool InLoader(DWORD64 address) {
    static const auto range = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        MODULEINFO info{};
        if (ntdll == nullptr || !GetModuleInformation(GetCurrentProcess(), ntdll, &info, sizeof info)) {
            return std::pair<DWORD64, DWORD64>(0, 0);
        }
        const auto base = reinterpret_cast<DWORD64>(info.lpBaseOfDll);
        return std::pair<DWORD64, DWORD64>(base, base + info.SizeOfImage);
    }();
    return address >= range.first && address < range.second;
}

Sample SampleThreadHandle(HANDLE thread) {
    Sample sample;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return sample;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    std::uintptr_t frames[kFrames];
    int count = 0;
    if (GetThreadContext(thread, &context) && !InLoader(context.Rip)) {
        for (; count < kFrames && context.Rip != 0; ++count) {
            frames[count] = static_cast<std::uintptr_t>(context.Rip);
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function == nullptr) {
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
            } else {
                void *handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
                                 &handlerData, &establisher, nullptr);
            }
        }
    }
    ResumeThread(thread);
    sample.assign(frames, frames + count);
    return sample;
}

Sample SampleGui() {
    return SampleThreadHandle(guiThread);
}

// Qt names its threads: the render thread is "QSGRenderThread".
Sample SampleRender() {
    static HANDLE render = nullptr;
    if (render == nullptr) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof entry;
            for (BOOL more = Thread32First(snapshot, &entry); more && render == nullptr;
                 more = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                   THREAD_QUERY_LIMITED_INFORMATION,
                                           FALSE, entry.th32ThreadID);
                if (thread == nullptr) continue;
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr &&
                    std::wstring(description).rfind(L"QSGRenderThread", 0) == 0) {
                    render = thread;
                } else {
                    CloseHandle(thread);
                }
                if (description != nullptr) LocalFree(description);
            }
            CloseHandle(snapshot);
        }
    }
    return render != nullptr ? SampleThreadHandle(render) : Sample{};
}

std::string Name(std::uintptr_t address) {
    static bool initialized = false;
    if (!initialized) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        initialized = true;
    }
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    HMODULE module = nullptr;
    char moduleName[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, moduleName, MAX_PATH);
    }
    const char *base = moduleName;
    for (const char *at = moduleName; *at; ++at) {
        if (*at == '\\' || *at == '/') base = at + 1;
    }
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        return std::string(symbol->Name) + " (" + base + ")";
    }
    char text[MAX_PATH + 32];
    std::snprintf(text, sizeof text, "%s+0x%llx", base,
                  static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    return text;
}

#else

pid_t guiThread = 0;
int sampleSignal = 0;
// The sampled thread, stopped in the handler, hands over where its stack is
// and waits (at most 20 ms) while the watch copies it. Nothing in the
// handler takes a lock: unwinding there could wait forever on the dynamic
// loader's lock if the thread was stopped holding it.
std::atomic<std::uintptr_t> handlerSp{0};
std::atomic<std::uintptr_t> handlerPc{0};
std::atomic<int> handlerSerial{0};
std::atomic<int> released{0};

void OnSample(int, siginfo_t *, void *context) {
    const auto *user = static_cast<const ucontext_t *>(context);
    handlerSp.store(static_cast<std::uintptr_t>(user->uc_mcontext.gregs[REG_RSP]),
                    std::memory_order_relaxed);
    handlerPc.store(static_cast<std::uintptr_t>(user->uc_mcontext.gregs[REG_RIP]),
                    std::memory_order_relaxed);
    const int serial = handlerSerial.fetch_add(1, std::memory_order_release) + 1;
    timespec pause{0, 100000};
    for (int waited = 0; waited < 200 && released.load(std::memory_order_acquire) != serial; ++waited) {
        nanosleep(&pause, nullptr);
    }
}

// Executable mappings: where return addresses can point.
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> CodeRanges() {
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
    if (FILE *maps = std::fopen("/proc/self/maps", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, maps) != nullptr) {
            unsigned long long from = 0, to = 0;
            char permissions[8] = {};
            if (std::sscanf(line, "%llx-%llx %7s", &from, &to, permissions) == 3 &&
                permissions[2] == 'x') {
                ranges.emplace_back(static_cast<std::uintptr_t>(from), static_cast<std::uintptr_t>(to));
            }
        }
        std::fclose(maps);
    }
    return ranges;
}

bool Read(std::uintptr_t address, void *into, std::size_t size) {
    iovec local{into, size};
    iovec remote{reinterpret_cast<void *>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

// A return address follows a call: E8 rel32, or FF /2 in its usual forms.
bool AfterCall(std::uintptr_t address) {
    unsigned char code[7] = {};
    if (!Read(address - 7, code, sizeof code)) return false;
    if (code[2] == 0xE8) return true;                                       // call rel32
    if (code[5] == 0xFF && (code[6] & 0x38) == 0x10) return true;           // call reg
    if (code[4] == 0xFF && (code[5] & 0x38) == 0x10) return true;           // call [reg+8]
    if (code[1] == 0xFF && (code[2] & 0x38) == 0x10) return true;           // call [rip/reg+32]
    if (code[0] == 0xFF && (code[1] & 0x38) == 0x10) return true;           // call [sib+32]
    if (code[3] == 0xFF && (code[4] & 0x38) == 0x10) return true;           // call [sib+8]
    return false;
}

// Where a thread blocked in a system call is: /proc/self/task/<tid>/syscall
// gives its stack and instruction pointers ("running" when it runs code).
bool BlockedAt(pid_t thread, std::uintptr_t *sp, std::uintptr_t *pc) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/syscall", static_cast<int>(thread));
    FILE *file = std::fopen(path, "r");
    if (file == nullptr) return false;
    char line[256] = {};
    const bool read = std::fgets(line, sizeof line, file) != nullptr;
    std::fclose(file);
    if (!read || std::strncmp(line, "running", 7) == 0) return false;
    // "nr a1 a2 a3 a4 a5 a6 sp pc" in a system call, "-1 sp pc" blocked
    // otherwise: the last two fields
    std::vector<std::string> fields;
    for (char *field = std::strtok(line, " \n"); field != nullptr; field = std::strtok(nullptr, " \n")) {
        fields.emplace_back(field);
    }
    if (fields.size() < 3) return false;
    *sp = static_cast<std::uintptr_t>(std::strtoull(fields[fields.size() - 2].c_str(), nullptr, 16));
    *pc = static_cast<std::uintptr_t>(std::strtoull(fields.back().c_str(), nullptr, 16));
    return *sp != 0 && *pc != 0;
}

Sample SampleThread(pid_t thread) {
    static std::vector<std::pair<std::uintptr_t, std::uintptr_t>> code = CodeRanges();
    static auto codeRead = Clock::now();
    if (Clock::now() - codeRead > std::chrono::seconds(5)) {
        code = CodeRanges();   // libraries loaded since
        codeRead = Clock::now();
    }
    std::uintptr_t sp = 0;
    std::uintptr_t pc = 0;
    int serial = -1;
    // A thread blocked in a system call is read where it waits, with no
    // signal: a signal would break the call off, and a call the kernel then
    // starts again (a file flush) might never finish while it is sampled.
    // Only a thread running code is stopped by the signal.
    if (!BlockedAt(thread, &sp, &pc)) {
        const int before = handlerSerial.load(std::memory_order_acquire);
        if (syscall(SYS_tgkill, getpid(), thread, sampleSignal) != 0) return {};
        const auto deadline = Clock::now() + std::chrono::milliseconds(5);
        while (handlerSerial.load(std::memory_order_acquire) == before) {
            if (Clock::now() > deadline) return {};
            std::this_thread::yield();
        }
        serial = handlerSerial.load(std::memory_order_acquire);
        sp = handlerSp.load(std::memory_order_relaxed);
        pc = handlerPc.load(std::memory_order_relaxed);
    }
    // the stack above the thread's stack pointer, page by page
    static std::vector<std::uintptr_t> words(64 * 1024 / sizeof(std::uintptr_t));
    std::size_t copied = 0;
    constexpr std::size_t kPage = 4096;
    std::size_t length = std::min(kPage - (sp % kPage), words.size() * sizeof(std::uintptr_t));
    std::uintptr_t at = sp;
    while (copied < words.size()) {
        if (!Read(at, words.data() + copied, length)) break;
        copied += length / sizeof(std::uintptr_t);
        at += length;
        length = std::min<std::size_t>(kPage, (words.size() - copied) * sizeof(std::uintptr_t));
    }
    if (serial >= 0) released.store(serial, std::memory_order_release);
    const auto inCode = [&](std::uintptr_t value) {
        for (const auto &range : code) {
            if (value >= range.first && value < range.second) return true;
        }
        return false;
    };
    Sample sample;
    sample.push_back(pc);
    for (std::size_t index = 0; index < copied && sample.size() < kFrames; ++index) {
        const std::uintptr_t value = words[index];
        if (inCode(value) && AfterCall(value)) sample.push_back(value);
    }
    return sample;
}

Sample SampleGui() {
    return SampleThread(guiThread);
}

// Qt Quick's render thread, which the window waits for in each frame.
Sample SampleRender() {
    static pid_t render = 0;
    if (render == 0 || syscall(SYS_tgkill, getpid(), render, 0) != 0) {
        render = 0;
        if (DIR *tasks = opendir("/proc/self/task")) {
            while (dirent *entry = readdir(tasks)) {
                const std::string path = std::string("/proc/self/task/") + entry->d_name + "/comm";
                if (FILE *comm = std::fopen(path.c_str(), "r")) {
                    char name[64] = {};
                    if (std::fgets(name, sizeof name, comm) != nullptr &&
                        std::string(name).rfind("QSGRenderThread", 0) == 0) {
                        render = static_cast<pid_t>(std::atoi(entry->d_name));
                    }
                    std::fclose(comm);
                }
            }
            closedir(tasks);
        }
    }
    return render != 0 ? SampleThread(render) : Sample{};
}

std::string Name(std::uintptr_t address) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(address - 1), &info) == 0) {
        char text[32];
        std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    const char *module = info.dli_fname != nullptr ? info.dli_fname : "?";
    for (const char *at = module; *at; ++at) {
        if (*at == '/') module = at + 1;
    }
    if (info.dli_sname != nullptr) {
        int status = 0;
        char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        std::string name = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
        std::free(demangled);
        // the arguments make the line long and say little here
        const std::size_t open = name.find('(');
        if (open != std::string::npos && open > 0) name.erase(open);
        return name + " (" + module + ")";
    }
    char text[256];
    std::snprintf(text, sizeof text, "%s+0x%llx", module,
                  static_cast<unsigned long long>(address -
                                                  reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    return text;
}

#endif

// What a set of samples spent its time in: the functions most samples were
// in (a function counts once per sample), and the innermost ones.
std::string Summary(const std::vector<Sample> &samples) {
    std::map<std::uintptr_t, std::string> names;
    std::map<std::string, int> inclusive;
    std::map<std::string, int> innermost;
    for (const Sample &sample : samples) {
        std::vector<std::string> seen;
        for (std::size_t index = 0; index < sample.size(); ++index) {
            auto found = names.find(sample[index]);
            if (found == names.end()) found = names.emplace(sample[index], Name(sample[index])).first;
            const std::string &name = found->second;
            if (index == 0) ++innermost[name];
            if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                seen.push_back(name);
                ++inclusive[name];
            }
        }
    }
    const auto ranked = [](const std::map<std::string, int> &counts) {
        std::vector<std::pair<int, std::string>> list;
        for (const auto &entry : counts) list.emplace_back(entry.second, entry.first);
        std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        return list;
    };
    std::string text = std::to_string(samples.size()) + " samples; innermost:";
    const auto inner = ranked(innermost);
    for (std::size_t index = 0; index < inner.size() && index < 5; ++index) {
        text += (index == 0 ? " " : ", ") + inner[index].second + " x" + std::to_string(inner[index].first);
    }
    // one whole stack, innermost first: the middle sample, as a typical one
    if (!samples.empty()) {
        text += "; a stack:";
        const Sample &typical = samples[samples.size() / 2];
        for (std::size_t index = 0; index < typical.size() && index < 40; ++index) {
            text += "\n      " + names.at(typical[index]);
        }
    }
    text += "; in:";
    const auto all = ranked(inclusive);
    for (std::size_t index = 0; index < all.size() && index < 28; ++index) {
        text += "\n    " + std::to_string(all[index].first) + "  " + all[index].second;
    }
    return text;
}

void Report(std::int64_t milliseconds, const std::vector<Sample> &gui,
            const std::vector<Sample> &render) {
    diagnostics::Log("ui stall: the window did not respond for %lld ms\n  window thread: %s\n"
                     "  render thread: %s",
                     static_cast<long long>(milliseconds), Summary(gui).c_str(),
                     Summary(render).c_str());
}

void Watch() {
    std::vector<Sample> samples;
    std::vector<Sample> renderSamples;
    Clock::time_point lastSample;
    while (running.load(std::memory_order_relaxed)) {
        const std::int64_t now = Now();
        if (!pending.load(std::memory_order_acquire)) {
            const std::int64_t sent = pingSent.load(std::memory_order_relaxed);
            const std::int64_t waited = now - sent;
            if (!samples.empty() || waited >= std::chrono::nanoseconds(kPing).count()) {
                // the last ping was answered: a freeze, if any, is over
                if (!samples.empty()) {
                    const std::int64_t stalled = (now - sent) / 1000000;
                    if (std::chrono::milliseconds(stalled) >= reported) {
                        Report(stalled, samples, renderSamples);
                    }
                    samples.clear();
                    renderSamples.clear();
                }
                pingSent.store(now, std::memory_order_relaxed);
                pending.store(true, std::memory_order_release);
                QMetaObject::invokeMethod(
                        qApp, [] { pending.store(false, std::memory_order_release); },
                        Qt::QueuedConnection);
            }
        } else if (now - pingSent.load(std::memory_order_relaxed) >
                           std::chrono::nanoseconds(kStalled).count() &&
                   Clock::now() - lastSample >= kSample && samples.size() < kMaximumSamples) {
            lastSample = Clock::now();
            Sample sample = SampleGui();
            if (!sample.empty()) samples.push_back(std::move(sample));
            Sample render = SampleRender();
            if (!render.empty()) renderSamples.push_back(std::move(render));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

void StartStallWatch() {
    if (running.exchange(true)) return;
    const int threshold = qEnvironmentVariableIntValue("FOREVERTAS_STALL_MS");
    if (threshold > 0) reported = std::chrono::milliseconds(std::max(45, threshold));
#if defined(_WIN32)
    guiThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                           GetCurrentThreadId());
    if (guiThread == nullptr) {
        running = false;
        return;
    }
#else
    guiThread = static_cast<pid_t>(syscall(SYS_gettid));
    sampleSignal = SIGRTMIN + 5;
    struct sigaction action{};
    action.sa_sigaction = OnSample;
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(sampleSignal, &action, nullptr);
#endif
    pingSent.store(Now(), std::memory_order_relaxed);
    pending.store(false);
    watcher = std::thread(Watch);
    // FOREVERTAS_STALL_TEST=MS: the window blocked in a system call that long,
    // once, 5 s after start (checks the watch on a real freeze)
    if (const int test = qEnvironmentVariableIntValue("FOREVERTAS_STALL_TEST"); test > 0) {
        QTimer::singleShot(5000, qApp, [test] {
            const auto started = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(test));
            diagnostics::Log("stall test: blocked %lld ms (asked %d)",
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - started).count()),
                             test);
        });
    }
}

void StopStallWatch() {
    if (!running.exchange(false)) return;
    if (watcher.joinable()) watcher.join();
#if defined(_WIN32)
    if (guiThread != nullptr) CloseHandle(guiThread);
    guiThread = nullptr;
#endif
}

}  // namespace forevertas::app
