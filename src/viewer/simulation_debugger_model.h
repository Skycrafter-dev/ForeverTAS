#ifndef FOREVERTAS_VIEWER_SIMULATION_DEBUGGER_MODEL_H
#define FOREVERTAS_VIEWER_SIMULATION_DEBUGGER_MODEL_H

#include <QHash>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <vector>

namespace forevertas::viewer {

class SimulationDebuggerModel final : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool preparing READ preparing NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool stepping READ stepping NOTIFY stateChanged)
    Q_PROPERTY(bool compiling READ compiling NOTIFY stateChanged)
    Q_PROPERTY(bool loadingReplay READ loadingReplay NOTIFY stateChanged)
    Q_PROPERTY(bool canStepSource READ canStepSource NOTIFY stateChanged)
    Q_PROPERTY(bool canStepTick READ canStepTick NOTIFY stateChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY stateChanged)
    Q_PROPERTY(QVariantList fileEntries READ fileEntries NOTIFY filesChanged)
    Q_PROPERTY(QString selectedFilePath READ selectedFilePath NOTIFY
                       selectionChanged)
    Q_PROPERTY(QVariantList lines READ lines NOTIFY linesChanged)
    Q_PROPERTY(int activeLine READ activeLine NOTIFY executionChanged)
    Q_PROPERTY(
            QString activeFilePath READ activeFilePath NOTIFY executionChanged)
    Q_PROPERTY(QVariantList debugOutput READ debugOutput NOTIFY
                       debugOutputChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString editError READ editError NOTIFY linesChanged)
    Q_PROPERTY(bool hasEdits READ hasEdits NOTIFY filesChanged)
    Q_PROPERTY(qint64 executionTick READ executionTick NOTIFY executionChanged)
    Q_PROPERTY(
            bool darkMode READ darkMode WRITE setDarkMode NOTIFY themeChanged)

  public:
    explicit SimulationDebuggerModel(QObject *parent = nullptr);
    ~SimulationDebuggerModel() override;

    bool available() const;
    bool shutdownReady() const { return debugger_.state() == QProcess::NotRunning; }
    bool preparing() const;
    bool active() const;
    bool running() const;
    bool stepping() const;
    bool compiling() const;
    bool loadingReplay() const;
    bool canStepSource() const;
    bool canStepTick() const;
    QString backendName() const;
    QVariantList fileEntries() const;
    QString selectedFilePath() const;
    QVariantList lines() const;
    int activeLine() const;
    QString activeFilePath() const;
    QVariantList debugOutput() const;
    QString statusText() const;
    QString editError() const;
    bool hasEdits() const;
    qint64 executionTick() const;
    bool darkMode() const;

    Q_INVOKABLE bool selectFile(const QString &path);
    Q_INVOKABLE void toggleFolder(const QString &path);
    Q_INVOKABLE bool updateLine(int lineNumber, const QString &text);
    Q_INVOKABLE int insertLineAfter(int lineNumber);
    Q_INVOKABLE bool deleteLine(int lineNumber);
    Q_INVOKABLE bool toggleBreakpoint(const QString &path, int lineNumber);
    Q_INVOKABLE void clearDebugOutput();
    Q_INVOKABLE bool openDebugOutput(int index);
    Q_INVOKABLE void resetEdits();
    Q_INVOKABLE bool stepSubstep();
    Q_INVOKABLE bool stepSourceLine();
    Q_INVOKABLE bool stepTick();
    void setDarkMode(bool value);

    void configure(const QString &backendName);
    bool startSession(const QString &packsDirectory,
                      const QString &replayPath,
                      qint64 simulationHorizonMs,
                      const QString &inputScript);
    void stopSession();
    void play();
    void pause();

  signals:
    void stateChanged();
    void filesChanged();
    void selectionChanged();
    void linesChanged();
    void executionChanged();
    void debugOutputChanged();
    void sourceLocationRequested(int line);
    void frameProduced(const QVariantMap &frame);
    void sessionFinished();
    void themeChanged();

  private:
    struct SourceEdit {
        qint64 effectiveTick = 0;
        bool affectsExecution = true;
    };

    struct SourceFile {
        QString path;
        QString absolutePath;
        QStringList originalLines;
        QStringList currentLines;
        QStringList highlightedLight;
        QStringList highlightedDark;
        QStringList inlineValueLines;
        QVector<quint64> lineIds;
        QVector<int> sourceLineNumbers;
        QVector<int> anchorLineNumbers;
        QHash<quint64, SourceEdit> edits;
        QHash<int, SourceEdit> deletedSourceLines;
        QSet<quint64> breakpoints;
        QSet<int> executableSourceLines;
    };

    struct Variable {
        QString name;
        QString value;
        QString type;
        QString evaluateName;
        int variablesReference = 0;
    };

    enum class CommandKind {
        Setting,
        FunctionBreakpoint,
        SourceBreakpoint,
        SourceBreakpointLookup,
        SourceBreakpointRemove,
        Run,
        Continue,
        Substep,
        SourceLineStep,
        RefreshLocation,
        Variables,
        EvaluateEdit,
        JumpAfterEdit,
        Quit,
    };

    struct DebuggerCommand {
        CommandKind kind = CommandKind::Setting;
        QString text;
        QString sourcePath;
        QString printToken;
        quint64 lineId = 0;
        int line = 0;
        int sourceLine = 0;
    };

    struct ProcessedDebuggerOutput {
        QString rawOutput;
        QString diagnostics;
        QString stopPath;
        QStringList workerErrors;
        QStringList printedLines;
        QVariantList frames;
        QVariantList parsedVariables;
        QHash<QString, QStringList> inlineValuesBySource;
        quint64 sourceRevision = 0;
        int stopLine = -1;
        bool commandFailed = false;
        bool tickBoundary = false;
    };

    enum class StepMode {
        None,
        Substep,
        SourceLine,
        Tick,
    };

    static QString syntaxHighlighted(const QString &text, bool darkMode);
    static ProcessedDebuggerOutput
    processDebuggerOutput(const QString &output, bool parseVariables,
                          bool parseStopLocation, const QString &printToken);
    static QHash<QString, QStringList>
    buildInlineValueCache(const QVariantList &variables,
                          const QHash<QString, QStringList> &sourceLines);
    static QString fileName(const QString &path);
    static int depth(const QString &path);
    static QString lldbExecutablePath();
    static QString scriptExecutablePath();
    static QString workerExecutablePath();
    static QString sourceRootPath();
    static QString quoteDebuggerArgument(const QString &value);
    static QString quoteShellArgument(const QString &value);

    int sourceIndex(const QString &path) const;
    SourceFile *selectedSource();
    const SourceFile *selectedSource() const;
    int displayLineForSourceLine(const SourceFile &source,
                                 int sourceLine) const;
    int displayLineForId(const SourceFile &source, quint64 lineId) const;
    int sourceLineForDisplayLine(const SourceFile &source,
                                 int displayLine) const;
    int anchorLineForDisplayLine(const SourceFile &source,
                                 int displayLine) const;
    bool sourceWantsBreakpoint(const SourceFile &source,
                               int sourceLine) const;
    bool hasApplicableEdits(const SourceFile &source,
                            int sourceLine) const;
    qint64 effectiveTickForBoundary(const SourceFile &source,
                                    int sourceLine) const;
    QVariantList visibleFileEntries() const;
    QString inlineValues(const QString &line) const;
    QString relativeSourcePath(const QString &absolutePath) const;
    QString lineKey(const QString &path, int line) const;
    QString executionContextLabel() const;
    int statementEndLine(const SourceFile &source, int line) const;
    void loadSources();
    void restoreBreakpoints();
    void saveBreakpoints() const;
    void syncSourceBreakpoints(SourceFile &source);
    void installSourceBreakpoint(SourceFile &source, int line);
    void refreshDebugOutputLocations(const QString &path);
    void queueCommand(CommandKind kind, const QString &text,
                      const QString &sourcePath = {}, int line = 0,
                      const QString &printToken = {}, quint64 lineId = 0,
                      int sourceLine = 0);
    void sendNextCommand();
    void readDebuggerOutput();
    void consumeDebuggerPrompts();
    void processCommandOutputAsync(const DebuggerCommand &command,
                                   const QString &output);
    void handleCommandResult(const DebuggerCommand &command,
                             const ProcessedDebuggerOutput &output);
    void handleSourceBreakpointResult(const DebuggerCommand &command,
                                      const QString &output);
    void handleSourceBreakpointLookupResult(const DebuggerCommand &command,
                                            const QString &output);
    void handleDebuggerStop(const ProcessedDebuggerOutput &output);
    void handleSourceStop(const QString &absolutePath, int line);
    void applyWorkerFrame(const QVariantMap &frame);
    void applyParsedVariables(const ProcessedDebuggerOutput &output);
    void appendDebugOutput(const DebuggerCommand &command,
                           const QStringList &messages);
    void applyInlineValueCache(
            const QHash<QString, QStringList> &inlineValuesBySource);
    void refreshInlineValueCacheAsync(const QVariantList &variables);
    void clearVariables();
    bool debuggerCommandFailed(const QString &output) const;
    void setStatus(const QString &status);
    void scheduleAdvance();
    void advanceExecution();
    void applyCurrentEdit();
    void applyNextBoundaryEdit();
    void finishBoundaryEdits();
    void jumpPastCurrentStatement();
    bool beginStep(StepMode mode);
    void queueStepCommand();
    void finishStep(const QString &status = {});
    void cancelStep();
    void clearExecutionLocation();
    void failSession(const QString &message);
    void setRunning(bool value);
    void beginPendingSession();
    void stopSessionProcess(bool keepPendingStart);

    std::vector<SourceFile> sources_;
    std::vector<Variable> variables_;
    QSet<QString> expandedFolders_;
    QVariantList debugOutput_;
    QSet<QString> executedLinesThisTick_;
    QSet<QString> installedBreakpointKeys_;
    QHash<QString, int> installedBreakpointIds_;
    QQueue<DebuggerCommand> commandQueue_;
    DebuggerCommand currentCommand_;
    QProcess debugger_;
    QString selectedFilePath_;
    QString backendName_ = QStringLiteral("Reference");
    QString statusText_;
    QString editError_;
    QString debuggerBuffer_;
    QString packsDirectory_;
    QString replayPath_;
    QString inputScriptPath_;
    qint64 simulationHorizonMs_ = 6000;
    QString pendingPacksDirectory_;
    QString pendingReplayPath_;
    QString pendingInputScript_;
    qint64 pendingSimulationHorizonMs_ = 6000;
    QString currentLineKey_;
    int activeLine_ = -1;
    int activeSourceLine_ = -1;
    QString activeFilePath_;
    qint64 executionTick_ = 0;
    QString lastBreakpointKey_;
    bool available_ = false;
    bool preparing_ = false;
    bool active_ = false;
    bool running_ = false;
    bool stepping_ = false;
    bool compiling_ = false;
    bool setupQueued_ = false;
    int startupPromptsRemaining_ = 0;
    bool hasCurrentCommand_ = false;
    bool commandInFlight_ = false;
    bool advanceScheduled_ = false;
    bool pauseRequested_ = false;
    bool editInterruptRequested_ = false;
    bool handlingDebuggerOutput_ = false;
    bool outputProcessing_ = false;
    bool atTickBoundary_ = false;
    bool workerReady_ = false;
    bool stopping_ = false;
    bool pendingStart_ = false;
    bool darkMode_ = false;
    quint64 sourceLoadGeneration_ = 0;
    quint64 sessionGeneration_ = 0;
    quint64 shutdownGeneration_ = 0;
    quint64 sourceRevision_ = 0;
    quint64 inlineCacheGeneration_ = 0;
    quint64 debugOutputSequence_ = 0;
    quint64 nextInsertedLineId_ = quint64{1} << 32;
    QList<quint64> pendingBoundaryEditIds_;
    int pendingBoundaryEditIndex_ = 0;
    bool boundaryEditInProgress_ = false;
    bool pendingBoundarySkipsOriginal_ = false;
    StepMode stepMode_ = StepMode::None;
};

} // namespace forevertas::viewer

#endif
