#pragma once

#include <QString>
#include <QStringList>

namespace forevertas::viewer {

enum class DebuggerTerminalPlatform { Linux, Mac, Windows };
DebuggerTerminalPlatform NativeDebuggerTerminalPlatform();
struct DebuggerTools {
    QString lldb;
    QString terminal;
    QString error;
};

QString ResolveDebuggerExecutable(const QString &overridePath, const QString &name,
                                  const QString &applicationDirectory, const QStringList &searchPath);
DebuggerTools DiscoverDebuggerTools();
QStringList DebuggerTerminalArguments(DebuggerTerminalPlatform platform, const QString &lldb,
                                      const QStringList &arguments);
QString ProbeDebuggerTools(const DebuggerTools &tools, const QString &worker, int timeoutMs = 5000);

}
