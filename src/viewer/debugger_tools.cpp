#include "viewer/debugger_tools.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>

namespace forevertas::viewer {

DebuggerTerminalPlatform NativeDebuggerTerminalPlatform() {
#if defined(Q_OS_WIN)
    return DebuggerTerminalPlatform::Windows;
#elif defined(Q_OS_MACOS)
    return DebuggerTerminalPlatform::Mac;
#else
    return DebuggerTerminalPlatform::Linux;
#endif
}

QString ResolveDebuggerExecutable(const QString &overridePath, const QString &name,
        const QString &applicationDirectory, const QStringList &searchPath) {
    const auto executable = [](const QString &path) {
        const QFileInfo file(path);
        return file.isFile() && file.isExecutable() ? file.absoluteFilePath() : QString{};
    };
    if (!overridePath.trimmed().isEmpty()) return executable(overridePath.trimmed());
    for (const auto &relative : {QStringLiteral("debugger/"), QString{}, QStringLiteral("bin/")}) {
        const auto found = executable(QDir(applicationDirectory).filePath(relative + name));
        if (!found.isEmpty()) return found;
    }
    return searchPath.isEmpty() ? QString{} : QStandardPaths::findExecutable(name, searchPath);
}

DebuggerTools DiscoverDebuggerTools() {
    DebuggerTools tools;
    const auto platform = NativeDebuggerTerminalPlatform();
    const auto path = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    const auto configured = [](const char *environment, const char *setting) {
        return qEnvironmentVariableIsSet(environment) ? qEnvironmentVariable(environment)
                : QSettings().value(QLatin1String(setting)).toString();
    };
    const auto lldb = configured("FOREVERTAS_DEBUG_LLDB", "simulationDebugger/lldbPath");
    const auto terminal = configured("FOREVERTAS_DEBUG_TERMINAL", "simulationDebugger/terminalPath");
    tools.lldb = ResolveDebuggerExecutable(lldb, platform == DebuggerTerminalPlatform::Windows
            ? QStringLiteral("lldb.exe") : QStringLiteral("lldb"), QCoreApplication::applicationDirPath(), path);
    tools.terminal = ResolveDebuggerExecutable(terminal, platform == DebuggerTerminalPlatform::Windows
            ? QStringLiteral("winpty.exe") : QStringLiteral("script"), QCoreApplication::applicationDirPath(), path);
    if (tools.lldb.isEmpty()) {
        tools.error = QStringLiteral("LLDB executable not found or not executable. Select a valid LLDB path in Debugger tools, or install LLVM LLDB on PATH.");
        if (!lldb.isEmpty()) tools.error += QStringLiteral(" Configured path: ") + lldb;
    } else if (tools.terminal.isEmpty()) {
        tools.error = platform == DebuggerTerminalPlatform::Windows
                ? QStringLiteral("Terminal bridge unavailable. Select winpty.exe from a complete MSYS2/Cygwin winpty installation (including its DLLs and agent).")
                : QStringLiteral("Terminal bridge unavailable. Select the system script executable in Debugger tools or install it on PATH.");
        if (!terminal.isEmpty()) tools.error += QStringLiteral(" Configured path: ") + terminal;
    }
    return tools;
}

QStringList DebuggerTerminalArguments(DebuggerTerminalPlatform platform, const QString &lldb,
        const QStringList &arguments) {
    if (platform == DebuggerTerminalPlatform::Windows)
        return QStringList{QStringLiteral("-Xallow-non-tty"), QStringLiteral("-Xplain"), lldb} + arguments;
    if (platform == DebuggerTerminalPlatform::Mac)
        return QStringList{QStringLiteral("-q"), QStringLiteral("/dev/null"), lldb} + arguments;
    const auto quote = [](QString value) {
        value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + value + QLatin1Char('\'');
    };
    QString command = quote(lldb);
    for (const auto &argument : arguments) command += QLatin1Char(' ') + quote(argument);
    return {QStringLiteral("-qefc"), command, QStringLiteral("/dev/null")};
}

QString ProbeDebuggerTools(const DebuggerTools &tools, const QString &worker, int timeoutMs) {
    if (!tools.error.isEmpty()) return tools.error;
    if (!QFileInfo(worker).isFile() || !QFileInfo(worker).isExecutable())
        return QStringLiteral("Reference debug worker is missing or not executable: %1. Reinstall the matching application package.").arg(worker);
    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    probe.setProcessEnvironment(environment);
    const QStringList arguments{QStringLiteral("--no-lldbinit"), QStringLiteral("--no-use-colors"),
            QStringLiteral("--batch"), QStringLiteral("--one-line"),
            QStringLiteral("script import json; print('FOREVERTAS_' + 'DEBUGGER_READY')")};
    probe.start(tools.terminal, DebuggerTerminalArguments(NativeDebuggerTerminalPlatform(), tools.lldb, arguments));
    if (!probe.waitForFinished(timeoutMs)) {
        const auto error = probe.errorString();
        probe.kill();
        probe.waitForFinished(1000);
        return QStringLiteral("Debugger capability probe timed out or could not start: %1. Check the LLDB/terminal paths and runtime dependencies.").arg(error);
    }
    const QString output = QString::fromLocal8Bit(probe.readAll()).trimmed();
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0 ||
        !output.contains(QStringLiteral("FOREVERTAS_DEBUGGER_READY")))
        return QStringLiteral("Debugger capability probe failed. LLDB must start through the terminal bridge with Python support. Check runtime dependencies.\n%1").arg(output.right(2000));
    return {};
}

}
