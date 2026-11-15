#include "app/diagnostics_startup.h"

#include "diagnostics/diagnostics_log.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlApplicationEngine>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0) && __has_include(<rhi/qrhi.h>)
#include <rhi/qrhi.h>
#define FOREVERTAS_HAS_RHI_INFO 1
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <psapi.h>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace forevertas::app {
namespace {

using diagnostics::Log;

constexpr int kKeptLogs = 20;
constexpr int kKeptCrashDumps = 3;
constexpr char kCrashMarker[] = "=== CRASH";

QString logsFolder;
QString previousLog;
bool previousCrashed = false;
QtMessageHandler previousMessageHandler = nullptr;

void QtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    static const char *const names[] = {"debug", "warning", "critical", "fatal", "info"};
    const char *const category = context.category != nullptr ? context.category : "";
    const bool plainCategory = category[0] == '\0' || std::strcmp(category, "default") == 0;
    Log("qt %s%s%s%s: %s", type >= 0 && type <= 4 ? names[type] : "?", plainCategory ? "" : " (",
        plainCategory ? "" : category, plainCategory ? "" : ")", message.toUtf8().constData());
    if (previousMessageHandler != nullptr) previousMessageHandler(type, context, message);
}

// The newest logs stay; older ones and all but the newest crash dumps go.
void PruneLogs(const QDir &folder) {
    const QFileInfoList logs = folder.entryInfoList({QStringLiteral("ForeverTAS-*.log")},
                                                    QDir::Files, QDir::Name | QDir::Reversed);
    for (qsizetype index = kKeptLogs; index < logs.size(); ++index) {
        QFile::remove(logs[index].absoluteFilePath());
    }
    const QFileInfoList dumps = folder.entryInfoList({QStringLiteral("ForeverTAS-*.dmp")},
                                                     QDir::Files, QDir::Name | QDir::Reversed);
    for (qsizetype index = kKeptCrashDumps; index < dumps.size(); ++index) {
        QFile::remove(dumps[index].absoluteFilePath());
    }
}

// The newest log before this session's: did that session end normally?
void InspectPreviousLog(const QDir &folder) {
    const QFileInfoList logs = folder.entryInfoList({QStringLiteral("ForeverTAS-*.log")},
                                                    QDir::Files, QDir::Name | QDir::Reversed);
    if (logs.isEmpty()) return;
    QFile file(logs.first().absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray contents = file.readAll();
    if (contents.contains(diagnostics::kClosedPrefix)) return;
    previousLog = logs.first().absoluteFilePath();
    previousCrashed = contents.contains(kCrashMarker);
}

void LogEnvironment(const char *const *names) {
    for (const char *const *name = names; *name != nullptr; ++name) {
        if (qEnvironmentVariableIsSet(*name)) {
            Log("env %s=%s", *name, qgetenv(*name).constData());
        }
    }
}

#if defined(_WIN32)

std::wstring dumpPath;

std::string Narrow(const std::wstring &text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

std::string FileVersion(const std::wstring &file) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0) return "no version";
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return "no version";
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) ||
        info == nullptr) {
        return "no version";
    }
    char text[64];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                  LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                  LOWORD(info->dwFileVersionLS));
    return text;
}

std::string RegistryString(HKEY root, const wchar_t *key, const wchar_t *value) {
    wchar_t buffer[1024];
    DWORD size = sizeof buffer;
    DWORD type = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_RT_REG_MULTI_SZ,
                     &type, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return Narrow(buffer);
}

DWORD RegistryNumber(HKEY root, const wchar_t *key, const wchar_t *value) {
    DWORD number = 0;
    DWORD size = sizeof number;
    RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &number, &size);
    return number;
}

void LogSystem() {
    const wchar_t *const nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    Log("windows: %s %s, build %s.%lu", RegistryString(HKEY_LOCAL_MACHINE, nt, L"ProductName").c_str(),
        RegistryString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion").c_str(),
        RegistryString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild").c_str(),
        static_cast<unsigned long>(RegistryNumber(HKEY_LOCAL_MACHINE, nt, L"UBR")));
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    Log("cpu: %s, %lu logical processors",
        RegistryString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       L"ProcessorNameString")
                .c_str(),
        static_cast<unsigned long>(system.dwNumberOfProcessors));
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    GlobalMemoryStatusEx(&memory);
    Log("memory: %.1f GiB, %.1f GiB free",
        static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0),
        static_cast<double>(memory.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
}

void LogAdapters() {
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
        Log("gpu: DXGI unavailable");
    } else {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            LARGE_INTEGER umd{};
            const bool hasVersion =
                    SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
            Log("gpu %u: %s, vendor %04x device %04x rev %02x, %.0f MiB dedicated%s, driver %u.%u.%u.%u",
                index, Narrow(description.Description).c_str(), description.VendorId,
                description.DeviceId, description.Revision,
                static_cast<double>(description.DedicatedVideoMemory) / (1024.0 * 1024.0),
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ? ", software" : "",
                hasVersion ? HIWORD(umd.HighPart) : 0u, hasVersion ? LOWORD(umd.HighPart) : 0u,
                hasVersion ? HIWORD(umd.LowPart) : 0u, hasVersion ? LOWORD(umd.LowPart) : 0u);
            adapter->Release();
        }
        factory->Release();
    }
    // The display drivers' own records: AMD's software version among them.
    const std::wstring displayClass =
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
    for (int index = 0; index < 16; ++index) {
        wchar_t number[8];
        std::swprintf(number, 8, L"%04d", index);
        const std::wstring key = displayClass + number;
        const std::string description = RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc");
        if (description.empty()) continue;
        const std::string radeon =
                RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"RadeonSoftwareVersion");
        Log("display driver: %s, %s %s, dated %s%s%s", description.c_str(),
            RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"ProviderName").c_str(),
            RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion").c_str(),
            RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDate").c_str(),
            radeon.empty() ? "" : ", Radeon Software ", radeon.c_str());
    }
}

// Is a GPU runtime this ForeverTAS may load installed, and which version?
void LogRuntimeFiles() {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    for (const wchar_t *name : {L"nvcuda.dll", L"amdhip64_6.dll", L"amdhip64_7.dll", L"vulkan-1.dll",
                                L"opengl32.dll"}) {
        const std::wstring file = std::wstring(system) + L"\\" + name;
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        Log("runtime %s: %s", Narrow(name).c_str(), FileVersion(file).c_str());
    }
}

const char *ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    case 0xE06D7363: return "C++ exception";
    case 0xE0000001: return "std::terminate";
    default: return "exception";
    }
}

void DescribeAddress(DWORD64 address, char *out, std::size_t size) {
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module != nullptr) {
        GetModuleFileNameW(module, name, MAX_PATH);
        const wchar_t *base = name;
        for (const wchar_t *at = name; *at; ++at) {
            if (*at == L'\\' || *at == L'/') base = at + 1;
        }
        std::snprintf(out, size, "%ls+0x%llx", base,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(out, size, "0x%llx (no module)", static_cast<unsigned long long>(address));
    }
}

// The crash: what, where (module and offset), the stack, the loaded modules,
// and a minidump next to the log.
LONG WINAPI Crash(EXCEPTION_POINTERS *pointers) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD &record = *pointers->ExceptionRecord;
    char where[512];
    DescribeAddress(reinterpret_cast<DWORD64>(record.ExceptionAddress), where, sizeof where);
    char line[1024];
    std::snprintf(line, sizeof line, "\n%s: %s (0x%08lx) at %s, thread %lu ===\n", kCrashMarker,
                  ExceptionName(record.ExceptionCode),
                  static_cast<unsigned long>(record.ExceptionCode), where,
                  static_cast<unsigned long>(GetCurrentThreadId()));
    diagnostics::LogRaw(line);
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        std::snprintf(line, sizeof line, "%s address 0x%llx\n",
                      kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                      static_cast<unsigned long long>(record.ExceptionInformation[1]));
        diagnostics::LogRaw(line);
    }
    // The stack: module+offset always (symbolized later against the build's
    // symbols), names where symbols are at hand.
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitializeW(process, nullptr, TRUE);
    CONTEXT context = *pointers->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + 512];
    for (int depth = 0; depth < 64; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        DescribeAddress(frame.AddrPC.Offset, where, sizeof where);
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolStorage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
        std::snprintf(line, sizeof line, "  #%02d %s%s%s\n", depth, where, named ? " " : "",
                      named ? symbol->Name : "");
        diagnostics::LogRaw(line);
    }
    HMODULE modules[512];
    DWORD needed = 0;
    if (EnumProcessModules(process, modules, sizeof modules, &needed)) {
        for (DWORD index = 0; index < needed / sizeof(HMODULE) && index < 512; ++index) {
            wchar_t name[MAX_PATH];
            GetModuleFileNameW(modules[index], name, MAX_PATH);
            MODULEINFO info{};
            GetModuleInformation(process, modules[index], &info, sizeof info);
            std::snprintf(line, sizeof line, "  module %p %8lx %ls %s\n", info.lpBaseOfDll,
                          static_cast<unsigned long>(info.SizeOfImage), name,
                          FileVersion(name).c_str());
            diagnostics::LogRaw(line);
        }
    }
    const HANDLE dump = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), pointers, FALSE};
        const BOOL written = MiniDumpWriteDump(
                process, GetCurrentProcessId(), dump,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                           MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                &exception, nullptr, nullptr);
        CloseHandle(dump);
        std::snprintf(line, sizeof line, "crash dump %s: %ls\n", written ? "written" : "FAILED",
                      dumpPath.c_str());
        diagnostics::LogRaw(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#else

const char *SignalName(int signal) {
    switch (signal) {
    case SIGSEGV: return "segmentation fault";
    case SIGBUS: return "bus error";
    case SIGILL: return "illegal instruction";
    case SIGFPE: return "arithmetic error";
    case SIGABRT: return "abort";
    default: return "signal";
    }
}

void WriteAll(int descriptor, const char *text) {
    std::size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t written = ::write(descriptor, text, left);
        if (written <= 0) return;
        text += written;
        left -= static_cast<std::size_t>(written);
    }
}

// The crash: what, where, and the stack (module+offset, symbolized later
// against the build), then the default action.
void Crash(int signal, siginfo_t *info, void *) {
    static volatile sig_atomic_t entered = 0;
    if (entered != 0) _exit(128 + signal);
    entered = 1;
    const int descriptor = diagnostics::LogDescriptor();
    if (descriptor >= 0) {
        char line[256];
        std::snprintf(line, sizeof line, "\n%s: %s (signal %d, code %d) at address %p ===\n",
                      kCrashMarker, SignalName(signal), signal, info != nullptr ? info->si_code : 0,
                      info != nullptr ? info->si_addr : nullptr);
        WriteAll(descriptor, line);
        void *frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, descriptor);
        // Where each module's code sits, for the offsets above.
        const int maps = ::open("/proc/self/maps", O_RDONLY);
        if (maps >= 0) {
            WriteAll(descriptor, "code mappings:\n");
            char buffer[4096];
            char line[512];
            std::size_t length = 0;
            ssize_t read = 0;
            while ((read = ::read(maps, buffer, sizeof buffer)) > 0) {
                for (ssize_t at = 0; at < read; ++at) {
                    if (length + 1 < sizeof line) line[length++] = buffer[at];
                    if (buffer[at] != '\n') continue;
                    line[length] = '\0';
                    if (std::strstr(line, " r-xp ") != nullptr) WriteAll(descriptor, line);
                    length = 0;
                }
            }
            ::close(maps);
        }
    }
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

// Logging out or shutting down closes ForeverTAS with SIGTERM: not a crash.
void Terminate(int signal) {
    const int descriptor = diagnostics::LogDescriptor();
    if (descriptor >= 0) {
        WriteAll(descriptor, "=== ForeverTAS closed by the system (SIGTERM) ===\n");
    }
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

void LogLinuxSystem() {
    QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
    if (cpuinfo.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : cpuinfo.readAll().split('\n')) {
            if (line.startsWith("model name")) {
                Log("cpu: %s, %u logical processors", line.mid(line.indexOf(':') + 2).constData(),
                    std::thread::hardware_concurrency());
                break;
            }
        }
    }
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        double total = 0.0;
        double available = 0.0;
        for (const QByteArray &line : meminfo.readAll().split('\n')) {
            const QList<QByteArray> fields = line.simplified().split(' ');
            if (fields.size() < 2) continue;
            if (fields[0] == "MemTotal:") total = fields[1].toDouble();
            if (fields[0] == "MemAvailable:") available = fields[1].toDouble();
        }
        Log("memory: %.1f GiB, %.1f GiB free", total / (1024.0 * 1024.0),
            available / (1024.0 * 1024.0));
    }
    const QDir drm(QStringLiteral("/sys/class/drm"));
    for (const QString &card : drm.entryList({QStringLiteral("card?"), QStringLiteral("card??")},
                                             QDir::Dirs | QDir::System)) {
        const QString device = drm.filePath(card) + QStringLiteral("/device");
        QFile vendor(device + QStringLiteral("/vendor"));
        QFile id(device + QStringLiteral("/device"));
        if (!vendor.open(QIODevice::ReadOnly) || !id.open(QIODevice::ReadOnly)) continue;
        Log("gpu %s: vendor %s device %s, driver %s", qUtf8Printable(card),
            vendor.readAll().trimmed().constData(), id.readAll().trimmed().constData(),
            qUtf8Printable(QFileInfo(QFileInfo(device + QStringLiteral("/driver")).symLinkTarget())
                                   .fileName()));
    }
    QFile nvidia(QStringLiteral("/proc/driver/nvidia/version"));
    if (nvidia.open(QIODevice::ReadOnly)) {
        Log("nvidia driver: %s", nvidia.readLine().trimmed().constData());
    }
    QFile amdgpu(QStringLiteral("/sys/module/amdgpu/version"));
    if (amdgpu.open(QIODevice::ReadOnly)) {
        Log("amdgpu module: %s", amdgpu.readAll().trimmed().constData());
    }
}

#endif

void Terminated() {
    const std::exception_ptr current = std::current_exception();
    if (current) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &error) {
            Log("terminate: uncaught exception: %s", error.what());
        } catch (...) {
            Log("terminate: uncaught exception of unknown type");
        }
    } else {
        Log("terminate called without an exception");
    }
#if defined(_WIN32)
    RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
    std::abort();
}

}  // namespace

bool StartDiagnostics() {
    logsFolder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                 QStringLiteral("/logs");
    QDir folder(logsFolder);
    if (!folder.mkpath(QStringLiteral("."))) return false;
    InspectPreviousLog(folder);
    PruneLogs(folder);
    const QString stem = QStringLiteral("ForeverTAS-%1-%2")
                                 .arg(QDateTime::currentDateTime().toString(
                                              QStringLiteral("yyyy-MM-dd_HH-mm-ss")))
                                 .arg(QCoreApplication::applicationPid());
    if (!diagnostics::OpenLog(QDir::toNativeSeparators(folder.filePath(stem + QStringLiteral(".log")))
                                      .toStdString())) {
        return false;
    }
#if defined(_WIN32)
    dumpPath = QDir::toNativeSeparators(folder.filePath(stem + QStringLiteral(".dmp"))).toStdWString();
    SetUnhandledExceptionFilter(Crash);
#else
    // A stack of its own, so a stack overflow is reported too; backtrace()
    // once now, so the crash handler does not load its library.
    static std::vector<char> alternateStack(64 * 1024);
    stack_t stack{};
    stack.ss_sp = alternateStack.data();
    stack.ss_size = alternateStack.size();
    sigaltstack(&stack, nullptr);
    void *frames[4];
    backtrace(frames, 4);
    struct sigaction action{};
    action.sa_sigaction = Crash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        sigaction(signal, &action, nullptr);
    }
    struct sigaction current{};
    if (sigaction(SIGTERM, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
        ::signal(SIGTERM, Terminate);
    }
#endif
    std::set_terminate(Terminated);
    previousMessageHandler = qInstallMessageHandler(QtMessage);

    Log("ForeverTAS %s, log %s", FOREVERTAS_VERSION, diagnostics::LogPath().c_str());
    if (!previousLog.isEmpty()) {
        Log("the previous session did not close normally%s: %s",
            previousCrashed ? " (it crashed)" : "", qUtf8Printable(previousLog));
    }
    Log("system: %s, kernel %s %s, %s", qUtf8Printable(QSysInfo::prettyProductName()),
        qUtf8Printable(QSysInfo::kernelType()), qUtf8Printable(QSysInfo::kernelVersion()),
        qUtf8Printable(QSysInfo::currentCpuArchitecture()));
    Log("qt %s, platform %s", qVersion(), qUtf8Printable(QGuiApplication::platformName()));
#if defined(_WIN32)
    LogSystem();
    LogAdapters();
    LogRuntimeFiles();
#else
    LogLinuxSystem();
#endif
    static const char *const environment[] = {
            "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY", "DISPLAY", "QT_QPA_PLATFORM",
            "QT_SCALE_FACTOR", "QSG_RHI_BACKEND", "QT_QUICK_BACKEND", "APPIMAGE", "CUDA_VISIBLE_DEVICES",
            "HIP_VISIBLE_DEVICES", "HSA_OVERRIDE_GFX_VERSION", "FOREVERTAS_RENDER_MODULE", nullptr};
    LogEnvironment(environment);
    for (QScreen *screen : QGuiApplication::screens()) {
        Log("screen %s: %dx%d at %.2fx, %.0f Hz", qUtf8Printable(screen->name()),
            screen->size().width(), screen->size().height(), screen->devicePixelRatio(),
            screen->refreshRate());
    }
    return true;
}

void LogLoadedGpuRuntimes() {
    if (!diagnostics::LogOpen()) return;
#if defined(_WIN32)
    HMODULE modules[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return;
    for (DWORD index = 0; index < needed / sizeof(HMODULE) && index < 512; ++index) {
        wchar_t name[MAX_PATH];
        GetModuleFileNameW(modules[index], name, MAX_PATH);
        std::wstring lower = name;
        for (wchar_t &c : lower) c = static_cast<wchar_t>(towlower(c));
        for (const wchar_t *part : {L"amdhip", L"hiprtc", L"comgr", L"nvcuda", L"cudart",
                                    L"vulkan-1", L"render-module"}) {
            if (lower.find(part) != std::wstring::npos) {
                Log("loaded %s: version %s", Narrow(name).c_str(), FileVersion(name).c_str());
                break;
            }
        }
    }
#else
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) return;
    QStringList seen;
    for (const QByteArray &line : maps.readAll().split('\n')) {
        const qsizetype slash = line.indexOf('/');
        if (slash < 0) continue;
        const QString file = QString::fromUtf8(line.mid(slash));
        const QString name = QFileInfo(file).fileName();
        if (seen.contains(file)) continue;
        for (const char *part : {"libcuda", "libamdhip", "libhsa-runtime", "libvulkan", "render-module",
                                 "libGLX_", "libEGL_", "libnvidia-glcore"}) {
            if (name.startsWith(QLatin1String(part)) || name.contains(QLatin1String(part))) {
                seen.append(file);
                Log("loaded %s", qUtf8Printable(file));
                break;
            }
        }
    }
#endif
}

void LogQmlSetup(const QQmlApplicationEngine &engine) {
    if (!diagnostics::LogOpen()) return;
    Log("qml import paths: %s", qUtf8Printable(engine.importPathList().join(QStringLiteral("; "))));
    static const char *const environment[] = {"QML_IMPORT_PATH", "QML2_IMPORT_PATH", "QT_QUICK_CONTROLS_STYLE",
                                              "QML_DISABLE_DISK_CACHE", "QT_PLUGIN_PATH", nullptr};
    LogEnvironment(environment);
#if defined(_WIN32)
    HMODULE modules[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) {
        for (DWORD index = 0; index < needed / sizeof(HMODULE) && index < 512; ++index) {
            wchar_t name[MAX_PATH];
            GetModuleFileNameW(modules[index], name, MAX_PATH);
            std::wstring lower = name;
            for (wchar_t &c : lower) c = static_cast<wchar_t>(towlower(c));
            if (lower.find(L"forevertas_qml") != std::wstring::npos) {
                Log("qml plugin loaded: %s, modified %s", Narrow(name).c_str(),
                    qUtf8Printable(QFileInfo(QString::fromWCharArray(name))
                                           .lastModified().toString(Qt::ISODate)));
            }
        }
    }
#else
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (maps.open(QIODevice::ReadOnly)) {
        QStringList seen;
        for (const QByteArray &line : maps.readAll().split('\n')) {
            const qsizetype slash = line.indexOf('/');
            if (slash < 0) continue;
            const QString file = QString::fromUtf8(line.mid(slash));
            if (!file.contains(QLatin1String("forevertas_qml")) || seen.contains(file)) continue;
            seen.append(file);
            Log("qml plugin loaded: %s", qUtf8Printable(file));
        }
    }
#endif
    if (QObject *const root = engine.rootObjects().value(0)) {
        const QQmlContext *const context = qmlContext(root);
        Log("qml main: %s", context != nullptr ? qUtf8Printable(context->baseUrl().toString()) : "?");
    }
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/qmlcache");
    const QDir cacheDir(cache);
    Log("qml cache %s: %s", qUtf8Printable(cache),
        cacheDir.exists() ? qUtf8Printable(QStringLiteral("%1 files").arg(cacheDir.entryList(QDir::Files).size()))
                          : "none");
}

void WatchWindow(QQuickWindow *window) {
    if (window == nullptr) return;
    QObject::connect(window, &QQuickWindow::sceneGraphError, window,
                     [](QQuickWindow::SceneGraphError error, const QString &message) {
                         Log("window: scene graph error %d: %s", static_cast<int>(error),
                             qUtf8Printable(message));
                     });
    QObject::connect(window, &QQuickWindow::sceneGraphInitialized, window, [window] {
        const QSGRendererInterface *const renderer = window->rendererInterface();
        static const char *const apis[] = {"unknown", "software", "OpenVG", "OpenGL", "Direct3D 11",
                                           "Vulkan", "Metal", "null", "Direct3D 12"};
        const int api = renderer != nullptr ? static_cast<int>(renderer->graphicsApi()) : 0;
        Log("window: graphics %s", api >= 0 && api < 9 ? apis[api] : "?");
#if defined(FOREVERTAS_HAS_RHI_INFO)
        if (QRhi *const rhi = window->rhi()) {
            const QRhiDriverInfo info = rhi->driverInfo();
            Log("window: device %s, vendor %llx device %llx", info.deviceName.constData(),
                static_cast<unsigned long long>(info.vendorId),
                static_cast<unsigned long long>(info.deviceId));
        }
#endif
        if (QOpenGLContext *const context = QOpenGLContext::currentContext()) {
            QOpenGLFunctions *const gl = context->functions();
            const auto text = [gl](GLenum name) {
                const GLubyte *const value = gl->glGetString(name);
                return value != nullptr ? reinterpret_cast<const char *>(value) : "?";
            };
            Log("window: OpenGL %s, %s, %s", text(GL_VERSION), text(GL_RENDERER), text(GL_VENDOR));
        }
    }, Qt::DirectConnection);
    QObject::connect(window, &QWindow::screenChanged, window, [](QScreen *screen) {
        if (screen != nullptr) {
            Log("window: on screen %s (%.2fx)", qUtf8Printable(screen->name()),
                screen->devicePixelRatio());
        }
    });
    QObject::connect(window, &QWindow::visibilityChanged, window, [](QWindow::Visibility visibility) {
        Log("window: visibility %d", static_cast<int>(visibility));
    });
}

void FinishDiagnostics() {
    diagnostics::LogCleanExit();
}

DiagnosticsLogs::DiagnosticsLogs(QObject *parent)
    : QObject(parent), previousSessionLog_(previousLog), previousSessionCrashed_(previousCrashed) {}

QString DiagnosticsLogs::folder() const {
    return QDir::toNativeSeparators(logsFolder);
}

QString DiagnosticsLogs::logPath() const {
    return QString::fromStdString(diagnostics::LogPath());
}

bool DiagnosticsLogs::openFolder() const {
    Log("ui: open log folder");
    return QDesktopServices::openUrl(QUrl::fromLocalFile(logsFolder));
}

void DiagnosticsLogs::dismissPreviousSession() {
    if (previousSessionLog_.isEmpty()) return;
    previousSessionLog_.clear();
    previousSessionCrashed_ = false;
    emit previousSessionChanged();
}

void DiagnosticsLogs::note(const QString &text) const {
    Log("ui: %s", qUtf8Printable(text));
}

void DiagnosticsLogs::keepPicture(const QVariant &image) {
    keptPicture_ = image.value<QImage>().convertToFormat(QImage::Format_ARGB32);
}

int DiagnosticsLogs::differingPixels(const QVariant &image) const {
    const QImage &a = keptPicture_;
    const QImage b = image.value<QImage>().convertToFormat(QImage::Format_ARGB32);
    if (a.isNull() || a.size() != b.size()) {
        Log("pixel comparison impossible: %dx%d against %dx%d", a.width(), a.height(), b.width(),
            b.height());
        return -1;
    }
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *rowA = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *rowB = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            // a few levels of noise (antialiasing, dithering) are not the car
            const QRgb pa = rowA[x];
            const QRgb pb = rowB[x];
            if (std::abs(qRed(pa) - qRed(pb)) + std::abs(qGreen(pa) - qGreen(pb)) +
                        std::abs(qBlue(pa) - qBlue(pb)) > 24) {
                ++count;
            }
        }
    }
    return count;
}

QVariantMap DiagnosticsLogs::carPixels(const QVariant &image, const QPointF &center, qreal radius,
                                       const QColor &color) const {
    float hue = 0.0f, saturation = 0.0f, value = 0.0f;
    color.getHsvF(&hue, &saturation, &value);
    if (hue < 0.0f || saturation < 0.3f) {
        return {{QStringLiteral("skipped"), QStringLiteral("the car's color has no hue to look for")}};
    }
    const QImage picture = image.value<QImage>();
    if (picture.isNull()) return {{QStringLiteral("skipped"), QStringLiteral("no picture")}};
    const int left = std::max(0, static_cast<int>(std::floor(center.x() - radius)));
    const int top = std::max(0, static_cast<int>(std::floor(center.y() - radius)));
    const int right = std::min(picture.width() - 1, static_cast<int>(std::ceil(center.x() + radius)));
    const int bottom = std::min(picture.height() - 1, static_cast<int>(std::ceil(center.y() + radius)));
    int samples = 0;
    int matches = 0;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const qreal dx = x - center.x(), dy = y - center.y();
            if (dx * dx + dy * dy > radius * radius) continue;
            ++samples;
            float h = 0.0f, sat = 0.0f, val = 0.0f;
            QColor(picture.pixel(x, y)).getHsvF(&h, &sat, &val);
            // tone mapping moves the hue a little (orange 24 -> 35 degrees)
            const float turn = std::fabs(h - hue);
            const float distance = std::min(turn, 1.0f - turn) * 360.0f;
            if (h >= 0.0f && distance <= 25.0f && sat >= 0.3f && val >= 0.15f) ++matches;
        }
    }
    if (samples == 0) return {{QStringLiteral("skipped"), QStringLiteral("the car is off the picture")}};
    return {{QStringLiteral("fraction"), static_cast<double>(matches) / samples},
            {QStringLiteral("samples"), samples}};
}

QString DiagnosticsLogs::saveKeptPicture(const QString &suffix) const {
    return savePicture(QVariant::fromValue(keptPicture_), suffix);
}

QString DiagnosticsLogs::savePicture(const QVariant &image, const QString &suffix) const {
    QString path = QString::fromStdString(diagnostics::LogPath());
    if (path.isEmpty()) return {};
    path.chop(4);   // ".log"
    path += QStringLiteral("-") + suffix + QStringLiteral(".png");
    return image.value<QImage>().save(path) ? path : QString();
}

}  // namespace forevertas::app
