#include "viewer/simulation_debugger_model.h"
#include "viewer/debugger_tools.h"

#include "simulation_debug_sources.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <utility>

void InitializeSimulationDebugResources() {
    Q_INIT_RESOURCE(simulation_debug_sources);
}

namespace forevertas::viewer {
namespace {

QString HtmlEscape(QString text) {
    return text.replace(QLatin1Char('&'), QStringLiteral("&amp;"))
            .replace(QLatin1Char('<'), QStringLiteral("&lt;"))
            .replace(QLatin1Char('>'), QStringLiteral("&gt;"));
}

QString TrimDebuggerNoise(QString diagnostics) {
    QStringList kept;
    for (const QString &line : diagnostics.split(QLatin1Char('\n'))) {
        if (line.contains(QStringLiteral("DW_TAG_member '_M_t'")) ||
            line.trimmed().isEmpty()) {
            continue;
        }
        kept.push_back(line);
    }
    return kept.join(QLatin1Char('\n')).trimmed();
}

QVariantList JsonVector(const QJsonValue &value) {
    QVariantList result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const QJsonValue &component : array) {
        result.push_back(component.toDouble());
    }
    return result;
}

bool IsExplicitPrintExpression(const QString &expression) {
    QString code = expression;
    bool quoted = false;
    bool blockComment = false;
    bool escaped = false;
    QChar quote;
    for (qsizetype index = 0; index < code.size(); ++index) {
        const QChar character = code[index];
        if (blockComment) {
            code[index] = QLatin1Char(' ');
            if (character == QLatin1Char('*') &&
                index + 1 < code.size() &&
                code[index + 1] == QLatin1Char('/')) {
                code[index + 1] = QLatin1Char(' ');
                ++index;
                blockComment = false;
            }
            continue;
        }
        if (quoted) {
            code[index] = QLatin1Char(' ');
            if (escaped) {
                escaped = false;
            } else if (character == QLatin1Char('\\')) {
                escaped = true;
            } else if (character == quote) {
                quoted = false;
            }
            continue;
        }
        if (character == QLatin1Char('"') ||
            character == QLatin1Char('\'')) {
            quoted = true;
            quote = character;
            code[index] = QLatin1Char(' ');
        } else if (character == QLatin1Char('/') &&
                   index + 1 < code.size() &&
                   code[index + 1] == QLatin1Char('/')) {
            code.truncate(index);
            break;
        } else if (character == QLatin1Char('/') &&
                   index + 1 < code.size() &&
                   code[index + 1] == QLatin1Char('*')) {
            code[index] = QLatin1Char(' ');
            code[index + 1] = QLatin1Char(' ');
            ++index;
            blockComment = true;
        }
    }
    static const QRegularExpression printPattern(QStringLiteral(
            "\\b(?:__builtin_)?(?:printf|fprintf|puts|fputs)\\s*\\(|"
            "\\bstd::(?:printf|fprintf|puts|cout|cerr|clog)\\b|"
            "\\bq(?:Debug|Info|Warning|Critical)\\s*\\("));
    return printPattern.match(code).hasMatch();
}

bool IsEditableSourceLine(const QString &text) {
    const QString trimmed = text.trimmed();
    return !trimmed.isEmpty() &&
           !trimmed.startsWith(QStringLiteral("//")) &&
           !trimmed.startsWith(QLatin1Char('#')) &&
           trimmed != QStringLiteral("{") && trimmed != QStringLiteral("}");
}

} // namespace

SimulationDebuggerModel::SimulationDebuggerModel(QObject *parent)
    : QObject(parent) {
    InitializeSimulationDebugResources();
    loadWatches();
    expandedFolders_.insert(QStringLiteral("src"));
    expandedFolders_.insert(QStringLiteral("src/simulation"));
    expandedFolders_.insert(QStringLiteral("src/simulation/runtime"));
    expandedFolders_.insert(QStringLiteral("src/engine"));
    expandedFolders_.insert(QStringLiteral("src/engine/physics"));
    debugger_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&debugger_, &QProcess::readyReadStandardOutput, this,
            &SimulationDebuggerModel::readDebuggerOutput);
    connect(&debugger_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
                if (!stopping_ && active_) {
                    failSession(
                            QStringLiteral("LLDB could not run the reference "
                                           "simulation."));
                }
            });
    connect(&debugger_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                if (stopping_) {
                    stopping_ = false;
                    if (pendingStart_) {
                        QTimer::singleShot(
                                0, this,
                                &SimulationDebuggerModel::beginPendingSession);
                    } else {
                        QFile::remove(inputScriptPath_);
                        inputScriptPath_.clear();
                    }
                    return;
                }
                if (!active_) {
                    return;
                }
                setRunning(false);
                active_ = false;
                QFile::remove(inputScriptPath_);
                inputScriptPath_.clear();
                cancelStep();
                clearExecutionLocation();
                if (status == QProcess::NormalExit && exitCode == 0) {
                    setStatus(
                            QStringLiteral("Reference simulation completed."));
                    emit sessionFinished();
                } else {
                    failSession(
                            QStringLiteral("The reference simulation debugger "
                                           "stopped unexpectedly."));
                }
                emit stateChanged();
            });
    loadSources();
}

SimulationDebuggerModel::~SimulationDebuggerModel() {
    stopSession();
    if (debugger_.state() != QProcess::NotRunning) {
        debugger_.disconnect(this);
        debugger_.kill();
    }
    QFile::remove(inputScriptPath_);
}

bool SimulationDebuggerModel::validWatchPath(const QString &path) {
    static const QRegularExpression pattern(QStringLiteral(
            "^[A-Za-z_][A-Za-z_0-9]*(?:(?:\\.|->)[A-Za-z_][A-Za-z_0-9]*|\\[[0-9]{1,10}\\])*$"));
    return !path.isEmpty() && path.size() <= 256 && pattern.match(path).hasMatch();
}

QString SimulationDebuggerModel::pinWatch(const QString &expression) {
    const auto path = expression.trimmed();
    if (!validWatchPath(path)) {
        watchError_ = QStringLiteral("Use a variable, member, or numeric array index; calls and assignments are not allowed.");
        emit watchesChanged();
        return {};
    }
    for (const auto &watch : watches_) {
        if (watch.toMap().value(QStringLiteral("expression")) != path) continue;
        watchError_.clear();
        emit watchesChanged();
        return watch.toMap().value(QStringLiteral("id")).toString();
    }
    if (watches_.size() >= 32) {
        watchError_ = QStringLiteral("The watch limit is 32.");
        emit watchesChanged();
        return {};
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    watches_.append(QVariantMap{{"id", id}, {"expression", path}, {"value", QString{}},
                               {"type", QString{}}, {"status", QStringLiteral("Stopped")}});
    watchError_.clear();
    saveWatches();
    refreshWatches();
    return id;
}

void SimulationDebuggerModel::unpinWatch(const QString &id) {
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [&id](const QVariant &watch) {
        return watch.toMap().value(QStringLiteral("id")) == id;
    }), watches_.end());
    watchError_.clear();
    saveWatches();
    refreshWatches();
}

void SimulationDebuggerModel::saveWatches() {
    QJsonArray persisted;
    for (const auto &watch : watches_) {
        const auto row = watch.toMap();
        persisted.append(QJsonObject{{"id", row.value("id").toString()}, {"expression", row.value("expression").toString()}});
    }
    QSettings().setValue(QStringLiteral("simulationDebugger/watchesV1"), QJsonDocument(persisted).toJson(QJsonDocument::Compact));
}

void SimulationDebuggerModel::loadWatches() {
    const auto persisted = QJsonDocument::fromJson(QSettings().value(QStringLiteral("simulationDebugger/watchesV1")).toByteArray()).array();
    QSet<QString> ids, paths;
    for (const auto &value : persisted) {
        const auto row = value.toObject();
        const auto path = row.value("expression").toString();
        const auto id = row.value("id").toString();
        if (watches_.size() >= 32) break;
        if (!validWatchPath(path) || QUuid(id).isNull() || ids.contains(id) || paths.contains(path)) continue;
        ids.insert(id);
        paths.insert(path);
        watches_.append(QVariantMap{{"id", id}, {"expression", path}, {"value", QString{}},
                                   {"type", QString{}}, {"status", QStringLiteral("Stopped")}});
    }
}

void SimulationDebuggerModel::invalidateWatches(const QString &status) {
    ++watchGeneration_;
    commandQueue_.erase(std::remove_if(commandQueue_.begin(), commandQueue_.end(), [](const DebuggerCommand &command) {
        return command.kind == CommandKind::Watch;
    }), commandQueue_.end());
    for (auto &watch : watches_) {
        auto row = watch.toMap();
        row.insert(QStringLiteral("value"), QString{});
        row.insert(QStringLiteral("type"), QString{});
        row.insert(QStringLiteral("status"), status);
        row.remove(QStringLiteral("location"));
        watch = row;
    }
    emit watchesChanged();
}

void SimulationDebuggerModel::refreshWatches() {
    const bool pausedFrame = active_ && !running_ && !stepping_ && !compiling_ &&
                             !pauseRequested_ && activeSourceLine_ > 0 && !activeFilePath_.isEmpty();
    invalidateWatches(pausedFrame ? QStringLiteral("Evaluating") :
                      active_ ? QStringLiteral("Waiting for pause") : QStringLiteral("Stopped"));
    if (!pausedFrame) return;
    for (const auto &watch : watches_) {
        const auto row = watch.toMap();
        DebuggerCommand command;
        command.kind = CommandKind::Watch;
        // frame variable reads debug information/memory; it does not evaluate C++ or call the inferior.
        command.text = QStringLiteral("frame variable --show-types --raw-output --dynamic-type no-dynamic-values --depth 0 --ptr-depth 0 -- %1")
                .arg(row.value(QStringLiteral("expression")).toString());
        command.watchId = row.value(QStringLiteral("id")).toString();
        command.watchGeneration = watchGeneration_;
        command.sourcePath = activeFilePath_;
        command.sourceLine = activeSourceLine_;
        commandQueue_.enqueue(std::move(command));
    }
    sendNextCommand();
}

bool SimulationDebuggerModel::available() const {
    return available_;
}
bool SimulationDebuggerModel::preparing() const {
    return preparing_;
}

bool SimulationDebuggerModel::loadingReplay() const {
    return active_ &&
           statusText_ == QStringLiteral(
                                  "Loading scenario in the Reference engine...");
}
bool SimulationDebuggerModel::active() const {
    return active_;
}
bool SimulationDebuggerModel::running() const {
    return running_;
}
bool SimulationDebuggerModel::stepping() const {
    return stepping_;
}
bool SimulationDebuggerModel::compiling() const {
    return compiling_;
}
bool SimulationDebuggerModel::canStepSource() const {
    return active_ && workerReady_ && !running_ && !compiling_ && !stepping_ &&
           activeLine_ > 0 && sourceIndex(activeFilePath_) >= 0;
}
bool SimulationDebuggerModel::canStepTick() const {
    return active_ && workerReady_ && !running_ && !compiling_ && !stepping_;
}
QString SimulationDebuggerModel::backendName() const {
    return backendName_;
}
QString SimulationDebuggerModel::selectedFilePath() const {
    return selectedFilePath_;
}
int SimulationDebuggerModel::activeLine() const {
    return activeLine_;
}
QString SimulationDebuggerModel::activeFilePath() const {
    return activeFilePath_;
}
QString SimulationDebuggerModel::statusText() const {
    return statusText_;
}
QString SimulationDebuggerModel::editError() const {
    return editError_;
}
qint64 SimulationDebuggerModel::executionTick() const {
    return executionTick_;
}

bool SimulationDebuggerModel::darkMode() const {
    return darkMode_;
}

void SimulationDebuggerModel::setDarkMode(bool value) {
    if (darkMode_ == value) {
        return;
    }
    darkMode_ = value;
    emit themeChanged();
    emit linesChanged();
}

bool SimulationDebuggerModel::hasEdits() const {
    return std::any_of(
            sources_.begin(), sources_.end(),
            [](const SourceFile &source) {
                return !source.edits.isEmpty() ||
                       !source.deletedSourceLines.isEmpty();
            });
}

QVariantList SimulationDebuggerModel::fileEntries() const {
    return visibleFileEntries();
}

QVariantList SimulationDebuggerModel::lines() const {
    QVariantList result;
    const SourceFile *const source = selectedSource();
    if (source == nullptr) {
        return result;
    }
    result.reserve(source->currentLines.size());
    for (int index = 0; index < source->currentLines.size(); ++index) {
        const int line = index + 1;
        const quint64 lineId = source->lineIds[index];
        const int originalSourceLine = source->sourceLineNumbers[index];
        const QString &text = source->currentLines[index];
        const QString original =
                originalSourceLine > 0 &&
                                originalSourceLine <=
                                        source->originalLines.size()
                        ? source->originalLines[originalSourceLine - 1]
                        : QString();
        const bool sourceLine = IsEditableSourceLine(text);
        result.push_back(QVariantMap{
                {QStringLiteral("number"), line},
                {QStringLiteral("lineId"),
                 static_cast<qulonglong>(lineId)},
                {QStringLiteral("text"), text},
                {QStringLiteral("original"), original},
                {QStringLiteral("highlighted"),
                 (darkMode_ ? source->highlightedDark
                            : source->highlightedLight)[index]},
                {QStringLiteral("active"),
                 active_ && source->path == activeFilePath_ &&
                         line == activeLine_},
                {QStringLiteral("breakpoint"),
                 source->breakpoints.contains(lineId)},
                {QStringLiteral("modified"),
                 source->edits.contains(lineId)},
                {QStringLiteral("inserted"), originalSourceLine == 0},
                {QStringLiteral("editable"),
                 sourceLine || originalSourceLine == 0},
                {QStringLiteral("executable"),
                 source->executableSourceLines.contains(
                         source->anchorLineNumbers[index]) ||
                         source->breakpoints.contains(lineId) ||
                         (source->path == activeFilePath_ &&
                          line == activeLine_)},
                {QStringLiteral("inlineValue"),
                 source->inlineValueLines[index]}});
    }
    return result;
}

QVariantList SimulationDebuggerModel::debugOutput() const {
    return debugOutput_;
}

bool SimulationDebuggerModel::selectFile(const QString &path) {
    if (sourceIndex(path) < 0) {
        return false;
    }
    if (selectedFilePath_ == path) {
        return true;
    }
    selectedFilePath_ = path;
    emit selectionChanged();
    emit linesChanged();
    return true;
}

void SimulationDebuggerModel::toggleFolder(const QString &path) {
    if (expandedFolders_.contains(path)) {
        expandedFolders_.remove(path);
    } else {
        expandedFolders_.insert(path);
    }
    emit filesChanged();
}

bool SimulationDebuggerModel::updateLine(int lineNumber, const QString &text) {
    SourceFile *const source = selectedSource();
    if (source == nullptr || lineNumber <= 0 ||
        lineNumber > source->currentLines.size()) {
        return false;
    }
    if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) {
        editError_ = QStringLiteral(
                "A live edit must contain exactly one source line.");
        emit linesChanged();
        return false;
    }
    const int index = lineNumber - 1;
    if (source->currentLines[index] == text) {
        return true;
    }
    const quint64 lineId = source->lineIds[index];
    const int sourceLine = source->sourceLineNumbers[index];
    const int anchorLine = source->anchorLineNumbers[index];
    source->currentLines[index] = text;
    ++sourceRevision_;
    source->highlightedLight[index] = syntaxHighlighted(text, false);
    source->highlightedDark[index] = syntaxHighlighted(text, true);
    source->inlineValueLines[index] = inlineValues(text);
    const QString original =
            sourceLine > 0 && sourceLine <= source->originalLines.size()
                    ? source->originalLines[sourceLine - 1]
                    : QString();
    if (sourceLine > 0 && text == original) {
        source->edits.remove(lineId);
    } else {
        const QString key = lineKey(source->path, anchorLine);
        const qint64 effectiveTick =
                active_ && executedLinesThisTick_.contains(key)
                        ? executionTick_ + 1
                        : executionTick_;
        source->edits.insert(lineId, SourceEdit{effectiveTick});
    }
    editError_.clear();
    if (active_) {
        syncSourceBreakpoints(*source);
        if (running_ && commandInFlight_ && !handlingDebuggerOutput_ &&
            (currentCommand_.kind == CommandKind::Run ||
             currentCommand_.kind == CommandKind::Continue)) {
            editInterruptRequested_ = true;
            debugger_.write("\x03", 1);
        }
    }
    emit filesChanged();
    emit linesChanged();
    setStatus(source->edits.contains(lineId)
                      ? QStringLiteral("Native edit queued for tick %1.")
                                .arg(source->edits[lineId].effectiveTick)
                      : QStringLiteral("Source line restored."));
    return true;
}

int SimulationDebuggerModel::insertLineAfter(int lineNumber) {
    SourceFile *const source = selectedSource();
    if (source == nullptr || lineNumber < 0 ||
        lineNumber >= source->currentLines.size()) {
        editError_ = QStringLiteral(
                "A line can only be inserted before another source boundary.");
        emit linesChanged();
        return -1;
    }
    const int insertIndex = lineNumber;
    int anchorLine = 0;
    for (int index = insertIndex; index < source->sourceLineNumbers.size();
         ++index) {
        if (source->sourceLineNumbers[index] > 0) {
            anchorLine = source->sourceLineNumbers[index];
            break;
        }
    }
    if (anchorLine <= 0) {
        editError_ = QStringLiteral(
                "The end of this file has no following executable boundary.");
        emit linesChanged();
        return -1;
    }
    const quint64 lineId = nextInsertedLineId_++;
    const qint64 effectiveTick =
            effectiveTickForBoundary(*source, anchorLine);
    source->currentLines.insert(insertIndex, QString());
    source->highlightedLight.insert(insertIndex, QString());
    source->highlightedDark.insert(insertIndex, QString());
    source->inlineValueLines.insert(insertIndex, QString());
    source->lineIds.insert(insertIndex, lineId);
    source->sourceLineNumbers.insert(insertIndex, 0);
    source->anchorLineNumbers.insert(insertIndex, anchorLine);
    source->edits.insert(lineId, SourceEdit{effectiveTick});
    ++sourceRevision_;
    editError_.clear();
    if (active_) {
        syncSourceBreakpoints(*source);
    }
    if (activeFilePath_ == source->path) {
        activeLine_ =
                displayLineForSourceLine(*source, activeSourceLine_);
    }
    refreshDebugOutputLocations(source->path);
    emit filesChanged();
    emit linesChanged();
    emit executionChanged();
    setStatus(QStringLiteral("Inserted source line queued for tick %1.")
                      .arg(effectiveTick));
    return insertIndex + 1;
}

bool SimulationDebuggerModel::deleteLine(int lineNumber) {
    SourceFile *const source = selectedSource();
    if (source == nullptr || lineNumber <= 0 ||
        lineNumber > source->currentLines.size()) {
        return false;
    }
    const int index = lineNumber - 1;
    const quint64 lineId = source->lineIds[index];
    const int sourceLine = source->sourceLineNumbers[index];
    const int anchorLine = source->anchorLineNumbers[index];
    const qint64 effectiveTick =
            effectiveTickForBoundary(*source, anchorLine);
    source->edits.remove(lineId);
    source->breakpoints.remove(lineId);
    if (sourceLine > 0) {
        source->deletedSourceLines.insert(
                sourceLine,
                SourceEdit{effectiveTick,
                           IsEditableSourceLine(
                                   source->originalLines[sourceLine - 1])});
    }
    source->currentLines.removeAt(index);
    source->highlightedLight.removeAt(index);
    source->highlightedDark.removeAt(index);
    source->inlineValueLines.removeAt(index);
    source->lineIds.removeAt(index);
    source->sourceLineNumbers.removeAt(index);
    source->anchorLineNumbers.removeAt(index);
    ++sourceRevision_;
    editError_.clear();
    saveBreakpoints();
    if (active_) {
        syncSourceBreakpoints(*source);
    }
    if (activeFilePath_ == source->path) {
        activeLine_ =
                displayLineForSourceLine(*source, activeSourceLine_);
    }
    refreshDebugOutputLocations(source->path);
    emit filesChanged();
    emit linesChanged();
    emit executionChanged();
    setStatus(sourceLine > 0
                      ? QStringLiteral("Deleted source line queued for tick %1.")
                                .arg(effectiveTick)
                      : QStringLiteral("Inserted source line removed."));
    return true;
}

bool SimulationDebuggerModel::toggleBreakpoint(const QString &path,
                                               int lineNumber) {
    const int index = sourceIndex(path);
    if (index < 0 || lineNumber <= 0 ||
        lineNumber >
                sources_[static_cast<std::size_t>(index)].currentLines.size()) {
        return false;
    }
    SourceFile &source = sources_[static_cast<std::size_t>(index)];
    const quint64 lineId = source.lineIds[lineNumber - 1];
    const bool removed = source.breakpoints.contains(lineId);
    if (removed) {
        source.breakpoints.remove(lineId);
    } else {
        source.breakpoints.insert(lineId);
        source.executableSourceLines.insert(
                source.anchorLineNumbers[lineNumber - 1]);
    }
    saveBreakpoints();
    if (active_) {
        syncSourceBreakpoints(source);
        if (running_ && commandInFlight_ && !handlingDebuggerOutput_ &&
            (currentCommand_.kind == CommandKind::Run ||
             currentCommand_.kind == CommandKind::Continue)) {
            editInterruptRequested_ = true;
            debugger_.write("\x03", 1);
        }
    }
    emit filesChanged();
    if (selectedFilePath_ == path) {
        emit linesChanged();
    }
    setStatus(removed ? QStringLiteral("Breakpoint removed from %1:%2.")
                                .arg(fileName(path))
                                .arg(lineNumber)
                      : QStringLiteral("Breakpoint requested at %1:%2.")
                                .arg(fileName(path))
                                .arg(lineNumber));
    return true;
}

void SimulationDebuggerModel::clearDebugOutput() {
    if (debugOutput_.isEmpty()) {
        return;
    }
    debugOutput_.clear();
    emit debugOutputChanged();
}

bool SimulationDebuggerModel::openDebugOutput(int index) {
    if (index < 0 || index >= debugOutput_.size()) {
        return false;
    }
    const QVariantMap entry = debugOutput_[index].toMap();
    const QString path = entry.value(QStringLiteral("path")).toString();
    const int line = entry.value(QStringLiteral("line")).toInt();
    if (line <= 0 || !selectFile(path)) {
        return false;
    }
    emit sourceLocationRequested(line);
    return true;
}

void SimulationDebuggerModel::resetEdits() {
    bool changed = false;
    for (SourceFile &source : sources_) {
        if (source.edits.isEmpty() && source.deletedSourceLines.isEmpty()) {
            continue;
        }
        QSet<int> breakpointSourceLines;
        for (const quint64 lineId : source.breakpoints) {
            const int displayLine = displayLineForId(source, lineId);
            if (displayLine > 0) {
                breakpointSourceLines.insert(
                        anchorLineForDisplayLine(source, displayLine));
            }
        }
        source.currentLines = source.originalLines;
        source.highlightedLight.clear();
        source.highlightedDark.clear();
        source.inlineValueLines.clear();
        source.lineIds.clear();
        source.sourceLineNumbers.clear();
        source.anchorLineNumbers.clear();
        source.highlightedLight.reserve(source.originalLines.size());
        source.highlightedDark.reserve(source.originalLines.size());
        source.inlineValueLines.resize(source.originalLines.size());
        source.lineIds.reserve(source.originalLines.size());
        source.sourceLineNumbers.reserve(source.originalLines.size());
        source.anchorLineNumbers.reserve(source.originalLines.size());
        for (int index = 0; index < source.originalLines.size(); ++index) {
            source.highlightedLight.push_back(
                    syntaxHighlighted(source.originalLines[index], false));
            source.highlightedDark.push_back(
                    syntaxHighlighted(source.originalLines[index], true));
            source.lineIds.push_back(static_cast<quint64>(index + 1));
            source.sourceLineNumbers.push_back(index + 1);
            source.anchorLineNumbers.push_back(index + 1);
        }
        source.edits.clear();
        source.deletedSourceLines.clear();
        source.breakpoints.clear();
        for (const int sourceLine : breakpointSourceLines) {
            if (sourceLine > 0 && sourceLine <= source.lineIds.size()) {
                source.breakpoints.insert(source.lineIds[sourceLine - 1]);
            }
        }
        refreshDebugOutputLocations(source.path);
        if (activeFilePath_ == source.path) {
            activeLine_ =
                    displayLineForSourceLine(source, activeSourceLine_);
        }
        if (active_) {
            syncSourceBreakpoints(source);
        }
        changed = true;
    }
    if (changed) {
        ++sourceRevision_;
    }
    editError_.clear();
    emit filesChanged();
    emit linesChanged();
    setStatus(QStringLiteral("All in-memory source edits were reset."));
}

bool SimulationDebuggerModel::stepSubstep() {
    return beginStep(StepMode::Substep);
}

bool SimulationDebuggerModel::stepSourceLine() {
    return beginStep(StepMode::SourceLine);
}

bool SimulationDebuggerModel::stepTick() {
    return beginStep(StepMode::Tick);
}

void SimulationDebuggerModel::configure(const QString &backendName) {
    backendName_ = backendName.trimmed().isEmpty() ? QStringLiteral("Reference")
                                                   : backendName;
    stopSession();
    emit stateChanged();
}

bool SimulationDebuggerModel::startSession(const QString &packsDirectory,
                                           const QString &replayPath,
                                           qint64 simulationHorizonMs,
                                           const QString &inputScript) {
    if (packsDirectory.trimmed().isEmpty() || replayPath.trimmed().isEmpty()) {
        setStatus(QStringLiteral("Load a map and Packs directory before "
                                 "starting native source debugging."));
        return false;
    }
    pendingPacksDirectory_ = QDir::toNativeSeparators(packsDirectory);
    pendingReplayPath_ = QDir::toNativeSeparators(replayPath);
    pendingInputScript_ = inputScript;
    pendingSimulationHorizonMs_ = simulationHorizonMs;
    pendingStart_ = true;
    if (preparing_) {
        setStatus(
                QStringLiteral("Waiting for reference source preparation..."));
        return true;
    }
    if (!available_) {
        pendingStart_ = false;
        setStatus(QStringLiteral("Native source debugging is not available."));
        return false;
    }
    if (active_ || debugger_.state() != QProcess::NotRunning) {
        stopSessionProcess(true);
    } else {
        beginPendingSession();
    }
    return true;
}

void SimulationDebuggerModel::beginPendingSession() {
    if (!pendingStart_ || preparing_ || !available_) {
        return;
    }
    if (debugger_.state() != QProcess::NotRunning) {
        stopSessionProcess(true);
        return;
    }
    packsDirectory_ = pendingPacksDirectory_;
    replayPath_ = pendingReplayPath_;
    simulationHorizonMs_ = pendingSimulationHorizonMs_;
    QFile::remove(inputScriptPath_);
    QTemporaryFile inputFile(
            QDir::tempPath() +
            QStringLiteral("/forevertas-debug-input-XXXXXX.txt"));
    inputFile.setAutoRemove(false);
    const QByteArray encodedInputScript = pendingInputScript_.toUtf8();
    if (!inputFile.open() ||
        inputFile.write(encodedInputScript) != encodedInputScript.size() ||
        !inputFile.flush()) {
        pendingStart_ = false;
        setStatus(QStringLiteral("Could not prepare the debugger input script."));
        emit stateChanged();
        return;
    }
    inputScriptPath_ = QDir::toNativeSeparators(inputFile.fileName());
    inputFile.close();
    pendingStart_ = false;
    stopping_ = false;
    const quint64 generation = ++sessionGeneration_;
    commandQueue_.clear();
    debuggerBuffer_.clear();
    editError_.clear();
    clearDebugOutput();
    debugOutputSequence_ = 0;
    executedLinesThisTick_.clear();
    currentLineKey_.clear();
    activeLine_ = -1;
    activeSourceLine_ = -1;
    activeFilePath_.clear();
    pendingBoundaryEditIds_.clear();
    pendingBoundaryEditIndex_ = 0;
    boundaryEditInProgress_ = false;
    pendingBoundarySkipsOriginal_ = false;
    lastBreakpointKey_.clear();
    executionTick_ = 0;
    installedBreakpointKeys_.clear();
    installedBreakpointIds_.clear();
    setupQueued_ = false;
    startupPromptsRemaining_ = 1;
    hasCurrentCommand_ = false;
    commandInFlight_ = false;
    advanceScheduled_ = false;
    cancelStep();
    pauseRequested_ = false;
    editInterruptRequested_ = false;
    handlingDebuggerOutput_ = false;
    outputProcessing_ = false;
    atTickBoundary_ = false;
    workerReady_ = false;
    active_ = true;
    setRunning(false);
    setStatus(QStringLiteral("Starting the real reference physics engine..."));
    emit stateChanged();
    debugger_.setWorkingDirectory(QCoreApplication::applicationDirPath());
    debugger_.start(runtimeTerminalPath_, DebuggerTerminalArguments(
            NativeDebuggerTerminalPlatform(), runtimeLldbPath_,
            {QStringLiteral("--no-lldbinit"), QStringLiteral("--no-use-colors")}));
    QTimer::singleShot(5000, this, [this, generation]() {
        if (generation == sessionGeneration_ && active_ &&
            (debugger_.state() != QProcess::Running || startupPromptsRemaining_ > 0)) {
            failSession(QStringLiteral("LLDB did not produce an interactive prompt. Check Debugger tools and terminal runtime dependencies.\n%1")
                                .arg(debuggerBuffer_.right(2000)));
        }
    });
}

void SimulationDebuggerModel::stopSession() {
    stopSessionProcess(false);
}

void SimulationDebuggerModel::stopSessionProcess(bool keepPendingStart) {
    const bool wasActive = active_;
    if (!keepPendingStart) {
        pendingStart_ = false;
        pendingPacksDirectory_.clear();
        pendingReplayPath_.clear();
        pendingInputScript_.clear();
    }
    ++sessionGeneration_;
    setRunning(false);
    active_ = false;
    compiling_ = false;
    commandInFlight_ = false;
    advanceScheduled_ = false;
    outputProcessing_ = false;
    handlingDebuggerOutput_ = false;
    cancelStep();
    atTickBoundary_ = false;
    commandQueue_.clear();
    hasCurrentCommand_ = false;
    installedBreakpointKeys_.clear();
    installedBreakpointIds_.clear();
    clearExecutionLocation();
    if (debugger_.state() != QProcess::NotRunning) {
        stopping_ = true;
        const quint64 shutdown = ++shutdownGeneration_;
        debugger_.write("quit\n");
        debugger_.closeWriteChannel();
        QTimer::singleShot(250, this, [this, shutdown]() {
            if (shutdown == shutdownGeneration_ &&
                debugger_.state() != QProcess::NotRunning) {
                debugger_.terminate();
            }
        });
        QTimer::singleShot(750, this, [this, shutdown]() {
            if (shutdown == shutdownGeneration_ &&
                debugger_.state() != QProcess::NotRunning) {
                debugger_.kill();
            }
        });
    } else {
        stopping_ = false;
        QFile::remove(inputScriptPath_);
        inputScriptPath_.clear();
    }
    if (wasActive) {
        setStatus(QStringLiteral("Native source debugging stopped."));
        emit stateChanged();
        emit sessionFinished();
    }
    if (!stopping_ && keepPendingStart && pendingStart_) {
        QTimer::singleShot(0, this,
                           &SimulationDebuggerModel::beginPendingSession);
    }
}

void SimulationDebuggerModel::play() {
    if (!active_ || compiling_ || stepping_) {
        return;
    }
    lastBreakpointKey_.clear();
    pauseRequested_ = false;
    editError_.clear();
    setRunning(true);
    setStatus(QStringLiteral("Stepping the real reference physics engine."));
    scheduleAdvance();
}

void SimulationDebuggerModel::pause() {
    if (!active_ || stepping_ || pauseRequested_ || !running_) {
        return;
    }
    setRunning(false);
    pauseRequested_ = true;
    setStatus(QStringLiteral(
            "Pausing at the next native source-line boundary..."));
    if (!commandInFlight_ && activeLine_ > 0) {
        pauseRequested_ = false;
        clearVariables();
        queueCommand(CommandKind::Variables,
                     QStringLiteral("frame variable --show-types"));
        setStatus(QStringLiteral("Paused at a native source-line boundary."));
    } else if (!commandInFlight_ && atTickBoundary_) {
        queueCommand(CommandKind::Continue, QStringLiteral("continue"));
    }
}

QString SimulationDebuggerModel::syntaxHighlighted(const QString &text,
                                                   bool darkMode) {
    QString escaped = HtmlEscape(text);
    static const QRegularExpression stringPattern(
            QStringLiteral("(\".*?\"|'.*?')"));
    static const QRegularExpression numberPattern(
            QStringLiteral("\\b([0-9]+(?:\\.[0-9]+)?[fFuUlL]*)\\b"));
    static const QRegularExpression keywordPattern(QStringLiteral(
            "\\b(auto|bool|break|case|catch|class|const|constexpr|"
            "continue|default|do|double|else|enum|explicit|false|"
            "float|for|if|inline|int|namespace|noexcept|nullptr|"
            "private|protected|public|return|sizeof|static|struct|"
            "switch|template|this|throw|true|try|typename|using|"
            "void|while)\\b"));
    const QString stringColor =
            darkMode ? QStringLiteral("#e9b86c") : QStringLiteral("#8a4f27");
    const QString numberColor =
            darkMode ? QStringLiteral("#d9a0e8") : QStringLiteral("#8b3fa3");
    const QString keywordColor =
            darkMode ? QStringLiteral("#80b9ef") : QStringLiteral("#235f9e");
    const QString commentColor =
            darkMode ? QStringLiteral("#91a092") : QStringLiteral("#6b786b");
    escaped.replace(stringPattern,
                    QStringLiteral("<span style=\"color:%1\">\\1</span>")
                            .arg(stringColor));
    escaped.replace(numberPattern,
                    QStringLiteral("<span style=\"color:%1\">\\1</span>")
                            .arg(numberColor));
    escaped.replace(keywordPattern,
                    QStringLiteral("<span style=\"color:%1;font-weight:600\">"
                                   "\\1</span>")
                            .arg(keywordColor));
    const qsizetype comment = escaped.indexOf(QStringLiteral("//"));
    if (comment >= 0) {
        escaped =
                escaped.left(comment) +
                QStringLiteral("<span style=\"color:%1\">").arg(commentColor) +
                escaped.mid(comment) + QStringLiteral("</span>");
    }
    return QStringLiteral(
                   "<pre style=\"margin:0; white-space:pre\">%1</pre>")
            .arg(escaped);
}

SimulationDebuggerModel::ProcessedDebuggerOutput
SimulationDebuggerModel::processDebuggerOutput(const QString &output,
                                               bool parseVariables,
                                               bool parseStopLocation,
                                               const QString &printToken) {
    ProcessedDebuggerOutput result;
    result.rawOutput = output;
    result.diagnostics = TrimDebuggerNoise(output);
    static const QRegularExpression errorPattern(
            QStringLiteral("(error:|Errors occurred while evaluating|"
                           "<user expression [^>]*>:\\d+:\\d+: error:)"),
            QRegularExpression::CaseInsensitiveOption);
    result.commandFailed = errorPattern.match(result.diagnostics).hasMatch();

    const QString marker = QStringLiteral("@FOREVERTAS_STATE ");
    const QString workerErrorMarker = QStringLiteral("@FOREVERTAS_ERROR ");
    static const QRegularExpression variablePattern(
            QStringLiteral("^\\s*\\(([^)]*)\\)\\s+([^\\s=]+)\\s+=\\s+(.*)$"));
    for (const QString &rawLine : output.split(QLatin1Char('\n'))) {
        QString line = rawLine;
        line.remove(QLatin1Char('\r'));
        if (line.startsWith(marker)) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(
                    line.mid(marker.size()).toUtf8(), &error);
            if (error.error != QJsonParseError::NoError ||
                !document.isObject()) {
                result.workerErrors.push_back(QStringLiteral(
                        "Reference worker emitted an invalid state."));
                continue;
            }
            const QJsonObject state = document.object();
            QVariantMap frame;
            frame.insert(
                    QStringLiteral("tick"),
                    static_cast<qint64>(
                            state.value(QStringLiteral("tick")).toDouble()));
            frame.insert(
                    QStringLiteral("timeMs"),
                    static_cast<qint64>(
                            state.value(QStringLiteral("timeMs")).toDouble()));
            frame.insert(QStringLiteral("horizonMs"),
                         static_cast<qint64>(
                                 state.value(QStringLiteral("horizonMs"))
                                         .toDouble()));
            frame.insert(QStringLiteral("position"),
                         JsonVector(state.value(QStringLiteral("position"))));
            frame.insert(QStringLiteral("rotation"),
                         JsonVector(state.value(QStringLiteral("rotation"))));
            frame.insert(
                    QStringLiteral("linearSpeed"),
                    JsonVector(state.value(QStringLiteral("linearSpeed"))));
            frame.insert(
                    QStringLiteral("angularSpeed"),
                    JsonVector(state.value(QStringLiteral("angularSpeed"))));
            frame.insert(QStringLiteral("force"),
                         JsonVector(state.value(QStringLiteral("force"))));
            frame.insert(QStringLiteral("torque"),
                         JsonVector(state.value(QStringLiteral("torque"))));
            frame.insert(QStringLiteral("accelerate"),
                         state.value(QStringLiteral("accelerate")).toDouble());
            frame.insert(QStringLiteral("brake"),
                         state.value(QStringLiteral("brake")).toDouble());
            frame.insert(QStringLiteral("steering"),
                         state.value(QStringLiteral("steering")).toDouble());
            frame.insert(QStringLiteral("checkpointsCollected"),
                         state.value(QStringLiteral("checkpointsCollected"))
                                 .toInt());
            frame.insert(
                    QStringLiteral("checkpointsTotal"),
                    state.value(QStringLiteral("checkpointsTotal")).toInt());
            frame.insert(QStringLiteral("completedLaps"),
                         state.value(QStringLiteral("completedLaps")).toInt());
            frame.insert(QStringLiteral("totalLaps"),
                         state.value(QStringLiteral("totalLaps")).toInt());
            frame.insert(QStringLiteral("raceCompleted"),
                         state.value(QStringLiteral("raceCompleted")).toBool());
            if (state.value(QStringLiteral("finishTimeMs")).isDouble()) {
                frame.insert(QStringLiteral("finishTimeMs"),
                             static_cast<qint64>(
                                     state.value(QStringLiteral("finishTimeMs"))
                                             .toDouble()));
            }
            result.frames.push_back(frame);
            continue;
        }
        if (line.startsWith(workerErrorMarker)) {
            const QJsonDocument error = QJsonDocument::fromJson(
                    line.mid(workerErrorMarker.size()).toUtf8());
            result.workerErrors.push_back(
                    error.object()
                            .value(QStringLiteral("message"))
                            .toString(QStringLiteral(
                                    "Reference worker failed.")));
            continue;
        }
        if (parseVariables && result.parsedVariables.size() < 512) {
            const QRegularExpressionMatch match = variablePattern.match(line);
            if (match.hasMatch()) {
                result.parsedVariables.push_back(QVariantMap{
                        {QStringLiteral("name"), match.captured(2)},
                        {QStringLiteral("value"), match.captured(3).trimmed()},
                        {QStringLiteral("type"), match.captured(1).trimmed()}});
            }
        }
    }
    if (!printToken.isEmpty()) {
        const QString beginMarker =
                QStringLiteral("@FOREVERTAS_DEBUG_PRINT_BEGIN_%1@")
                        .arg(printToken);
        const QString endMarker =
                QStringLiteral("@FOREVERTAS_DEBUG_PRINT_END_%1@")
                        .arg(printToken);
        QString normalized = output;
        normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        normalized.remove(QLatin1Char('\r'));
        const qsizetype begin = normalized.lastIndexOf(beginMarker);
        const qsizetype end =
                begin < 0
                        ? -1
                        : normalized.indexOf(endMarker,
                                             begin + beginMarker.size());
        if (begin >= 0 && end >= 0) {
            QString printed =
                    normalized.mid(begin + beginMarker.size(),
                                   end - begin - beginMarker.size());
            if (printed.startsWith(QLatin1Char('\n'))) {
                printed.remove(0, 1);
            }
            QStringList lines =
                    printed.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
            if (!lines.isEmpty() && lines.back().isEmpty()) {
                lines.removeLast();
            }
            result.printedLines = std::move(lines);
        }
    }
    if (parseVariables) {
        std::sort(result.parsedVariables.begin(), result.parsedVariables.end(),
                  [](const QVariant &left, const QVariant &right) {
                      return left.toMap()
                                     .value(QStringLiteral("name"))
                                     .toString() <
                             right.toMap()
                                     .value(QStringLiteral("name"))
                                     .toString();
                  });
    }

    if (parseStopLocation) {
        result.tickBoundary = output.contains(
                QStringLiteral("forevertas_debugger_tick_boundary"));
        static const QRegularExpression locationPattern(
                QStringLiteral("\\bat (.+?):(\\d+)(?::\\d+)?\\s*$"),
                QRegularExpression::MultilineOption);
        QRegularExpressionMatchIterator matches =
                locationPattern.globalMatch(output);
        QRegularExpressionMatch last;
        while (matches.hasNext()) {
            last = matches.next();
        }
        if (last.hasMatch()) {
            result.stopPath = QDir::cleanPath(last.captured(1).trimmed());
            result.stopLine = last.captured(2).toInt();
        }
    }
    return result;
}

QHash<QString, QStringList> SimulationDebuggerModel::buildInlineValueCache(
        const QVariantList &variables,
        const QHash<QString, QStringList> &sourceLines) {
    struct InlineVariable {
        QString display;
        QRegularExpression pattern;
    };
    std::vector<InlineVariable> patterns;
    patterns.reserve(static_cast<std::size_t>(variables.size()));
    for (const QVariant &value : variables) {
        const QVariantMap variable = value.toMap();
        const QString name = variable.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || name == QStringLiteral("this")) {
            continue;
        }
        patterns.push_back(InlineVariable{
                name + QStringLiteral(" = ") +
                        variable.value(QStringLiteral("value")).toString(),
                QRegularExpression(
                        QStringLiteral("(^|[^A-Za-z0-9_])%1"
                                       "([^A-Za-z0-9_]|$)")
                                .arg(QRegularExpression::escape(name)))});
    }

    QHash<QString, QStringList> result;
    result.reserve(sourceLines.size());
    for (auto source = sourceLines.cbegin(); source != sourceLines.cend();
         ++source) {
        QStringList valuesByLine;
        valuesByLine.reserve(source.value().size());
        for (const QString &line : source.value()) {
            QStringList values;
            for (const InlineVariable &variable : patterns) {
                if (!variable.pattern.match(line).hasMatch()) {
                    continue;
                }
                values.push_back(variable.display);
                if (values.size() == 3) {
                    break;
                }
            }
            valuesByLine.push_back(values.join(QStringLiteral("   ")));
        }
        result.insert(source.key(), std::move(valuesByLine));
    }
    return result;
}

QString SimulationDebuggerModel::fileName(const QString &path) {
    return path.section(QLatin1Char('/'), -1);
}

int SimulationDebuggerModel::depth(const QString &path) {
    return path.count(QLatin1Char('/'));
}

QString SimulationDebuggerModel::configuredLldbPath() const {
    return QSettings().value(QStringLiteral("simulationDebugger/lldbPath")).toString();
}

QString SimulationDebuggerModel::configuredTerminalPath() const {
    return QSettings().value(QStringLiteral("simulationDebugger/terminalPath")).toString();
}

void SimulationDebuggerModel::configureTools(const QString &lldb, const QString &terminal) {
    if (active_ || preparing_) return;
    QSettings().setValue(QStringLiteral("simulationDebugger/lldbPath"), lldb.trimmed());
    QSettings().setValue(QStringLiteral("simulationDebugger/terminalPath"), terminal.trimmed());
    refreshDebuggerTools();
}

void SimulationDebuggerModel::refreshDebuggerTools() {
    if (active_ || preparing_) return;
    const auto tools = DiscoverDebuggerTools();
    runtimeLldbPath_ = tools.lldb;
    runtimeTerminalPath_ = tools.terminal;
    preparing_ = true;
    available_ = false;
    setStatus(QStringLiteral("Checking debugger runtime..."));
    emit stateChanged();
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        watcher->deleteLater();
        preparing_ = false;
        available_ = error.isEmpty() && !sources_.empty();
        setStatus(available_ ? QStringLiteral("Real reference engine source is ready.")
                            : error.isEmpty() ? QStringLiteral("Reference source resources are missing; reinstall the application.") : error);
        emit stateChanged();
        if (pendingStart_ && available_) beginPendingSession();
        else if (!available_) pendingStart_ = false;
    });
    const auto worker = workerExecutablePath();
    watcher->setFuture(QtConcurrent::run([tools, worker] { return ProbeDebuggerTools(tools, worker); }));
}

QString SimulationDebuggerModel::workerExecutablePath() {
#ifdef Q_OS_WIN
    const QString name =
            QStringLiteral("forevertas-simulation-debug-worker.exe");
#else
    const QString name = QStringLiteral("forevertas-simulation-debug-worker");
#endif
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const QString adjacent = applicationDirectory.filePath(name);
    if (QFileInfo::exists(adjacent)) {
        return adjacent;
    }
    return applicationDirectory.filePath(QStringLiteral("bin/") + name);
}

QString SimulationDebuggerModel::sourceRootPath() {
#ifdef FOREVERTAS_SIMULATION_DEBUG_SOURCE_ROOT
    return QDir::cleanPath(
            QString::fromUtf8(FOREVERTAS_SIMULATION_DEBUG_SOURCE_ROOT));
#else
    return {};
#endif
}

QString SimulationDebuggerModel::quoteDebuggerArgument(const QString &value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

int SimulationDebuggerModel::sourceIndex(const QString &path) const {
    const auto found = std::find_if(
            sources_.begin(), sources_.end(),
            [&path](const SourceFile &source) { return source.path == path; });
    return found == sources_.end()
                   ? -1
                   : static_cast<int>(std::distance(sources_.begin(), found));
}

SimulationDebuggerModel::SourceFile *SimulationDebuggerModel::selectedSource() {
    const int index = sourceIndex(selectedFilePath_);
    return index < 0 ? nullptr : &sources_[static_cast<std::size_t>(index)];
}

const SimulationDebuggerModel::SourceFile *
SimulationDebuggerModel::selectedSource() const {
    const int index = sourceIndex(selectedFilePath_);
    return index < 0 ? nullptr : &sources_[static_cast<std::size_t>(index)];
}

int SimulationDebuggerModel::displayLineForSourceLine(
        const SourceFile &source, int sourceLine) const {
    if (sourceLine <= 0) {
        return -1;
    }
    for (int index = 0; index < source.sourceLineNumbers.size(); ++index) {
        if (source.sourceLineNumbers[index] == sourceLine) {
            return index + 1;
        }
    }
    for (int index = 0; index < source.anchorLineNumbers.size(); ++index) {
        if (source.anchorLineNumbers[index] == sourceLine) {
            return index + 1;
        }
    }
    for (int index = 0; index < source.sourceLineNumbers.size(); ++index) {
        if (source.sourceLineNumbers[index] > sourceLine) {
            return index + 1;
        }
    }
    return source.currentLines.isEmpty() ? -1 : source.currentLines.size();
}

int SimulationDebuggerModel::displayLineForId(const SourceFile &source,
                                              quint64 lineId) const {
    const auto found = std::find(source.lineIds.cbegin(),
                                 source.lineIds.cend(), lineId);
    return found == source.lineIds.cend()
                   ? -1
                   : static_cast<int>(
                             std::distance(source.lineIds.cbegin(), found)) +
                             1;
}

int SimulationDebuggerModel::sourceLineForDisplayLine(
        const SourceFile &source, int displayLine) const {
    return displayLine > 0 &&
                           displayLine <= source.sourceLineNumbers.size()
                   ? source.sourceLineNumbers[displayLine - 1]
                   : -1;
}

int SimulationDebuggerModel::anchorLineForDisplayLine(
        const SourceFile &source, int displayLine) const {
    return displayLine > 0 &&
                           displayLine <= source.anchorLineNumbers.size()
                   ? source.anchorLineNumbers[displayLine - 1]
                   : -1;
}

bool SimulationDebuggerModel::sourceWantsBreakpoint(
        const SourceFile &source, int sourceLine) const {
    for (const quint64 lineId : source.breakpoints) {
        const int displayLine = displayLineForId(source, lineId);
        if (anchorLineForDisplayLine(source, displayLine) == sourceLine) {
            return true;
        }
    }
    for (auto edit = source.edits.cbegin(); edit != source.edits.cend();
         ++edit) {
        const int displayLine = displayLineForId(source, edit.key());
        if (anchorLineForDisplayLine(source, displayLine) == sourceLine) {
            return true;
        }
    }
    const auto deleted = source.deletedSourceLines.constFind(sourceLine);
    return deleted != source.deletedSourceLines.cend() &&
           deleted->affectsExecution;
}

bool SimulationDebuggerModel::hasApplicableEdits(
        const SourceFile &source, int sourceLine) const {
    const auto deleted = source.deletedSourceLines.constFind(sourceLine);
    if (deleted != source.deletedSourceLines.cend() &&
        deleted->affectsExecution &&
        deleted->effectiveTick <= executionTick_) {
        return true;
    }
    for (auto edit = source.edits.cbegin(); edit != source.edits.cend();
         ++edit) {
        const int displayLine = displayLineForId(source, edit.key());
        if (anchorLineForDisplayLine(source, displayLine) == sourceLine &&
            edit->effectiveTick <= executionTick_) {
            return true;
        }
    }
    return false;
}

qint64 SimulationDebuggerModel::effectiveTickForBoundary(
        const SourceFile &source, int sourceLine) const {
    const QString key = lineKey(source.path, sourceLine);
    return active_ && executedLinesThisTick_.contains(key)
                   ? executionTick_ + 1
                   : executionTick_;
}

QVariantList SimulationDebuggerModel::visibleFileEntries() const {
    QSet<QString> directories;
    for (const SourceFile &source : sources_) {
        QString folder = source.path.section(QLatin1Char('/'), 0, -2);
        while (!folder.isEmpty()) {
            directories.insert(folder);
            folder = folder.section(QLatin1Char('/'), 0, -2);
        }
    }
    QStringList sortedDirectories(directories.begin(), directories.end());
    std::sort(sortedDirectories.begin(), sortedDirectories.end());

    QVariantList result;
    const auto visible = [this](const QString &path) {
        QString parent = path.section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            if (!expandedFolders_.contains(parent)) {
                return false;
            }
            parent = parent.section(QLatin1Char('/'), 0, -2);
        }
        return true;
    };
    for (const QString &directory : sortedDirectories) {
        if (!visible(directory)) {
            continue;
        }
        const QString prefix = directory + QLatin1Char('/');
        const bool modified =
                std::any_of(sources_.begin(), sources_.end(),
                            [&prefix](const SourceFile &source) {
                                return source.path.startsWith(prefix) &&
                                       (!source.edits.isEmpty() ||
                                        !source.deletedSourceLines.isEmpty());
                            });
        const bool breakpoint =
                std::any_of(sources_.begin(), sources_.end(),
                            [&prefix](const SourceFile &source) {
                                return source.path.startsWith(prefix) &&
                                       !source.breakpoints.isEmpty();
                            });
        result.push_back(
                QVariantMap{{QStringLiteral("path"), directory},
                            {QStringLiteral("name"), fileName(directory)},
                            {QStringLiteral("depth"), depth(directory)},
                            {QStringLiteral("directory"), true},
                            {QStringLiteral("expanded"),
                             expandedFolders_.contains(directory)},
                            {QStringLiteral("modified"), modified},
                            {QStringLiteral("breakpoint"), breakpoint},
                            {QStringLiteral("selected"), false}});
    }
    for (const SourceFile &source : sources_) {
        if (!visible(source.path)) {
            continue;
        }
        result.push_back(QVariantMap{
                {QStringLiteral("path"), source.path},
                {QStringLiteral("name"), fileName(source.path)},
                {QStringLiteral("depth"), depth(source.path)},
                {QStringLiteral("directory"), false},
                {QStringLiteral("expanded"), false},
                {QStringLiteral("modified"),
                 !source.edits.isEmpty() ||
                         !source.deletedSourceLines.isEmpty()},
                {QStringLiteral("breakpoint"), !source.breakpoints.isEmpty()},
                {QStringLiteral("selected"), source.path == selectedFilePath_},
                {QStringLiteral("active"), source.path == activeFilePath_}});
    }
    std::sort(
            result.begin(), result.end(),
            [](const QVariant &left, const QVariant &right) {
                const QVariantMap leftMap = left.toMap();
                const QVariantMap rightMap = right.toMap();
                const QString leftPath =
                        leftMap.value(QStringLiteral("path")).toString();
                const QString rightPath =
                        rightMap.value(QStringLiteral("path")).toString();
                if (leftPath == rightPath) {
                    return leftMap.value(QStringLiteral("directory")).toBool();
                }
                const QString leftPrefix = leftPath + QLatin1Char('/');
                const QString rightPrefix = rightPath + QLatin1Char('/');
                if (rightPath.startsWith(leftPrefix)) {
                    return true;
                }
                if (leftPath.startsWith(rightPrefix)) {
                    return false;
                }
                return leftPath < rightPath;
            });
    return result;
}

QString SimulationDebuggerModel::inlineValues(const QString &line) const {
    QStringList values;
    for (const Variable &variable : variables_) {
        if (variable.name.isEmpty() ||
            variable.name == QStringLiteral("this")) {
            continue;
        }
        const QRegularExpression pattern(
                QStringLiteral("(^|[^A-Za-z0-9_])%1([^A-Za-z0-9_]|$)")
                        .arg(QRegularExpression::escape(variable.name)));
        if (pattern.match(line).hasMatch()) {
            values.push_back(variable.name + QStringLiteral(" = ") +
                             variable.value);
            if (values.size() == 3) {
                break;
            }
        }
    }
    return values.join(QStringLiteral("   "));
}

QString
SimulationDebuggerModel::relativeSourcePath(const QString &absolutePath) const {
    const QString clean = QDir::cleanPath(absolutePath);
    const QString root = sourceRootPath();
    if (!root.isEmpty() && clean.startsWith(root + QLatin1Char('/'))) {
        return clean.mid(root.size() + 1);
    }
    const qsizetype sourceIndex = clean.lastIndexOf(QStringLiteral("/src/"));
    if (sourceIndex >= 0) {
        const QString candidate = clean.mid(sourceIndex + 1);
        if (this->sourceIndex(candidate) >= 0) {
            return candidate;
        }
    }
    const QString basename = QFileInfo(clean).fileName();
    QString uniqueMatch;
    for (const SourceFile &source : sources_) {
        if (fileName(source.path) != basename) {
            continue;
        }
        if (!uniqueMatch.isEmpty()) {
            return {};
        }
        uniqueMatch = source.path;
    }
    if (!uniqueMatch.isEmpty()) {
        return uniqueMatch;
    }
    return {};
}

QString SimulationDebuggerModel::lineKey(const QString &path, int line) const {
    return path + QLatin1Char(':') + QString::number(line);
}

QString SimulationDebuggerModel::executionContextLabel() const {
    switch (stepMode_) {
    case StepMode::Substep:
        return QStringLiteral("substep");
    case StepMode::SourceLine:
        return QStringLiteral("source-line step");
    case StepMode::Tick:
        return QStringLiteral("tick step");
    case StepMode::None:
        return running_ ? QStringLiteral("playback")
                        : QStringLiteral("paused execution");
    }
    return QStringLiteral("execution");
}

int SimulationDebuggerModel::statementEndLine(const SourceFile &source,
                                              int line) const {
    int parentheses = 0;
    int brackets = 0;
    bool sawDelimiter = false;
    for (int current = line; current <= source.originalLines.size();
         ++current) {
        const QString &text = source.originalLines[current - 1];
        bool string = false;
        QChar quote;
        for (int index = 0; index < text.size(); ++index) {
            const QChar character = text[index];
            if (string) {
                if (character == QLatin1Char('\\')) {
                    ++index;
                } else if (character == quote) {
                    string = false;
                }
                continue;
            }
            if (character == QLatin1Char('"') ||
                character == QLatin1Char('\'')) {
                string = true;
                quote = character;
            } else if (character == QLatin1Char('(')) {
                ++parentheses;
                sawDelimiter = true;
            } else if (character == QLatin1Char(')')) {
                --parentheses;
            } else if (character == QLatin1Char('[')) {
                ++brackets;
                sawDelimiter = true;
            } else if (character == QLatin1Char(']')) {
                --brackets;
            }
        }
        const QString trimmed = text.trimmed();
        if (parentheses <= 0 && brackets <= 0 &&
            (trimmed.endsWith(QLatin1Char(';')) ||
             trimmed.endsWith(QLatin1Char('{')) ||
             trimmed == QStringLiteral("}") ||
             (!sawDelimiter && current > line))) {
            return current;
        }
    }
    return line;
}

void SimulationDebuggerModel::loadSources() {
    const quint64 generation = ++sourceLoadGeneration_;
    preparing_ = true;
    available_ = false;
    statusText_ =
            QStringLiteral("Preparing the real reference engine source...");
    emit stateChanged();

    const QString root = sourceRootPath();
    const QString workerPath = workerExecutablePath();
    const auto tools = DiscoverDebuggerTools();
    runtimeLldbPath_ = tools.lldb;
    runtimeTerminalPath_ = tools.terminal;
    using SourcePreparation = std::pair<std::vector<SourceFile>, QString>;
    auto *const watcher = new QFutureWatcher<SourcePreparation>(this);
    connect(watcher, &QFutureWatcher<SourcePreparation>::finished, this,
            [this, watcher, generation]() {
                SourcePreparation prepared = watcher->result();
                watcher->deleteLater();
                if (generation != sourceLoadGeneration_) {
                    return;
                }
                sources_ = std::move(prepared.first);
                ++sourceRevision_;
                const QString preferred =
                        QStringLiteral("src/simulation/runtime/"
                                       "replay_simulation_runtime.cpp");
                selectedFilePath_ =
                        sourceIndex(preferred) >= 0
                                ? preferred
                                : (sources_.empty() ? QString()
                                                    : sources_.front().path);
                available_ = prepared.second.isEmpty() && !sources_.empty();
                preparing_ = false;
                restoreBreakpoints();
                statusText_ =
                        available_
                                ? QStringLiteral(
                                          "Real reference engine source is "
                                          "ready.")
                                : prepared.second.isEmpty()
                                    ? QStringLiteral("Reference source resources are missing; reinstall the application.")
                                    : prepared.second;
                emit selectionChanged();
                emit filesChanged();
                emit linesChanged();
                emit stateChanged();
                if (pendingStart_ && available_ &&
                    debugger_.state() == QProcess::NotRunning) {
                    beginPendingSession();
                } else if (pendingStart_ && !available_) {
                    pendingStart_ = false;
                }
            });
    watcher->setFuture(QtConcurrent::run([root, workerPath, tools]() {
        std::vector<SourceFile> sources;
        sources.reserve(kSimulationDebugSourcePaths.size());
        for (const std::string_view entry : kSimulationDebugSourcePaths) {
            const QString path = QString::fromUtf8(
                    entry.data(), static_cast<qsizetype>(entry.size()));
            QFile file(QStringLiteral(":/simulation-debug/") + path);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            QString content = QString::fromUtf8(file.readAll());
            content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            content.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            QStringList lines = content.split(QLatin1Char('\n'));
            if (!lines.isEmpty() && lines.back().isEmpty()) {
                lines.removeLast();
            }
            QStringList highlightedLight;
            QStringList highlightedDark;
            QStringList inlineValueLines;
            highlightedLight.reserve(lines.size());
            highlightedDark.reserve(lines.size());
            inlineValueLines.resize(lines.size());
            for (const QString &line : lines) {
                highlightedLight.push_back(syntaxHighlighted(line, false));
                highlightedDark.push_back(syntaxHighlighted(line, true));
            }
            SourceFile source;
            source.path = path;
            source.absolutePath = QDir(root).filePath(path);
            source.originalLines = lines;
            source.currentLines = lines;
            source.highlightedLight = highlightedLight;
            source.highlightedDark = highlightedDark;
            source.inlineValueLines = inlineValueLines;
            source.lineIds.reserve(lines.size());
            source.sourceLineNumbers.reserve(lines.size());
            source.anchorLineNumbers.reserve(lines.size());
            for (int line = 1; line <= lines.size(); ++line) {
                source.lineIds.push_back(static_cast<quint64>(line));
                source.sourceLineNumbers.push_back(line);
                source.anchorLineNumbers.push_back(line);
            }
            sources.push_back(std::move(source));
        }
        std::sort(sources.begin(), sources.end(),
                  [](const SourceFile &left, const SourceFile &right) {
                      return left.path < right.path;
                  });
        return SourcePreparation{std::move(sources), ProbeDebuggerTools(tools, workerPath)};
    }));
}

void SimulationDebuggerModel::restoreBreakpoints() {
    const QStringList persisted =
            QSettings()
                    .value(QStringLiteral("simulationDebugger/"
                                          "breakpointsV1"))
                    .toStringList();
    for (const QString &key : persisted) {
        const qsizetype separator = key.lastIndexOf(QLatin1Char(':'));
        bool lineValid = false;
        const int line = key.mid(separator + 1).toInt(&lineValid);
        const QString path = separator > 0 ? key.left(separator) : QString();
        const int index = sourceIndex(path);
        if (!lineValid || line <= 0 || index < 0) {
            continue;
        }
        SourceFile &source = sources_[static_cast<std::size_t>(index)];
        const int displayLine = displayLineForSourceLine(source, line);
        if (displayLine <= 0 ||
            sourceLineForDisplayLine(source, displayLine) != line) {
            continue;
        }
        source.breakpoints.insert(source.lineIds[displayLine - 1]);
        source.executableSourceLines.insert(line);
    }
}

void SimulationDebuggerModel::saveBreakpoints() const {
    QStringList persisted;
    for (const SourceFile &source : sources_) {
        for (const quint64 lineId : source.breakpoints) {
            const int displayLine = displayLineForId(source, lineId);
            const int sourceLine =
                    anchorLineForDisplayLine(source, displayLine);
            if (sourceLine > 0) {
                persisted.push_back(lineKey(source.path, sourceLine));
            }
        }
    }
    std::sort(persisted.begin(), persisted.end());
    QSettings().setValue(QStringLiteral("simulationDebugger/breakpointsV1"),
                         persisted);
}

void SimulationDebuggerModel::syncSourceBreakpoints(SourceFile &source) {
    QSet<int> lines;
    for (const quint64 lineId : source.breakpoints) {
        const int displayLine = displayLineForId(source, lineId);
        const int anchorLine =
                anchorLineForDisplayLine(source, displayLine);
        if (anchorLine > 0) {
            lines.insert(anchorLine);
        }
    }
    for (auto edit = source.edits.cbegin(); edit != source.edits.cend();
         ++edit) {
        const int displayLine = displayLineForId(source, edit.key());
        const int anchorLine =
                anchorLineForDisplayLine(source, displayLine);
        if (anchorLine > 0) {
            lines.insert(anchorLine);
        }
    }
    for (auto deleted = source.deletedSourceLines.cbegin();
         deleted != source.deletedSourceLines.cend(); ++deleted) {
        if (deleted->affectsExecution) {
            lines.insert(deleted.key());
        }
    }
    const QString keyPrefix = source.path + QLatin1Char(':');
    const QList<QString> installed = installedBreakpointKeys_.values();
    for (const QString &key : installed) {
        if (!key.startsWith(keyPrefix)) {
            continue;
        }
        bool lineValid = false;
        const int installedLine = key.mid(keyPrefix.size()).toInt(&lineValid);
        if (!lineValid || lines.contains(installedLine)) {
            continue;
        }
        installedBreakpointKeys_.remove(key);
        const int breakpointId = installedBreakpointIds_.take(key);
        if (breakpointId > 0) {
            queueCommand(
                    CommandKind::SourceBreakpointRemove,
                    QStringLiteral("breakpoint delete %1").arg(breakpointId),
                    key, breakpointId);
        }
    }
    QList<int> sorted(lines.begin(), lines.end());
    std::sort(sorted.begin(), sorted.end());
    for (const int line : sorted) {
        installSourceBreakpoint(source, line);
    }
}

void SimulationDebuggerModel::installSourceBreakpoint(SourceFile &source,
                                                      int line) {
    const QString key = lineKey(source.path, line);
    if (!active_ || installedBreakpointKeys_.contains(key)) {
        return;
    }
    installedBreakpointKeys_.insert(key);
    source.executableSourceLines.insert(line);
    queueCommand(CommandKind::SourceBreakpoint,
                 QStringLiteral("breakpoint set --file %1 --line %2")
                         .arg(quoteDebuggerArgument(source.absolutePath))
                         .arg(line),
                 source.path, line);
}

void SimulationDebuggerModel::refreshDebugOutputLocations(
        const QString &path) {
    const int index = sourceIndex(path);
    if (index < 0) {
        return;
    }
    const SourceFile &source = sources_[static_cast<std::size_t>(index)];
    bool changed = false;
    for (QVariant &value : debugOutput_) {
        QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("path")).toString() != path) {
            continue;
        }
        const quint64 lineId =
                entry.value(QStringLiteral("lineId")).toULongLong();
        int displayLine = displayLineForId(source, lineId);
        if (displayLine <= 0) {
            displayLine = displayLineForSourceLine(
                    source,
                    entry.value(QStringLiteral("sourceLine")).toInt());
        }
        if (displayLine <= 0 ||
            entry.value(QStringLiteral("line")).toInt() == displayLine) {
            continue;
        }
        entry.insert(QStringLiteral("line"), displayLine);
        entry.insert(QStringLiteral("location"),
                     fileName(path) + QLatin1Char(':') +
                             QString::number(displayLine));
        value = entry;
        changed = true;
    }
    if (changed) {
        emit debugOutputChanged();
    }
}

void SimulationDebuggerModel::queueCommand(CommandKind kind,
                                           const QString &text,
                                           const QString &sourcePath,
                                           int line,
                                           const QString &printToken,
                                           quint64 lineId,
                                           int sourceLine) {
    if (!active_ || debugger_.state() == QProcess::NotRunning) {
        return;
    }
    commandQueue_.enqueue(DebuggerCommand{
            kind, text, sourcePath, printToken, lineId, line, sourceLine, {}, 0});
    sendNextCommand();
}

void SimulationDebuggerModel::sendNextCommand() {
    if (!active_ || hasCurrentCommand_ || outputProcessing_ ||
        commandQueue_.isEmpty() || debugger_.state() != QProcess::Running) {
        return;
    }
    currentCommand_ = commandQueue_.dequeue();
    hasCurrentCommand_ = true;
    commandInFlight_ = true;
    debugger_.write(currentCommand_.text.toUtf8());
    debugger_.write("\n");
}

void SimulationDebuggerModel::readDebuggerOutput() {
    const QByteArray chunk = debugger_.readAllStandardOutput();
    debuggerBuffer_ += QString::fromLocal8Bit(chunk);
    consumeDebuggerPrompts();
}

void SimulationDebuggerModel::consumeDebuggerPrompts() {
    if (outputProcessing_) {
        return;
    }
    static const QString prompt = QStringLiteral("(lldb) ");
    qsizetype promptPosition = debuggerBuffer_.indexOf(prompt);
    while (promptPosition >= 0) {
        handlingDebuggerOutput_ = true;
        const QString output = debuggerBuffer_.left(promptPosition);
        debuggerBuffer_.remove(0, promptPosition + prompt.size());
        if (startupPromptsRemaining_ > 0) {
            --startupPromptsRemaining_;
            if (startupPromptsRemaining_ == 0 && !setupQueued_) {
                setupQueued_ = true;
                queueCommand(CommandKind::Setting,
                             QStringLiteral("target create %1")
                                     .arg(quoteDebuggerArgument(
                                             workerExecutablePath())));
                queueCommand(
                        CommandKind::Setting,
                        QStringLiteral(
                                "settings set -- target.run-args %1 %2 %3 %4")
                                .arg(quoteDebuggerArgument(packsDirectory_))
                                .arg(quoteDebuggerArgument(replayPath_))
                                .arg(simulationHorizonMs_)
                                .arg(quoteDebuggerArgument(inputScriptPath_)));
                queueCommand(
                        CommandKind::Setting,
                        QStringLiteral(
                                "settings set symbols.enable-external-lookup "
                                "false"));
                queueCommand(CommandKind::Setting,
                             QStringLiteral("settings set "
                                            "target.inline-breakpoint-strategy "
                                            "always"));
                queueCommand(
                        CommandKind::FunctionBreakpoint,
                        QStringLiteral("breakpoint set --name "
                                       "forevertas_debugger_tick_boundary"));
                queueCommand(
                        CommandKind::FunctionBreakpoint,
                        QStringLiteral("breakpoint set --method AdvanceTicks"));
                queueCommand(CommandKind::Run, QStringLiteral("run"));
                setStatus(QStringLiteral(
                        "Loading scenario in the Reference engine..."));
            }
        } else if (hasCurrentCommand_) {
            const DebuggerCommand completed = currentCommand_;
            hasCurrentCommand_ = false;
            commandInFlight_ = false;
            processCommandOutputAsync(completed, output);
            return;
        }
        handlingDebuggerOutput_ = false;
        sendNextCommand();
        promptPosition = debuggerBuffer_.indexOf(prompt);
    }
}

void SimulationDebuggerModel::processCommandOutputAsync(
        const DebuggerCommand &command, const QString &output) {
    const bool cacheVariables = command.kind == CommandKind::Variables;
    const bool parseVariables = cacheVariables || command.kind == CommandKind::Watch;
    const bool parseStopLocation =
            command.kind == CommandKind::Run ||
            command.kind == CommandKind::Continue ||
            command.kind == CommandKind::Substep ||
            command.kind == CommandKind::SourceLineStep ||
            command.kind == CommandKind::RefreshLocation;
    const QString printToken = command.printToken;
    QHash<QString, QStringList> sourceLines;
    if (cacheVariables) {
        sourceLines.reserve(static_cast<qsizetype>(sources_.size()));
        for (const SourceFile &source : sources_) {
            sourceLines.insert(source.path, source.currentLines);
        }
    }
    const quint64 sourceRevision = sourceRevision_;
    const quint64 generation = sessionGeneration_;
    outputProcessing_ = true;
    auto *const watcher = new QFutureWatcher<ProcessedDebuggerOutput>(this);
    connect(watcher, &QFutureWatcher<ProcessedDebuggerOutput>::finished, this,
            [this, watcher, command, generation]() {
                ProcessedDebuggerOutput output = watcher->result();
                watcher->deleteLater();
                if (generation != sessionGeneration_) {
                    return;
                }
                if (!output.workerErrors.isEmpty()) {
                    failSession(output.workerErrors.constFirst());
                } else if (active_) {
                    for (const QVariant &frame : output.frames) {
                        applyWorkerFrame(frame.toMap());
                    }
                    handleCommandResult(command, output);
                }
                outputProcessing_ = false;
                handlingDebuggerOutput_ = false;
                sendNextCommand();
                consumeDebuggerPrompts();
            });
    watcher->setFuture(QtConcurrent::run(
            [output, parseVariables, cacheVariables, parseStopLocation, printToken,
             sourceRevision, sourceLines = std::move(sourceLines)]() {
                ProcessedDebuggerOutput processed = processDebuggerOutput(
                        output, parseVariables, parseStopLocation,
                        printToken);
                if (cacheVariables) {
                    processed.sourceRevision = sourceRevision;
                    processed.inlineValuesBySource = buildInlineValueCache(
                            processed.parsedVariables, sourceLines);
                }
                return processed;
            }));
}

void SimulationDebuggerModel::handleCommandResult(
        const DebuggerCommand &command, const ProcessedDebuggerOutput &output) {
    switch (command.kind) {
    case CommandKind::Setting:
    case CommandKind::FunctionBreakpoint:
        if (output.commandFailed) {
            failSession(
                    QStringLiteral("LLDB rejected required debugger setup: %1")
                            .arg(output.diagnostics));
        }
        break;
    case CommandKind::SourceBreakpoint:
        handleSourceBreakpointResult(command, output.rawOutput);
        scheduleAdvance();
        break;
    case CommandKind::SourceBreakpointLookup:
        handleSourceBreakpointLookupResult(command, output.rawOutput);
        scheduleAdvance();
        break;
    case CommandKind::SourceBreakpointRemove:
        if (output.commandFailed) {
            installedBreakpointKeys_.insert(command.sourcePath);
            installedBreakpointIds_.insert(command.sourcePath, command.line);
            editError_ =
                    QStringLiteral("Could not remove native breakpoint %1: %2")
                            .arg(command.line)
                            .arg(output.diagnostics);
            emit linesChanged();
        }
        scheduleAdvance();
        break;
    case CommandKind::Run:
    case CommandKind::Continue:
    case CommandKind::Substep:
    case CommandKind::SourceLineStep:
    case CommandKind::RefreshLocation:
        handleDebuggerStop(output);
        break;
    case CommandKind::Variables:
        applyParsedVariables(output);
        if (running_) {
            scheduleAdvance();
        }
        break;
    case CommandKind::Watch:
        if (command.watchGeneration == watchGeneration_ && active_ && !running_ && !stepping_ &&
            command.sourcePath == activeFilePath_ && command.sourceLine == activeSourceLine_) {
            for (auto &watch : watches_) {
                auto row = watch.toMap();
                if (row.value(QStringLiteral("id")) != command.watchId) continue;
                const bool readable = !output.commandFailed && !output.parsedVariables.isEmpty();
                const auto variable = readable ? output.parsedVariables.front().toMap() : QVariantMap{};
                row.insert(QStringLiteral("status"), readable ? QStringLiteral("Ready") : QStringLiteral("Out of scope or unreadable"));
                row.insert(QStringLiteral("value"), variable.value(QStringLiteral("value")).toString().left(4096));
                row.insert(QStringLiteral("type"), variable.value(QStringLiteral("type")));
                row.insert(QStringLiteral("location"), fileName(activeFilePath_) + ':' + QString::number(activeSourceLine_));
                watch = row;
            }
            emit watchesChanged();
        }
        if (running_) scheduleAdvance();
        break;
    case CommandKind::EvaluateEdit: {
        appendDebugOutput(command, output.printedLines);
        compiling_ = false;
        emit stateChanged();
        QString diagnostic = output.diagnostics;
        if (diagnostic.startsWith(command.text)) {
            diagnostic.remove(0, command.text.size());
            diagnostic = diagnostic.trimmed();
        }
        if (output.commandFailed) {
            cancelStep();
            boundaryEditInProgress_ = false;
            pendingBoundaryEditIds_.clear();
            pendingBoundaryEditIndex_ = 0;
            pendingBoundarySkipsOriginal_ = false;
            editError_ = diagnostic;
            if (editError_.isEmpty()) {
                editError_ = QStringLiteral(
                        "The edited C++ statement was rejected.");
            }
            setRunning(false);
            emit linesChanged();
            setStatus(QStringLiteral("Native edit did not compile."));
        } else {
            applyNextBoundaryEdit();
        }
        break;
    }
    case CommandKind::JumpAfterEdit:
        if (output.commandFailed) {
            cancelStep();
            boundaryEditInProgress_ = false;
            pendingBoundaryEditIds_.clear();
            pendingBoundaryEditIndex_ = 0;
            pendingBoundarySkipsOriginal_ = false;
            editError_ =
                    QStringLiteral("The debugger could not skip the original "
                                   "source statement: %1")
                            .arg(output.diagnostics);
            setRunning(false);
            compiling_ = false;
            emit linesChanged();
        } else {
            boundaryEditInProgress_ = false;
            executedLinesThisTick_.insert(currentLineKey_);
            if (stepping_) {
                if (stepMode_ == StepMode::Tick) {
                    queueCommand(CommandKind::Continue,
                                 QStringLiteral("continue"));
                } else {
                    queueCommand(CommandKind::RefreshLocation,
                                 QStringLiteral("frame info"));
                }
            } else {
                scheduleAdvance();
            }
        }
        break;
    case CommandKind::Quit:
        break;
    }
}

void SimulationDebuggerModel::handleSourceBreakpointResult(
        const DebuggerCommand &command, const QString &output) {
    const QString requestedKey = lineKey(command.sourcePath, command.line);
    const int index = sourceIndex(command.sourcePath);
    if (index < 0) {
        installedBreakpointKeys_.remove(requestedKey);
        return;
    }
    SourceFile &source = sources_[static_cast<std::size_t>(index)];

    static const QRegularExpression locationPattern(
            QStringLiteral("\\bat\\s+(.+?):(\\d+)(?::\\d+)?(?:\\s|,|$)"));
    static const QRegularExpression idPattern(
            QStringLiteral("\\bBreakpoint\\s+(\\d+):"),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression locationCountPattern(
            QStringLiteral("\\bBreakpoint\\s+\\d+:\\s+(\\d+)\\s+locations?\\b"),
            QRegularExpression::CaseInsensitiveOption);
    const int breakpointId = idPattern.match(output).captured(1).toInt();
    int resolvedLine = -1;
    QRegularExpressionMatchIterator locations =
            locationPattern.globalMatch(output);
    while (locations.hasNext()) {
        const QRegularExpressionMatch location = locations.next();
        const QString resolvedPath =
                relativeSourcePath(QDir::cleanPath(location.captured(1)));
        if (resolvedPath == command.sourcePath ||
            QFileInfo(location.captured(1)).fileName() ==
                    fileName(command.sourcePath)) {
            resolvedLine = location.captured(2).toInt();
            break;
        }
    }
    if (resolvedLine <= 0 &&
        locationCountPattern.match(output).captured(1).toInt() > 0) {
        resolvedLine = command.line;
    }

    const bool unresolved =
            resolvedLine <= 0 ||
            output.contains(QStringLiteral("no locations (pending)"),
                            Qt::CaseInsensitive) ||
            output.contains(QStringLiteral("Unable to resolve breakpoint"),
                            Qt::CaseInsensitive);
    if (debuggerCommandFailed(output) || unresolved) {
        installedBreakpointKeys_.remove(requestedKey);
        installedBreakpointIds_.remove(requestedKey);
        if (breakpointId > 0) {
            queueCommand(
                    CommandKind::SourceBreakpointRemove,
                    QStringLiteral("breakpoint delete %1").arg(breakpointId),
                    requestedKey, breakpointId);
        }
        bool hasBoundaryEdit = false;
        const auto deletedEdit =
                source.deletedSourceLines.constFind(command.line);
        hasBoundaryEdit =
                deletedEdit != source.deletedSourceLines.cend() &&
                deletedEdit->affectsExecution;
        for (auto edit = source.edits.cbegin();
             !hasBoundaryEdit && edit != source.edits.cend(); ++edit) {
            const int displayLine = displayLineForId(source, edit.key());
            hasBoundaryEdit =
                    anchorLineForDisplayLine(source, displayLine) ==
                    command.line;
        }
        int fallbackLine = command.line + 1;
        while (fallbackLine <= source.originalLines.size() &&
               !IsEditableSourceLine(
                       source.originalLines[fallbackLine - 1])) {
            ++fallbackLine;
        }
        if (hasBoundaryEdit &&
            fallbackLine <= source.originalLines.size()) {
            bool movedUserBreakpoint = false;
            for (int displayIndex = 0;
                 displayIndex < source.anchorLineNumbers.size();
                 ++displayIndex) {
                if (source.anchorLineNumbers[displayIndex] == command.line) {
                    movedUserBreakpoint |= source.breakpoints.contains(
                            source.lineIds[displayIndex]);
                    source.anchorLineNumbers[displayIndex] = fallbackLine;
                }
            }
            if (deletedEdit != source.deletedSourceLines.cend()) {
                const SourceEdit edit = deletedEdit.value();
                source.deletedSourceLines.remove(command.line);
                source.deletedSourceLines.insert(fallbackLine, edit);
            }
            if (movedUserBreakpoint) {
                saveBreakpoints();
                emit filesChanged();
            }
            editError_.clear();
            syncSourceBreakpoints(source);
            emit linesChanged();
            setStatus(QStringLiteral(
                              "Inserted code mapped to the next native source "
                              "boundary at %1:%2.")
                              .arg(fileName(command.sourcePath))
                              .arg(fallbackLine));
            return;
        }
        if (sourceWantsBreakpoint(source, command.line)) {
            queueCommand(
                    CommandKind::SourceBreakpointLookup,
                    QStringLiteral("image lookup -v -f %1 -l %2")
                            .arg(quoteDebuggerArgument(source.absolutePath))
                            .arg(command.line),
                    command.sourcePath, command.line);
            setStatus(QStringLiteral(
                              "Resolving breakpoint to native source code in "
                              "%1.")
                              .arg(fileName(command.sourcePath)));
            return;
        }
        bool removedUserBreakpoint = false;
        const QList<quint64> breakpointIds = source.breakpoints.values();
        for (const quint64 lineId : breakpointIds) {
            const int displayLine = displayLineForId(source, lineId);
            if (anchorLineForDisplayLine(source, displayLine) ==
                command.line) {
                source.breakpoints.remove(lineId);
                removedUserBreakpoint = true;
            }
        }
        if (removedUserBreakpoint) {
            saveBreakpoints();
            emit filesChanged();
        }
        editError_ =
                QStringLiteral("Line %1 has no executable debugger location.")
                        .arg(command.line);
        const QString diagnostic = TrimDebuggerNoise(output);
        if (!diagnostic.isEmpty()) {
            editError_ += QLatin1Char('\n') + diagnostic;
        }
        emit linesChanged();
        setStatus(QStringLiteral("Breakpoint could not be resolved."));
        return;
    }

    const bool stillWanted =
            sourceWantsBreakpoint(source, command.line);
    if (!stillWanted) {
        installedBreakpointKeys_.remove(requestedKey);
        if (breakpointId > 0) {
            queueCommand(
                    CommandKind::SourceBreakpointRemove,
                    QStringLiteral("breakpoint delete %1").arg(breakpointId),
                    requestedKey, breakpointId);
        }
        return;
    }

    if (breakpointId > 0) {
        installedBreakpointIds_.insert(requestedKey, breakpointId);
    }
    source.executableSourceLines.insert(resolvedLine);
    if (resolvedLine != command.line) {
        QList<quint64> movedBreakpointIds;
        for (int displayIndex = 0;
             displayIndex < source.anchorLineNumbers.size();
             ++displayIndex) {
            if (source.anchorLineNumbers[displayIndex] != command.line) {
                continue;
            }
            if (source.breakpoints.contains(source.lineIds[displayIndex])) {
                movedBreakpointIds.push_back(source.lineIds[displayIndex]);
            }
            source.anchorLineNumbers[displayIndex] = resolvedLine;
        }
        for (const quint64 lineId : movedBreakpointIds) {
            source.breakpoints.remove(lineId);
        }
        if (!movedBreakpointIds.isEmpty()) {
            const int resolvedDisplayLine =
                    displayLineForSourceLine(source, resolvedLine);
            if (resolvedDisplayLine > 0) {
                source.breakpoints.insert(
                        source.lineIds[resolvedDisplayLine - 1]);
            }
        }
        const auto deleted =
                source.deletedSourceLines.constFind(command.line);
        if (deleted != source.deletedSourceLines.cend()) {
            const SourceEdit edit = deleted.value();
            source.deletedSourceLines.remove(command.line);
            const auto existing =
                    source.deletedSourceLines.constFind(resolvedLine);
            if (existing == source.deletedSourceLines.cend() ||
                edit.effectiveTick < existing->effectiveTick) {
                source.deletedSourceLines.insert(resolvedLine, edit);
            }
        }
        installedBreakpointKeys_.remove(requestedKey);
        installedBreakpointIds_.remove(requestedKey);
        const QString resolvedKey = lineKey(command.sourcePath, resolvedLine);
        installedBreakpointKeys_.insert(resolvedKey);
        if (breakpointId > 0) {
            installedBreakpointIds_.insert(resolvedKey, breakpointId);
        }
        if (!movedBreakpointIds.isEmpty()) {
            saveBreakpoints();
            emit filesChanged();
        }
        emit linesChanged();
        if (!movedBreakpointIds.isEmpty()) {
            setStatus(QStringLiteral(
                              "Breakpoint moved to executable line %1 in %2.")
                              .arg(resolvedLine)
                              .arg(fileName(command.sourcePath)));
        }
    }
}

void SimulationDebuggerModel::handleSourceBreakpointLookupResult(
        const DebuggerCommand &command, const QString &output) {
    const int index = sourceIndex(command.sourcePath);
    if (index < 0) {
        return;
    }
    SourceFile &source = sources_[static_cast<std::size_t>(index)];
    if (!sourceWantsBreakpoint(source, command.line)) {
        return;
    }

    static const QRegularExpression locationPattern(
            QStringLiteral("\\bat\\s+(.+?):(\\d+)(?::\\d+)?(?:\\s|,|$)"));
    int resolvedLine = -1;
    QRegularExpressionMatchIterator locations =
            locationPattern.globalMatch(output);
    while (locations.hasNext()) {
        const QRegularExpressionMatch location = locations.next();
        const QString resolvedPath =
                relativeSourcePath(QDir::cleanPath(location.captured(1)));
        if (resolvedPath == command.sourcePath ||
            QFileInfo(location.captured(1)).fileName() ==
                    fileName(command.sourcePath)) {
            resolvedLine = location.captured(2).toInt();
            break;
        }
    }

    const int resolvedDisplayLine =
            displayLineForSourceLine(source, resolvedLine);
    if (resolvedLine <= 0 || resolvedLine == command.line ||
        resolvedDisplayLine <= 0) {
        const QList<quint64> breakpointIds = source.breakpoints.values();
        for (const quint64 lineId : breakpointIds) {
            const int displayLine = displayLineForId(source, lineId);
            if (anchorLineForDisplayLine(source, displayLine) ==
                command.line) {
                source.breakpoints.remove(lineId);
            }
        }
        saveBreakpoints();
        emit filesChanged();
        emit linesChanged();
        editError_ =
                QStringLiteral("Line %1 has no executable debugger location.")
                        .arg(command.line);
        const QString diagnostic = TrimDebuggerNoise(output);
        if (!diagnostic.isEmpty()) {
            editError_ += QLatin1Char('\n') + diagnostic;
        }
        setStatus(QStringLiteral("Breakpoint could not be resolved."));
        return;
    }

    QList<quint64> movedBreakpointIds;
    for (const quint64 lineId : source.breakpoints) {
        const int displayLine = displayLineForId(source, lineId);
        if (anchorLineForDisplayLine(source, displayLine) == command.line) {
            movedBreakpointIds.push_back(lineId);
        }
    }
    for (const quint64 lineId : movedBreakpointIds) {
        source.breakpoints.remove(lineId);
    }
    if (movedBreakpointIds.isEmpty()) {
        return;
    }
    source.breakpoints.insert(source.lineIds[resolvedDisplayLine - 1]);
    source.executableSourceLines.insert(resolvedLine);
    editError_.clear();
    saveBreakpoints();
    emit filesChanged();
    emit linesChanged();
    syncSourceBreakpoints(source);
    setStatus(QStringLiteral("Breakpoint moved to executable line %1 in %2.")
                      .arg(resolvedLine)
                      .arg(fileName(command.sourcePath)));
}

void SimulationDebuggerModel::handleDebuggerStop(
        const ProcessedDebuggerOutput &output) {
    if (!active_) {
        return;
    }
    if (output.tickBoundary) {
        invalidateWatches(QStringLiteral("Paused frame unavailable"));
        if (!currentLineKey_.isEmpty()) {
            executedLinesThisTick_.insert(currentLineKey_);
        }
        currentLineKey_.clear();
        executedLinesThisTick_.clear();
        activeLine_ = -1;
        activeSourceLine_ = -1;
        activeFilePath_.clear();
        pendingBoundaryEditIds_.clear();
        pendingBoundaryEditIndex_ = 0;
        boundaryEditInProgress_ = false;
        pendingBoundarySkipsOriginal_ = false;
        atTickBoundary_ = true;
        editInterruptRequested_ = false;
        emit executionChanged();
        emit linesChanged();
        emit filesChanged();
        for (SourceFile &source : sources_) {
            syncSourceBreakpoints(source);
        }
        if (stepping_) {
            finishStep(
                    stepMode_ == StepMode::Tick
                            ? QStringLiteral(
                                      "Advanced exactly one physics tick.")
                            : QStringLiteral(
                                      "Step reached the next tick boundary."));
        } else if (pauseRequested_) {
            setStatus(QStringLiteral(
                    "Locating the next native source-line boundary..."));
            queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        } else {
            setStatus(QStringLiteral("Reference tick %1 completed.")
                              .arg(executionTick_));
            scheduleAdvance();
        }
        return;
    }
    if (!output.stopPath.isEmpty() && output.stopLine > 0) {
        handleSourceStop(output.stopPath, output.stopLine);
        return;
    }
    if (output.rawOutput.contains(QStringLiteral("exited with status")) ||
        output.rawOutput.contains(QStringLiteral("Process 0 exited"))) {
        return;
    }
    if (editInterruptRequested_) {
        editInterruptRequested_ = false;
        scheduleAdvance();
        return;
    }
    if (stepping_) {
        if (stepMode_ == StepMode::Tick) {
            queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        } else {
            cancelStep();
            setStatus(QStringLiteral(
                    "The debugger step did not resolve to a source line."));
        }
        return;
    }
    if (running_) {
        queueCommand(CommandKind::Continue, QStringLiteral("continue"));
    }
}

void SimulationDebuggerModel::handleSourceStop(const QString &absolutePath,
                                               int line) {
    const QString relative = relativeSourcePath(absolutePath);
    if (!workerReady_) {
        queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        return;
    }
    if (relative.isEmpty() || line <= 0) {
        if (stepping_) {
            if (stepMode_ == StepMode::Tick) {
                queueCommand(CommandKind::Continue, QStringLiteral("continue"));
            } else {
                finishStep(QStringLiteral(
                        "Step paused outside the explorable source tree."));
            }
        } else if (running_ || pauseRequested_) {
            editInterruptRequested_ = false;
            queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        } else {
            setStatus(QStringLiteral("Paused in the native reference engine."));
        }
        return;
    }
    pauseRequested_ = false;
    editInterruptRequested_ = false;
    atTickBoundary_ = false;
    if (!currentLineKey_.isEmpty() &&
        currentLineKey_ != lineKey(relative, line)) {
        executedLinesThisTick_.insert(currentLineKey_);
    }
    const int index = sourceIndex(relative);
    if (index < 0) {
        queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        return;
    }
    SourceFile &source = sources_[static_cast<std::size_t>(index)];
    activeFilePath_ = relative;
    activeSourceLine_ = line;
    activeLine_ = displayLineForSourceLine(source, line);
    currentLineKey_ = lineKey(relative, line);
    source.executableSourceLines.insert(line);
    bool userBreakpoint = false;
    for (const quint64 lineId : source.breakpoints) {
        const int displayLine = displayLineForId(source, lineId);
        if (anchorLineForDisplayLine(source, displayLine) == line) {
            userBreakpoint = true;
            break;
        }
    }
    const bool stopsAtUserBreakpoint =
            userBreakpoint && lastBreakpointKey_ != currentLineKey_;
    if (stopsAtUserBreakpoint) {
        lastBreakpointKey_ = currentLineKey_;
        setRunning(false);
        cancelStep();
        setStatus(QStringLiteral("Paused at breakpoint %1:%2.")
                          .arg(fileName(relative))
                          .arg(activeLine_));
    } else if (!userBreakpoint) {
        lastBreakpointKey_.clear();
    }
    if (!running_ || stepping_) {
        selectFile(relative);
    }
    emit filesChanged();
    emit executionChanged();
    emit linesChanged();

    if (stepping_ && stepMode_ == StepMode::Tick) {
        if (!executedLinesThisTick_.contains(currentLineKey_) &&
            hasApplicableEdits(source, activeSourceLine_)) {
            applyCurrentEdit();
        } else {
            queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        }
    } else if (stepping_) {
        const QString status =
                stepMode_ == StepMode::Substep
                        ? QStringLiteral("Substep completed at %1:%2.")
                                  .arg(fileName(relative))
                                  .arg(line)
                        : QStringLiteral("Source-line step completed at %1:%2.")
                                  .arg(fileName(relative))
                                  .arg(line);
        finishStep(status);
        clearVariables();
        queueCommand(CommandKind::Variables,
                     QStringLiteral("frame variable --show-types"));
    } else if (!running_) {
        if (!userBreakpoint) {
            setStatus(QStringLiteral("Paused at %1:%2.")
                              .arg(fileName(relative))
                              .arg(activeLine_));
        }
        clearVariables();
        queueCommand(CommandKind::Variables,
                     QStringLiteral("frame variable --show-types"));
    } else if (running_) {
        scheduleAdvance();
    }
}

void SimulationDebuggerModel::applyParsedVariables(
        const ProcessedDebuggerOutput &output) {
    ++inlineCacheGeneration_;
    variables_.clear();
    variables_.reserve(static_cast<std::size_t>(output.parsedVariables.size()));
    for (const QVariant &value : output.parsedVariables) {
        const QVariantMap entry = value.toMap();
        const QString name = entry.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            continue;
        }
        variables_.push_back(Variable{
                name, entry.value(QStringLiteral("value")).toString(),
                entry.value(QStringLiteral("type")).toString(), name, 0});
    }
    if (output.sourceRevision == sourceRevision_) {
        applyInlineValueCache(output.inlineValuesBySource);
    } else {
        refreshInlineValueCacheAsync(output.parsedVariables);
    }
    refreshWatches();
    emit linesChanged();
}

void SimulationDebuggerModel::appendDebugOutput(
        const DebuggerCommand &command, const QStringList &messages) {
    if (messages.isEmpty() || command.sourcePath.isEmpty() ||
        command.line <= 0) {
        return;
    }
    constexpr qsizetype kMaximumOutputEntries = 10000;
    for (const QString &message : messages) {
        debugOutput_.push_back(QVariantMap{
                {QStringLiteral("sequence"),
                 static_cast<qulonglong>(++debugOutputSequence_)},
                {QStringLiteral("message"), message},
                {QStringLiteral("tick"), executionTick_},
                {QStringLiteral("context"), executionContextLabel()},
                {QStringLiteral("path"), command.sourcePath},
                {QStringLiteral("line"), command.line},
                {QStringLiteral("lineId"),
                 static_cast<qulonglong>(command.lineId)},
                {QStringLiteral("sourceLine"), command.sourceLine},
                {QStringLiteral("location"),
                 fileName(command.sourcePath) + QLatin1Char(':') +
                         QString::number(command.line)}});
    }
    if (debugOutput_.size() > kMaximumOutputEntries) {
        debugOutput_.remove(0, debugOutput_.size() - kMaximumOutputEntries);
    }
    emit debugOutputChanged();
}

void SimulationDebuggerModel::applyInlineValueCache(
        const QHash<QString, QStringList> &inlineValuesBySource) {
    for (SourceFile &source : sources_) {
        source.inlineValueLines = inlineValuesBySource.value(source.path);
        if (source.inlineValueLines.size() != source.currentLines.size()) {
            source.inlineValueLines.clear();
            source.inlineValueLines.resize(source.currentLines.size());
        }
    }
}

void SimulationDebuggerModel::refreshInlineValueCacheAsync(
        const QVariantList &values) {
    QHash<QString, QStringList> sourceLines;
    sourceLines.reserve(static_cast<qsizetype>(sources_.size()));
    for (const SourceFile &source : sources_) {
        sourceLines.insert(source.path, source.currentLines);
    }
    const quint64 generation = ++inlineCacheGeneration_;
    const quint64 sourceRevision = sourceRevision_;
    auto *const watcher = new QFutureWatcher<QHash<QString, QStringList>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, QStringList>>::finished,
            this, [this, watcher, generation, sourceRevision, values]() {
                const QHash<QString, QStringList> cache = watcher->result();
                watcher->deleteLater();
                if (generation != inlineCacheGeneration_) {
                    return;
                }
                if (sourceRevision != sourceRevision_) {
                    refreshInlineValueCacheAsync(values);
                    return;
                }
                applyInlineValueCache(cache);
                emit linesChanged();
            });
    watcher->setFuture(
            QtConcurrent::run([values, sourceLines = std::move(sourceLines)]() {
                return buildInlineValueCache(values, sourceLines);
            }));
}

void SimulationDebuggerModel::clearVariables() {
    invalidateWatches(QStringLiteral("Waiting for pause"));
    ++inlineCacheGeneration_;
    variables_.clear();
    for (SourceFile &source : sources_) {
        source.inlineValueLines.clear();
        source.inlineValueLines.resize(source.currentLines.size());
    }
}

void SimulationDebuggerModel::applyWorkerFrame(const QVariantMap &frame) {
    workerReady_ = true;
    executionTick_ = frame.value(QStringLiteral("tick")).toLongLong();
    emit frameProduced(frame);
    emit executionChanged();
}

bool SimulationDebuggerModel::debuggerCommandFailed(
        const QString &output) const {
    static const QRegularExpression errorPattern(
            QStringLiteral("(error:|Errors occurred while evaluating|"
                           "<user expression [^>]*>:\\d+:\\d+: error:)"),
            QRegularExpression::CaseInsensitiveOption);
    return errorPattern.match(output).hasMatch();
}

void SimulationDebuggerModel::setStatus(const QString &status) {
    if (statusText_ == status) {
        return;
    }
    statusText_ = status;
    emit stateChanged();
}

void SimulationDebuggerModel::scheduleAdvance() {
    if (!active_ || !running_ || compiling_ || commandInFlight_ ||
        advanceScheduled_) {
        return;
    }
    advanceScheduled_ = true;
    QTimer::singleShot(8, this, [this]() {
        advanceScheduled_ = false;
        advanceExecution();
    });
}

bool SimulationDebuggerModel::beginStep(StepMode mode) {
    if (mode == StepMode::None ||
        (mode == StepMode::Tick ? !canStepTick() : !canStepSource())) {
        return false;
    }
    pauseRequested_ = false;
    editError_.clear();
    stepMode_ = mode;
    stepping_ = true;
    invalidateWatches(QStringLiteral("Stepping"));
    emit stateChanged();
    emit executionChanged();

    const int index = sourceIndex(activeFilePath_);
    if (index >= 0 && activeSourceLine_ > 0 &&
        !executedLinesThisTick_.contains(currentLineKey_) &&
        hasApplicableEdits(sources_[static_cast<std::size_t>(index)],
                           activeSourceLine_)) {
        setStatus(QStringLiteral(
                "Executing the edited source line before stepping."));
        applyCurrentEdit();
        return true;
    }
    queueStepCommand();
    return true;
}

void SimulationDebuggerModel::queueStepCommand() {
    switch (stepMode_) {
    case StepMode::Substep:
        setStatus(QStringLiteral("Advancing one native substep..."));
        queueCommand(CommandKind::Substep, QStringLiteral("thread step-inst"));
        break;
    case StepMode::SourceLine:
        setStatus(QStringLiteral("Advancing one source line..."));
        queueCommand(CommandKind::SourceLineStep,
                     QStringLiteral("thread step-over"));
        break;
    case StepMode::Tick:
        setStatus(QStringLiteral("Advancing one physics tick..."));
        queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        break;
    case StepMode::None:
        break;
    }
}

void SimulationDebuggerModel::finishStep(const QString &status) {
    if (!stepping_) {
        return;
    }
    stepping_ = false;
    invalidateWatches(QStringLiteral("Waiting for paused frame"));
    stepMode_ = StepMode::None;
    if (!status.isEmpty()) {
        setStatus(status);
    }
    emit stateChanged();
    emit executionChanged();
}

void SimulationDebuggerModel::cancelStep() {
    if (!stepping_ && stepMode_ == StepMode::None) {
        return;
    }
    stepping_ = false;
    stepMode_ = StepMode::None;
    emit stateChanged();
    emit executionChanged();
}

void SimulationDebuggerModel::advanceExecution() {
    if (!active_ || !running_ || compiling_ || commandInFlight_) {
        return;
    }
    if (atTickBoundary_) {
        queueCommand(CommandKind::Continue, QStringLiteral("continue"));
        return;
    }
    const int index = sourceIndex(activeFilePath_);
    if (index >= 0 && activeSourceLine_ > 0 &&
        !executedLinesThisTick_.contains(currentLineKey_) &&
        hasApplicableEdits(sources_[static_cast<std::size_t>(index)],
                           activeSourceLine_)) {
        applyCurrentEdit();
        return;
    }
    queueCommand(CommandKind::Continue, QStringLiteral("continue"));
}

void SimulationDebuggerModel::applyCurrentEdit() {
    const int index = sourceIndex(activeFilePath_);
    if (index < 0 || activeSourceLine_ <= 0) {
        scheduleAdvance();
        return;
    }
    const SourceFile &source = sources_[static_cast<std::size_t>(index)];
    pendingBoundaryEditIds_.clear();
    pendingBoundaryEditIndex_ = 0;
    pendingBoundarySkipsOriginal_ = false;
    const auto deleted =
            source.deletedSourceLines.constFind(activeSourceLine_);
    if (deleted != source.deletedSourceLines.cend() &&
        deleted->affectsExecution &&
        deleted->effectiveTick <= executionTick_) {
        pendingBoundarySkipsOriginal_ = true;
    }
    for (int displayIndex = 0; displayIndex < source.lineIds.size();
         ++displayIndex) {
        const quint64 lineId = source.lineIds[displayIndex];
        const auto edit = source.edits.constFind(lineId);
        if (edit == source.edits.cend() ||
            edit->effectiveTick > executionTick_ ||
            source.anchorLineNumbers[displayIndex] != activeSourceLine_) {
            continue;
        }
        pendingBoundaryEditIds_.push_back(lineId);
        if (source.sourceLineNumbers[displayIndex] > 0) {
            pendingBoundarySkipsOriginal_ = true;
        }
    }
    if (pendingBoundaryEditIds_.isEmpty() &&
        !pendingBoundarySkipsOriginal_) {
        if (stepping_) {
            queueStepCommand();
        } else {
            scheduleAdvance();
        }
        return;
    }
    boundaryEditInProgress_ = true;
    editError_.clear();
    applyNextBoundaryEdit();
}

void SimulationDebuggerModel::applyNextBoundaryEdit() {
    const int index = sourceIndex(activeFilePath_);
    if (!boundaryEditInProgress_ || index < 0 ||
        activeSourceLine_ <= 0) {
        return;
    }
    const SourceFile &source = sources_[static_cast<std::size_t>(index)];
    while (pendingBoundaryEditIndex_ < pendingBoundaryEditIds_.size()) {
        const quint64 lineId =
                pendingBoundaryEditIds_[pendingBoundaryEditIndex_++];
        const int displayLine = displayLineForId(source, lineId);
        if (displayLine <= 0) {
            continue;
        }
        const auto edit = source.edits.constFind(lineId);
        if (edit == source.edits.cend() ||
            edit->effectiveTick > executionTick_ ||
            anchorLineForDisplayLine(source, displayLine) !=
                    activeSourceLine_) {
            continue;
        }
        QString expression =
                source.currentLines[displayLine - 1].trimmed();
        if (!IsEditableSourceLine(expression)) {
            continue;
        }
        if (activeLine_ != displayLine) {
            activeLine_ = displayLine;
            emit executionChanged();
            emit linesChanged();
        }
        compiling_ = true;
        setStatus(QStringLiteral(
                "Compiling edited C++ in the live physics frame..."));
        emit stateChanged();
        if (expression.endsWith(QLatin1Char(';'))) {
            expression.chop(1);
        }
        QString printToken;
        if (IsExplicitPrintExpression(expression)) {
            printToken =
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
            static const QRegularExpression printfPattern(
                    QStringLiteral("(^|[^A-Za-z0-9_.>:])(?:std::)?"
                                   "printf(?=\\s*\\()"));
            static const QRegularExpression putsPattern(
                    QStringLiteral("(^|[^A-Za-z0-9_.>:])(?:std::)?"
                                   "puts(?=\\s*\\()"));
            expression.replace(printfPattern,
                               QStringLiteral("\\1__builtin_printf"));
            expression.replace(putsPattern,
                               QStringLiteral("\\1__builtin_puts"));
            expression =
                    QStringLiteral(
                            "(void)(__builtin_printf("
                            "\"@FOREVERTAS_DEBUG_PRINT_BEGIN_%1@\\n\"), "
                            "(void)(%2), __builtin_printf("
                            "\"@FOREVERTAS_DEBUG_PRINT_END_%1@\\n\"))")
                            .arg(printToken, expression);
        }
        expression.prepend(QStringLiteral("expression -- "));
        queueCommand(CommandKind::EvaluateEdit, expression,
                     activeFilePath_, displayLine, printToken, lineId,
                     activeSourceLine_);
        return;
    }
    finishBoundaryEdits();
}

void SimulationDebuggerModel::finishBoundaryEdits() {
    boundaryEditInProgress_ = false;
    pendingBoundaryEditIds_.clear();
    pendingBoundaryEditIndex_ = 0;
    if (pendingBoundarySkipsOriginal_) {
        pendingBoundarySkipsOriginal_ = false;
        setStatus(QStringLiteral(
                "Applying inserted code and skipping the original statement."));
        jumpPastCurrentStatement();
        return;
    }
    pendingBoundarySkipsOriginal_ = false;
    executedLinesThisTick_.insert(currentLineKey_);
    const int index = sourceIndex(activeFilePath_);
    if (index >= 0) {
        const int originalDisplayLine = displayLineForSourceLine(
                sources_[static_cast<std::size_t>(index)],
                activeSourceLine_);
        if (originalDisplayLine != activeLine_) {
            activeLine_ = originalDisplayLine;
            emit executionChanged();
            emit linesChanged();
        }
    }
    if (stepping_) {
        queueStepCommand();
    } else if (running_) {
        scheduleAdvance();
    }
}

void SimulationDebuggerModel::jumpPastCurrentStatement() {
    const int index = sourceIndex(activeFilePath_);
    if (index < 0) {
        failSession(QStringLiteral("Edited source file was not found."));
        return;
    }
    const SourceFile &source = sources_[static_cast<std::size_t>(index)];
    const int nextLine =
            statementEndLine(source, activeSourceLine_) + 1;
    const QString command =
            QStringLiteral("thread jump --file %1 --line %2")
                    .arg(quoteDebuggerArgument(source.absolutePath))
                    .arg(nextLine);
    queueCommand(CommandKind::JumpAfterEdit, command);
}

void SimulationDebuggerModel::clearExecutionLocation() {
    invalidateWatches(QStringLiteral("Stopped"));
    activeLine_ = -1;
    activeSourceLine_ = -1;
    activeFilePath_.clear();
    pendingBoundaryEditIds_.clear();
    pendingBoundaryEditIndex_ = 0;
    boundaryEditInProgress_ = false;
    pendingBoundarySkipsOriginal_ = false;
    emit executionChanged();
    emit linesChanged();
    emit filesChanged();
}

void SimulationDebuggerModel::failSession(const QString &message) {
    if (stopping_) {
        return;
    }
    const bool wasActive = active_;
    setRunning(false);
    cancelStep();
    active_ = false;
    compiling_ = false;
    commandInFlight_ = false;
    advanceScheduled_ = false;
    commandQueue_.clear();
    hasCurrentCommand_ = false;
    editError_ = message;
    setStatus(message);
    emit linesChanged();
    emit stateChanged();
    if (debugger_.state() != QProcess::NotRunning) {
        debugger_.write("quit\n");
        QTimer::singleShot(250, this, [this]() {
            if (!active_ && !stopping_ &&
                debugger_.state() != QProcess::NotRunning) {
                debugger_.terminate();
            }
        });
    }
    if (wasActive) {
        emit sessionFinished();
    }
}

void SimulationDebuggerModel::setRunning(bool value) {
    if (running_ == value) {
        return;
    }
    running_ = value;
    if (value) invalidateWatches(QStringLiteral("Running"));
    emit stateChanged();
}

} // namespace forevertas::viewer
