#include "viewer/debugger_tools.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("FOREVERTAS_TEST_PROBE_HELPER")) {
        if (qEnvironmentVariable("FOREVERTAS_TEST_PROBE_HELPER") == QLatin1String("hang")) QThread::msleep(10000);
        else std::cout << "FOREVERTAS_DEBUGGER_READY\n";
        return 0;
    }
    using namespace forevertas::viewer;
    QTemporaryDir root;
    const auto copy = [&](const QString &relative) {
        const auto path = root.filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::copy(QCoreApplication::applicationFilePath(), path);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return path;
    };
    const QString name = QStringLiteral("lldb-test.exe");
    const auto bundled = copy("app/debugger/" + name);
    const auto onPath = copy("path/" + name);
    const auto custom = copy("custom ' path/" + name);
    const auto application = root.filePath("app");
    const QStringList paths{root.filePath("path")};
    bool okay = ResolveDebuggerExecutable({}, name, application, paths) == bundled &&
                ResolveDebuggerExecutable(custom, name, application, paths) == custom &&
                ResolveDebuggerExecutable(root.filePath("missing"), name, application, paths).isEmpty();
    QFile::remove(bundled);
    okay = ResolveDebuggerExecutable({}, name, application, paths) == onPath && okay;
    const auto linuxArguments = DebuggerTerminalArguments(DebuggerTerminalPlatform::Linux, custom, {"--no-lldbinit"});
    const auto mac = DebuggerTerminalArguments(DebuggerTerminalPlatform::Mac, custom, {"--no-lldbinit"});
    const auto windows = DebuggerTerminalArguments(DebuggerTerminalPlatform::Windows, custom, {"--no-lldbinit"});
    okay = linuxArguments.size() == 3 && linuxArguments[0] == "-qefc" && linuxArguments[1].contains("'\\''") &&
            mac == QStringList{"-q", "/dev/null", custom, "--no-lldbinit"} &&
            windows == QStringList{"-Xallow-non-tty", "-Xplain", custom, "--no-lldbinit"} && okay;
    DebuggerTools tools{QCoreApplication::applicationFilePath(), QCoreApplication::applicationFilePath(), {}};
    qputenv("FOREVERTAS_TEST_PROBE_HELPER", "ready");
    okay = ProbeDebuggerTools(tools, custom).isEmpty() && okay;
    okay = !ProbeDebuggerTools(tools, root.filePath("missing")).isEmpty() && okay;
    qputenv("FOREVERTAS_TEST_PROBE_HELPER", "hang");
    QElapsedTimer timer;
    timer.start();
    okay = !ProbeDebuggerTools(tools, custom, 100).isEmpty() && timer.elapsed() < 2000 && okay;
    qunsetenv("FOREVERTAS_TEST_PROBE_HELPER");
    if (!okay) std::cerr << "debugger tool discovery, platform arguments, capability check or timeout failed\n";
    return okay ? 0 : 1;
}
