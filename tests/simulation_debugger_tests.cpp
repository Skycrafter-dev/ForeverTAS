#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"
#include "viewer/simulation_debugger_model.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QHash>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

constexpr auto kVehicleSource =
        "src/simulation/runtime/replay_vehicle_simulation.cpp";
constexpr auto kStableInspectionSource =
        "src/engine/physics/world/physics_step.cpp";
constexpr auto kRuntimeSource =
        "src/simulation/runtime/replay_simulation_runtime.cpp";
constexpr int kApplyControlsLine = 31;
constexpr int kRelocatedBreakpointRequestLine = 34;
constexpr int kRelocatedBreakpointLine = 37;
constexpr int kPreparedBodyLine = 200;
constexpr int kRuntimeIncludeLine = 7;

bool Check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

QString SourceLine(forevertas::viewer::SimulationDebuggerModel &model,
                   const QString &path, int line) {
    if (!model.selectFile(path)) {
        return {};
    }
    const QVariantList lines = model.lines();
    return line > 0 && line <= lines.size()
                   ? lines[line - 1]
                             .toMap()
                             .value(QStringLiteral("text"))
                             .toString()
                   : QString();
}

double VectorComponent(const QVariantMap &frame, const QString &name,
                       int component) {
    const QVariantList values = frame.value(name).toList();
    return component >= 0 && component < values.size()
                   ? values[component].toDouble()
                   : 0.0;
}

double HorizontalSpeed(const QVariantMap &frame) {
    return std::hypot(
            VectorComponent(frame, QStringLiteral("linearSpeed"), 0),
            VectorComponent(frame, QStringLiteral("linearSpeed"), 2));
}

bool HasRealEngineTree(forevertas::viewer::SimulationDebuggerModel &model) {
    const bool physics = model.selectFile(
            QStringLiteral("src/engine/physics/world/physics_step.cpp"));
    const bool runtime = model.selectFile(QStringLiteral(
            "src/simulation/runtime/replay_simulation_runtime.cpp"));
    bool synthetic = false;
    for (const QVariant &value : model.fileEntries()) {
        const QString path =
                value.toMap().value(QStringLiteral("path")).toString();
        synthetic |=
                path.contains(QStringLiteral("adapter"), Qt::CaseInsensitive);
    }
    return physics && runtime && !synthetic;
}

bool HasModifiedEntry(forevertas::viewer::SimulationDebuggerModel &model,
                      const QString &path) {
    for (const QVariant &entry : model.fileEntries()) {
        const QVariantMap data = entry.toMap();
        if (data.value(QStringLiteral("path")).toString() == path) {
            return data.value(QStringLiteral("modified")).toBool();
        }
    }
    return false;
}

QVariantMap FileEntry(forevertas::viewer::SimulationDebuggerModel &model,
                      const QString &path) {
    for (const QVariant &entry : model.fileEntries()) {
        const QVariantMap data = entry.toMap();
        if (data.value(QStringLiteral("path")).toString() == path) {
            return data;
        }
    }
    return {};
}

bool ActiveLineIsSelectedAndMarked(
        forevertas::viewer::SimulationDebuggerModel &model) {
    if (model.activeLine() <= 0 ||
        model.selectedFilePath() != model.activeFilePath()) {
        return false;
    }
    const QVariantList lines = model.lines();
    return model.activeLine() <= static_cast<int>(lines.size()) &&
           lines[model.activeLine() - 1]
                   .toMap()
                   .value(QStringLiteral("active"))
                   .toBool();
}

QString InlineValue(forevertas::viewer::SimulationDebuggerModel &model,
                    const QString &path, int line) {
    if (!model.selectFile(path)) {
        return {};
    }
    const QVariantList lines = model.lines();
    return line > 0 && line <= lines.size()
                   ? lines[line - 1]
                             .toMap()
                             .value(QStringLiteral("inlineValue"))
                             .toString()
                   : QString();
}

bool HasInlineValue(forevertas::viewer::SimulationDebuggerModel &model) {
    const QVariantList lines = model.lines();
    return std::any_of(lines.cbegin(), lines.cend(), [](const QVariant &line) {
        return !line.toMap()
                        .value(QStringLiteral("inlineValue"))
                        .toString()
                        .isEmpty();
    });
}

bool WaitForPreparation(forevertas::viewer::SimulationDebuggerModel &model,
                        int *pulseCount = nullptr) {
    QEventLoop loop;
    QTimer pulse;
    QTimer timeout;
    int pulses = 0;
    pulse.setInterval(1);
    QObject::connect(&pulse, &QTimer::timeout, &loop, [&]() { ++pulses; });
    QObject::connect(&model,
                     &forevertas::viewer::SimulationDebuggerModel::stateChanged,
                     &loop, [&]() {
                         if (!model.preparing()) {
                             loop.quit();
                         }
                     });
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    pulse.start();
    timeout.start(10000);
    if (model.preparing()) {
        loop.exec();
    }
    if (pulseCount != nullptr) {
        *pulseCount = pulses;
    }
    return !model.preparing() && model.available();
}

void SendTimelineMouseEvent(
        forevertas::viewer::RaceTimelineItem &timeline,
        QEvent::Type type,
        Qt::MouseButton button,
        Qt::MouseButtons buttons,
        const QPointF &position) {
    QMouseEvent event(type,
                      position,
                      position,
                      button,
                      buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&timeline, &event);
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: forevertas-simulation-debugger-tests <Packs> "
                     "<replay>\n";
        return 2;
    }

    QStandardPaths::setTestModeEnabled(true);
    QGuiApplication application(argc, argv);
    QSettings().clear();
    forevertas::viewer::RaceViewerController viewer;
    auto *const model = viewer.simulationDebugger();
    forevertas::viewer::RaceTimelineItem timeline;
    timeline.setWidth(252);
    timeline.setHeight(600);
    timeline.setViewer(&viewer);
    bool okay = true;
    okay &= Check(
            model->preparing(),
            "reference source preparation completed synchronously on the UI "
            "thread");
    int preparationPulses = 0;
    okay &= Check(WaitForPreparation(*model, &preparationPulses) &&
                          preparationPulses > 0,
                  "reference source preparation blocked the UI event loop");
    okay &= Check(model->toggleBreakpoint(QString::fromLatin1(kVehicleSource),
                                          kApplyControlsLine),
                  "persistent breakpoint setup failed");
    {
        forevertas::viewer::SimulationDebuggerModel restored;
        int restoredPreparationPulses = 0;
        okay &= Check(
                restored.preparing() &&
                        WaitForPreparation(restored,
                                           &restoredPreparationPulses) &&
                        restoredPreparationPulses > 0,
                "reconstructed source model did not prepare asynchronously");
        const QVariantMap restoredEntry =
                FileEntry(restored, QString::fromLatin1(kVehicleSource));
        okay &= Check(
                restored.selectFile(QString::fromLatin1(kVehicleSource)) &&
                        restored.lines()
                                .at(kApplyControlsLine - 1)
                                .toMap()
                                .value(QStringLiteral("breakpoint"))
                                .toBool() &&
                        restoredEntry.value(QStringLiteral("breakpoint"))
                                .toBool(),
                "line and file breakpoint state did not survive model "
                "reconstruction");
    }
    okay &= Check(model->toggleBreakpoint(QString::fromLatin1(kVehicleSource),
                                          kApplyControlsLine),
                  "persistent breakpoint cleanup failed");
    okay &= Check(!viewer.startSimulationDebugger(),
                  "debugger started without a loaded replay");

    enum class Phase {
        Loading,
        WaitingInitialFrame,
        WaitingNonExecutableBreakpoint,
        WaitingResolvedBreakpointInstall,
        WaitingApplyControls,
        WaitingEditedSourceStep,
        WaitingSubstep,
        WaitingSourceLineStep,
        WaitingTickStep,
        WaitingPastLine,
        WaitingDeferredTick,
        WaitingRestoredTick,
        WaitingInvalidLine,
        WaitingRepeatedBreakpoint,
        WaitingPrintOutput,
        WaitingInsertedOutput,
        WaitingDeletedTick,
        WaitingPostDeletionBreakpoint,
        WaitingCompileFailure,
        WaitingPauseStop,
        WaitingTimelineSeekStability,
        Finished,
    };
    Phase phase = Phase::Loading;
    QString originalApplyLine;
    QHash<qint64, QVariantMap> frames;
    qint64 frameCountBeforeInvalid = 0;
    qint64 firstBreakpointTick = -1;
    qint64 stepTick = -1;
    qint64 insertedEditTick = -1;
    qint64 framesBeforeSteps = -1;
    qint64 framesBeforeDeletion = -1;
    qint64 deletionTick = -1;
    QString sourceStepFile;
    int sourceStepLine = -1;
    QString heldInspectionFile;
    int requestedOutputLine = -1;
    QList<int> insertedActiveLines;
    int selectionChangesWhileRunning = 0;
    int resolvedRuntimeBreakpointLine = -1;
    qint64 timelineSeekTick = -1;
    qint64 timelineSeekFrameCount = -1;
    qint64 timelineSeekExpectedTime = -1;
    QString timelineSeekFile;
    int timelineSeekLine = -1;
    int timelineSeekLineChanges = 0;
    int timelineSeekStateChanges = 0;
    int timelineSeekTimeChanges = 0;
    int timelineSeekPoseChanges = 0;
    bool countingTimelineSeekSignals = false;
    bool loadingReplaySeen = false;
    QStringList startupStatuses;
    QElapsedTimer timelineSeekClock;
    QElapsedTimer resolvedBreakpointClock;
    QElapsedTimer responsivenessClock;
    qint64 lastUiPulse = 0;
    qint64 maximumUiGap = 0;
    int maximumUiGapPhase = -1;
    QString maximumUiGapStatus;
    int uiPulseCount = 0;
    QTimer uiPulse;
    uiPulse.setInterval(10);
    QObject::connect(&uiPulse, &QTimer::timeout, &application, [&]() {
        const qint64 now = responsivenessClock.elapsed();
        const qint64 gap = now - lastUiPulse;
        if (gap > maximumUiGap) {
            maximumUiGap = gap;
            maximumUiGapPhase = static_cast<int>(phase);
            maximumUiGapStatus = model->statusText();
        }
        lastUiPulse = now;
        ++uiPulseCount;
    });

    QObject::connect(
            model,
            &forevertas::viewer::SimulationDebuggerModel::selectionChanged,
            &application, [&]() {
                if (model->running()) {
                    ++selectionChangesWhileRunning;
                }
            });
    QObject::connect(
            model,
            &forevertas::viewer::SimulationDebuggerModel::linesChanged,
            &application,
            [&]() {
                if (countingTimelineSeekSignals) {
                    ++timelineSeekLineChanges;
                }
            });
    QObject::connect(
            model,
            &forevertas::viewer::SimulationDebuggerModel::stateChanged,
            &application,
            [&]() {
                if (phase == Phase::WaitingInitialFrame &&
                    (startupStatuses.isEmpty() ||
                     startupStatuses.back() != model->statusText())) {
                    startupStatuses.push_back(model->statusText());
                }
                if (model->loadingReplay()) {
                    loadingReplaySeen =
                            model->statusText() ==
                            QStringLiteral(
                                    "Loading scenario in the Reference engine...");
                }
                if (countingTimelineSeekSignals) {
                    ++timelineSeekStateChanges;
                }
            });
    QObject::connect(
            &viewer,
            &forevertas::viewer::RaceViewerController::timeChanged,
            &application,
            [&]() {
                if (countingTimelineSeekSignals) {
                    ++timelineSeekTimeChanges;
                }
            });
    QObject::connect(
            &viewer,
            &forevertas::viewer::RaceViewerController::poseChanged,
            &application,
            [&]() {
                if (countingTimelineSeekSignals) {
                    ++timelineSeekPoseChanges;
                }
            });
    QObject::connect(
            model,
            &forevertas::viewer::SimulationDebuggerModel::debugOutputChanged,
            &application, [&]() {
                if (phase == Phase::WaitingInsertedOutput &&
                    !model->debugOutput().isEmpty()) {
                    insertedActiveLines.push_back(model->activeLine());
                }
            });
    QObject::connect(
            model,
            &forevertas::viewer::SimulationDebuggerModel::
                    sourceLocationRequested,
            &application,
            [&](int line) { requestedOutputLine = line; });

    QObject::connect(
            model, &forevertas::viewer::SimulationDebuggerModel::frameProduced,
            &application, [&](const QVariantMap &frame) {
                const qint64 tick =
                        frame.value(QStringLiteral("tick")).toLongLong();
                frames.insert(tick, frame);
                if (phase == Phase::WaitingInitialFrame && tick == 0) {
                    if (!loadingReplaySeen) {
                        std::cerr << "startup statuses: "
                                  << startupStatuses.join(
                                             QStringLiteral(" | "))
                                             .toStdString()
                                  << '\n';
                    }
                    okay &= Check(
                            loadingReplaySeen && model->active() &&
                            viewer.selectedRunId() == QStringLiteral("debug"),
                            "native debugger did not initialize its viewer "
                            "run or expose the Reference loading state");
                    okay &= Check(
                            viewer.durationMs() ==
                                    frame.value(QStringLiteral("horizonMs"))
                                            .toLongLong(),
                            "live viewer did not retain the Simulation horizon");
                    okay &= Check(
                            model->toggleBreakpoint(
                                    QString::fromLatin1(kRuntimeSource),
                                    kRuntimeIncludeLine),
                            "non-executable runtime breakpoint was rejected");
                    phase = Phase::WaitingNonExecutableBreakpoint;
                } else if (phase == Phase::WaitingTickStep && tick == 1) {
                    const double forward = HorizontalSpeed(frame);
                    okay &= Check(
                            std::abs(forward) < 0.02,
                            "edited real ApplyControls statement did not alter "
                            "reference physics");
                } else if (phase == Phase::WaitingDeferredTick && tick == 2) {
                    const double forward = HorizontalSpeed(frame);
                    okay &= Check(
                            std::abs(forward) < 0.03,
                            "restoring an already executed line changed the "
                            "current tick");
                    phase = Phase::WaitingRestoredTick;
                } else if (phase == Phase::WaitingRestoredTick && tick == 3) {
                    const double previous = HorizontalSpeed(frames.value(2));
                    const double restored = HorizontalSpeed(frame);
                    if (!(restored > previous + 0.05)) {
                        std::cerr << "restoration speeds: previous="
                                  << previous << ", restored=" << restored
                                  << ", executionTick="
                                  << model->executionTick() << '\n';
                    }
                    okay &= Check(
                            restored > previous + 0.05,
                            "past-line restoration did not take effect on the "
                            "next tick");
                    okay &= Check(HorizontalSpeed(frames.value(1)) < 0.02,
                                  "a later source edit changed a past "
                                  "simulated tick");
                    okay &= Check(model->toggleBreakpoint(
                                          QString::fromLatin1(kVehicleSource),
                                          kApplyControlsLine),
                                  "invalid-edit breakpoint was rejected");
                    phase = Phase::WaitingInvalidLine;
                }
            });

    QTimer poll;
    poll.setInterval(2);
    QObject::connect(&poll, &QTimer::timeout, &application, [&]() {
        if (phase == Phase::WaitingNonExecutableBreakpoint &&
            !model->running() &&
            model->statusText().startsWith(
                    QStringLiteral("Breakpoint moved to executable line")) &&
            model->statusText().contains(
                    QStringLiteral("replay_simulation_runtime.cpp"))) {
            okay &= Check(
                    !model->loadingReplay(),
                    "Reference loading state remained active after the native "
                    "engine reached its first source boundary");
            okay &= Check(
                    model->selectFile(QString::fromLatin1(kRuntimeSource)),
                    "resolved runtime breakpoint source could not be selected");
            const QVariantList runtimeLines = model->lines();
            int resolvedDisplayLine = -1;
            for (int index = 0; index < runtimeLines.size(); ++index) {
                if (runtimeLines[index]
                            .toMap()
                            .value(QStringLiteral("breakpoint"))
                            .toBool()) {
                    resolvedDisplayLine = index + 1;
                    break;
                }
            }
            const bool includeStillMarked =
                    runtimeLines[kRuntimeIncludeLine - 1]
                            .toMap()
                            .value(QStringLiteral("breakpoint"))
                            .toBool();
            const bool resolvedBreakpointMoved =
                    resolvedDisplayLine > 0 &&
                    resolvedDisplayLine != kRuntimeIncludeLine &&
                    !includeStillMarked;
            if (resolvedDisplayLine <= 0 ||
                resolvedDisplayLine == kRuntimeIncludeLine ||
                includeStillMarked) {
                std::cerr << "nearest breakpoint details: resolvedDisplayLine="
                          << resolvedDisplayLine
                          << ", includeStillMarked=" << includeStillMarked
                          << ", status="
                          << model->statusText().toStdString() << '\n';
            }
            okay &= Check(
                    resolvedBreakpointMoved,
                    "non-executable breakpoint did not move to the nearest "
                    "native source location");
            resolvedRuntimeBreakpointLine = resolvedDisplayLine;
            resolvedBreakpointClock.start();
            phase = Phase::WaitingResolvedBreakpointInstall;
        } else if (phase == Phase::WaitingResolvedBreakpointInstall &&
                   resolvedBreakpointClock.elapsed() >= 200) {
            okay &= Check(
                    model->selectFile(QString::fromLatin1(kRuntimeSource)) &&
                            resolvedRuntimeBreakpointLine > 0 &&
                            model->lines()[resolvedRuntimeBreakpointLine - 1]
                                    .toMap()
                                    .value(QStringLiteral("breakpoint"))
                                    .toBool() &&
                            model->editError().isEmpty() &&
                            model->toggleBreakpoint(
                                    QString::fromLatin1(kRuntimeSource),
                                    resolvedRuntimeBreakpointLine),
                    "resolved native breakpoint did not remain installed and "
                    "toggle cleanly");
            okay &= Check(
                    model->selectFile(
                            QString::fromLatin1(kStableInspectionSource)),
                    "could not select a stable inspection file before "
                    "continuous playback");
            heldInspectionFile = model->selectedFilePath();
            phase = Phase::WaitingApplyControls;
            viewer.play();
        } else if (phase == Phase::WaitingApplyControls && !model->running() &&
            model->activeFilePath() == QString::fromLatin1(kVehicleSource) &&
            model->activeLine() == kApplyControlsLine) {
            okay &= Check(
                    !heldInspectionFile.isEmpty() &&
                            heldInspectionFile != model->activeFilePath() &&
                            selectionChangesWhileRunning == 0 &&
                            ActiveLineIsSelectedAndMarked(*model),
                    "continuous playback moved the inspected source file or "
                    "failed to restore the exact breakpoint location");
            okay &= Check(
                    !model->lines().at(kRelocatedBreakpointRequestLine - 1)
                                    .toMap()
                                    .value(QStringLiteral("breakpoint"))
                                    .toBool() &&
                            model->lines()
                                    .at(kRelocatedBreakpointLine - 1)
                                    .toMap()
                                    .value(QStringLiteral("breakpoint"))
                                    .toBool() &&
                            model->toggleBreakpoint(
                                    QString::fromLatin1(kVehicleSource),
                                    kRelocatedBreakpointLine),
                    "non-executable breakpoint was not moved to the resolved "
                    "native source line");
            stepTick = model->executionTick();
            framesBeforeSteps = viewer.tickCount();
            okay &= Check(
                    model->canStepSource() && model->canStepTick() &&
                            model->stepSourceLine(),
                    "source-line step could not execute the pending edited "
                    "line");
            phase = Phase::WaitingEditedSourceStep;
        } else if (phase == Phase::WaitingEditedSourceStep &&
                   !model->stepping() && model->activeLine() > 0) {
            const bool editedStepSettled =
                    model->executionTick() == stepTick &&
                    viewer.tickCount() == framesBeforeSteps &&
                    ActiveLineIsSelectedAndMarked(*model) &&
                    model->statusText().startsWith(
                            QStringLiteral("Source-line step completed"));
            if (!editedStepSettled) {
                std::cerr << "edited-step details: expectedTick=" << stepTick
                          << ", actualTick=" << model->executionTick()
                          << ", expectedFrames=" << framesBeforeSteps
                          << ", actualFrames=" << viewer.tickCount()
                          << ", active=" << model->activeLine() << ", selected="
                          << model->selectedFilePath().toStdString()
                          << ", activeFile="
                          << model->activeFilePath().toStdString()
                          << ", status=" << model->statusText().toStdString()
                          << '\n';
            }
            okay &= Check(
                    editedStepSettled,
                    "edited source-line step advanced a physics tick or did "
                    "not settle at the next source location");
            okay &= Check(
                    model->toggleBreakpoint(QString::fromLatin1(kVehicleSource),
                                            kApplyControlsLine),
                    "initial execution breakpoint could not be removed");
            okay &= Check(model->canStepSource() && model->stepSubstep(),
                          "native substep could not start");
            phase = Phase::WaitingSubstep;
        } else if (phase == Phase::WaitingSubstep && !model->stepping() &&
                   model->activeLine() > 0) {
            okay &= Check(
                    model->executionTick() == stepTick &&
                            viewer.tickCount() == framesBeforeSteps &&
                            ActiveLineIsSelectedAndMarked(*model) &&
                            model->statusText().startsWith(
                                    QStringLiteral("Substep completed")),
                    "native substep advanced a physics tick or lost its source "
                    "location");
            sourceStepFile = model->activeFilePath();
            sourceStepLine = model->activeLine();
            okay &= Check(model->canStepSource() && model->stepSourceLine(),
                          "native source-line step could not start");
            phase = Phase::WaitingSourceLineStep;
        } else if (phase == Phase::WaitingSourceLineStep &&
                   !model->stepping() && model->activeLine() > 0) {
            okay &= Check(
                    model->executionTick() == stepTick &&
                            viewer.tickCount() == framesBeforeSteps &&
                            ActiveLineIsSelectedAndMarked(*model) &&
                            (model->activeFilePath() != sourceStepFile ||
                             model->activeLine() != sourceStepLine),
                    "source-line step did not advance to a new source line");
            okay &= Check(model->canStepTick() && model->stepTick(),
                          "one-tick step could not start");
            phase = Phase::WaitingTickStep;
        } else if (phase == Phase::WaitingTickStep && !model->stepping() &&
                   model->executionTick() == stepTick + 1) {
            okay &= Check(viewer.tickCount() == framesBeforeSteps + 1 &&
                                  frames.contains(stepTick + 1) &&
                                  model->statusText() ==
                                          QStringLiteral("Advanced exactly "
                                                         "one physics tick."),
                          "one-tick step did not produce exactly one new tick");
            okay &= Check(
                    model->toggleBreakpoint(QString::fromLatin1(kVehicleSource),
                                            kPreparedBodyLine),
                    "future real-source breakpoint was rejected");
            phase = Phase::WaitingPastLine;
            viewer.play();
        } else if (phase == Phase::WaitingPastLine && !model->running() &&
                   model->activeFilePath() ==
                           QString::fromLatin1(kVehicleSource) &&
                   model->activeLine() == kPreparedBodyLine &&
                   !InlineValue(*model, QString::fromLatin1(kVehicleSource),
                                kPreparedBodyLine)
                            .isEmpty()) {
            okay &= Check(
                    !InlineValue(*model, QString::fromLatin1(kVehicleSource),
                                 kPreparedBodyLine)
                             .isEmpty(),
                    "in-scope variable value was not shown beside code");
            okay &= Check(
                    model->updateLine(kApplyControlsLine, originalApplyLine) &&
                            !model->hasEdits(),
                    "restoring a past line failed");
            model->toggleBreakpoint(QString::fromLatin1(kVehicleSource),
                                    kPreparedBodyLine);
            phase = Phase::WaitingDeferredTick;
            viewer.play();
        } else if (phase == Phase::WaitingInvalidLine && !model->running() &&
                   model->activeFilePath() ==
                           QString::fromLatin1(kVehicleSource) &&
                   model->activeLine() == kApplyControlsLine) {
            firstBreakpointTick = model->executionTick();
            phase = Phase::WaitingRepeatedBreakpoint;
            viewer.play();
        } else if (phase == Phase::WaitingRepeatedBreakpoint &&
                   !model->running() &&
                   model->activeFilePath() ==
                           QString::fromLatin1(kVehicleSource) &&
                   model->activeLine() == kApplyControlsLine &&
                   model->executionTick() > firstBreakpointTick) {
            okay &= Check(model->executionTick() > firstBreakpointTick,
                          "breakpoint did not stop again on the next tick");
            okay &= Check(
                    model->selectFile(QString::fromLatin1(kVehicleSource)) &&
                            model->updateLine(
                                    kApplyControlsLine,
                                    QStringLiteral(
                                            "printf(\"FB16 first=%d "
                                            "@FOREVERTAS_DEBUG_PRINT_BEGIN@ "
                                            "@FOREVERTAS_DEBUG_PRINT_END@\\n"
                                            "FB16 second=%d\\n\", 73, 74);")),
                    "typed native print statement could not be staged");
            phase = Phase::WaitingPrintOutput;
            viewer.play();
        } else if (phase == Phase::WaitingPrintOutput && !model->running() &&
                   model->activeFilePath() ==
                           QString::fromLatin1(kVehicleSource) &&
                   model->activeLine() == kApplyControlsLine &&
                   model->debugOutput().size() >= 2) {
            const QVariantMap first = model->debugOutput().at(0).toMap();
            const QVariantMap second = model->debugOutput().at(1).toMap();
            okay &= Check(
                    model->debugOutput().size() == 2 &&
                            first.value(QStringLiteral("message")).toString() ==
                                    QStringLiteral(
                                            "FB16 first=73 "
                                            "@FOREVERTAS_DEBUG_PRINT_BEGIN@ "
                                            "@FOREVERTAS_DEBUG_PRINT_END@") &&
                            second.value(QStringLiteral("message")).toString() ==
                                    QStringLiteral("FB16 second=74") &&
                            first.value(QStringLiteral("sequence"))
                                            .toULongLong() <
                                    second.value(QStringLiteral("sequence"))
                                            .toULongLong() &&
                            first.value(QStringLiteral("tick")).toLongLong() ==
                                    model->executionTick() - 1 &&
                            first.value(QStringLiteral("context")).toString() ==
                                    QStringLiteral("playback") &&
                            first.value(QStringLiteral("path")).toString() ==
                                    QString::fromLatin1(kVehicleSource) &&
                            first.value(QStringLiteral("line")).toInt() ==
                                    kApplyControlsLine &&
                            first.value(QStringLiteral("location")).toString() ==
                                    QStringLiteral(
                                            "replay_vehicle_simulation.cpp:31"),
                    "printed output was not ordered or correctly contextualized");
            requestedOutputLine = -1;
            okay &= Check(
                    !model->openDebugOutput(-1) &&
                            !model->openDebugOutput(model->debugOutput().size()) &&
                            model->openDebugOutput(0) &&
                            model->selectedFilePath() ==
                                    QString::fromLatin1(kVehicleSource) &&
                            requestedOutputLine == kApplyControlsLine,
                    "printed-output source link did not open the correct code "
                    "location");
            model->clearDebugOutput();
            okay &= Check(model->debugOutput().isEmpty(),
                          "printed debug output did not clear");
            okay &= Check(
                    model->updateLine(kApplyControlsLine, originalApplyLine),
                    "original statement could not be restored before inserted "
                    "line execution");
            insertedEditTick = model->executionTick();
            const int firstInserted =
                    model->insertLineAfter(kPreparedBodyLine - 1);
            const int secondInserted =
                    model->insertLineAfter(firstInserted);
            okay &= Check(
                    firstInserted == kPreparedBodyLine &&
                            secondInserted == kPreparedBodyLine + 1 &&
                            model->updateLine(
                                    firstInserted,
                                    QStringLiteral(
                                            "printf(\"FB17 inserted "
                                            "first=%d\\n\", 117);")) &&
                            model->updateLine(
                                    secondInserted,
                                    QStringLiteral(
                                            "printf(\"FB17 inserted "
                                            "second=%d\\n\", 118);")) &&
                            model->activeLine() == kApplyControlsLine &&
                            model->lines()
                                    .at(kApplyControlsLine - 1)
                                    .toMap()
                                    .value(QStringLiteral("breakpoint"))
                                    .toBool(),
                    "future inserted runtime lines did not preserve active-line "
                    "and breakpoint identity");
            phase = Phase::WaitingInsertedOutput;
            viewer.play();
        } else if (phase == Phase::WaitingInsertedOutput &&
                   !model->running() &&
                   model->activeFilePath() ==
                           QString::fromLatin1(kVehicleSource) &&
                   model->activeLine() == kApplyControlsLine &&
                   model->debugOutput().size() >= 2) {
            const QVariantMap first = model->debugOutput().at(0).toMap();
            const QVariantMap second = model->debugOutput().at(1).toMap();
            okay &= Check(
                    model->debugOutput().size() == 2 &&
                            first.value(QStringLiteral("message")).toString() ==
                                    QStringLiteral(
                                            "FB17 inserted first=117") &&
                            second.value(QStringLiteral("message")).toString() ==
                                    QStringLiteral(
                                            "FB17 inserted second=118") &&
                            first.value(QStringLiteral("line")).toInt() ==
                                    kPreparedBodyLine &&
                            second.value(QStringLiteral("line")).toInt() ==
                                    kPreparedBodyLine + 1 &&
                            first.value(QStringLiteral("tick")).toLongLong() ==
                                    insertedEditTick &&
                            insertedActiveLines ==
                                    QList<int>{kPreparedBodyLine,
                                               kPreparedBodyLine + 1} &&
                            first.value(QStringLiteral("sequence"))
                                            .toULongLong() <
                                    second.value(QStringLiteral("sequence"))
                                            .toULongLong(),
                    "future inserted source lines did not execute immediately "
                    "in display order with matching active-line tracking during "
                    "the current tick");
            okay &= Check(
                    model->deleteLine(kPreparedBodyLine) &&
                            model->debugOutput()
                                            .at(1)
                                            .toMap()
                                            .value(QStringLiteral("line"))
                                            .toInt() == kPreparedBodyLine &&
                            model->activeLine() == kApplyControlsLine,
                    "line deletion did not remap later output and active "
                    "locations");
            requestedOutputLine = -1;
            okay &= Check(
                    model->openDebugOutput(1) &&
                            requestedOutputLine == kPreparedBodyLine,
                    "shifted inserted-output link did not follow its stable "
                    "source line");
            okay &= Check(
                    model->deleteLine(kPreparedBodyLine) &&
                            model->activeLine() == kApplyControlsLine &&
                            model->lines()
                                    .at(kApplyControlsLine - 1)
                                    .toMap()
                                    .value(QStringLiteral("breakpoint"))
                                    .toBool(),
                    "removing inserted lines did not restore original line "
                    "numbering and breakpoint placement");
            model->clearDebugOutput();
            deletionTick = model->executionTick();
            framesBeforeDeletion = viewer.tickCount();
            okay &= Check(
                    model->deleteLine(kApplyControlsLine) &&
                            model->hasEdits() && model->canStepTick() &&
                            model->stepTick(),
                    "original source-line deletion could not be executed by "
                    "the native engine");
            phase = Phase::WaitingDeletedTick;
        } else if (phase == Phase::WaitingDeletedTick &&
                   !model->stepping() &&
                   model->executionTick() == deletionTick + 1) {
            okay &= Check(
                    viewer.tickCount() == framesBeforeDeletion + 1 &&
                            model->editError().isEmpty(),
                    "deleting an original statement did not complete exactly "
                    "one real physics tick");
            model->resetEdits();
            okay &= Check(
                    !model->hasEdits() &&
                            SourceLine(
                                    *model,
                                    QString::fromLatin1(kVehicleSource),
                                    kApplyControlsLine) == originalApplyLine &&
                            model->toggleBreakpoint(
                                    QString::fromLatin1(kVehicleSource),
                                    kApplyControlsLine),
                    "reset after executed deletion did not restore source and "
                    "breakpoint mapping");
            phase = Phase::WaitingPostDeletionBreakpoint;
            viewer.play();
        } else if (phase == Phase::WaitingPostDeletionBreakpoint &&
                   !model->running() &&
                   model->activeFilePath() ==
                           QString::fromLatin1(kVehicleSource) &&
                   model->activeLine() == kApplyControlsLine) {
            okay &= Check(
                    model->toggleBreakpoint(
                            QString::fromLatin1(kVehicleSource),
                            kApplyControlsLine) &&
                            model->updateLine(
                                    kApplyControlsLine,
                                    QStringLiteral("this is not valid C++;")),
                    "invalid native edit could not be staged after insertion "
                    "and deletion");
            frameCountBeforeInvalid = viewer.tickCount();
            phase = Phase::WaitingCompileFailure;
            viewer.play();
        } else if (phase == Phase::WaitingCompileFailure && !model->running() &&
                   !model->editError().isEmpty()) {
            okay &= Check(viewer.tickCount() == frameCountBeforeInvalid,
                          "invalid native C++ advanced the simulation");
            model->resetEdits();
            okay &= Check(!model->hasEdits() && model->editError().isEmpty(),
                          "in-memory source edit reset failed");
            phase = Phase::WaitingPauseStop;
            viewer.play();
            QTimer::singleShot(2, &application,
                               [&viewer]() { viewer.pause(); });
        } else if (phase == Phase::WaitingPauseStop && !model->running() &&
                   model->activeLine() > 0 && HasInlineValue(*model) &&
                   model->statusText().startsWith(QStringLiteral("Paused"))) {
            okay &= Check(!model->activeFilePath().isEmpty() &&
                                  ActiveLineIsSelectedAndMarked(*model) &&
                                  selectionChangesWhileRunning == 0,
                          "normal pause did not settle at a real source line");
            timelineSeekTick = model->executionTick();
            timelineSeekFrameCount = viewer.tickCount();
            timelineSeekFile = model->activeFilePath();
            timelineSeekLine = model->activeLine();
            const qint64 scrubStartTime = viewer.timeMs();
            timelineSeekLineChanges = 0;
            timelineSeekStateChanges = 0;
            timelineSeekTimeChanges = 0;
            timelineSeekPoseChanges = 0;
            countingTimelineSeekSignals = true;
            QElapsedTimer synchronousSeekClock;
            synchronousSeekClock.start();
            for (int repeat = 0; repeat < 2000; ++repeat) {
                viewer.pause();
            }
            SendTimelineMouseEvent(
                    timeline,
                    QEvent::MouseButtonPress,
                    Qt::LeftButton,
                    Qt::LeftButton,
                    QPointF(126.0, 300.0));
            constexpr int kMoveCount = 4701;
            for (int move = 0; move < kMoveCount; ++move) {
                SendTimelineMouseEvent(
                        timeline,
                        QEvent::MouseMove,
                        Qt::NoButton,
                        Qt::LeftButton,
                        QPointF(126.0,
                                static_cast<qreal>(move % 600)));
            }
            const qreal finalY =
                    static_cast<qreal>((kMoveCount - 1) % 600);
            SendTimelineMouseEvent(
                    timeline,
                    QEvent::MouseButtonRelease,
                    Qt::LeftButton,
                    Qt::NoButton,
                    QPointF(126.0, finalY));
            timelineSeekExpectedTime = std::clamp<qint64>(
                    static_cast<qint64>(std::llround(
                            static_cast<double>(scrubStartTime) -
                            (finalY - 300.0) / timeline.pixelsPerTick() *
                                    viewer.tickDurationMs())),
                    0,
                    viewer.timelineSeekLimitMs());
            okay &= Check(
                    synchronousSeekClock.elapsed() < 250 &&
                            timelineSeekTimeChanges <= 2 &&
                            timelineSeekPoseChanges <= 2 &&
                            viewer.timeMs() == timelineSeekExpectedTime,
                    "continuous code-mode scrubbing was not coalesced or "
                    "lost its exact release position");
            timelineSeekClock.start();
            phase = Phase::WaitingTimelineSeekStability;
        } else if (phase == Phase::WaitingTimelineSeekStability &&
                   timelineSeekClock.elapsed() >= 250) {
            countingTimelineSeekSignals = false;
            okay &= Check(
                    model->active() && !model->running() &&
                            !model->preparing() &&
                            model->executionTick() == timelineSeekTick &&
                            viewer.tickCount() == timelineSeekFrameCount &&
                            model->activeFilePath() == timelineSeekFile &&
                            model->activeLine() == timelineSeekLine &&
                            HasInlineValue(*model) &&
                            timelineSeekLineChanges == 0 &&
                            timelineSeekStateChanges == 0 &&
                            viewer.timeMs() == timelineSeekExpectedTime,
                    "timeline seeking restarted or reprocessed the native "
                    "debugger instead of reusing cached simulation frames");
            QElapsedTimer stopTimer;
            stopTimer.start();
            viewer.stopSimulationDebugger();
            okay &= Check(stopTimer.elapsed() < 100,
                          "stopping the debugger blocked the UI thread");
            const qint64 finalGap = responsivenessClock.elapsed() - lastUiPulse;
            if (finalGap > maximumUiGap) {
                maximumUiGap = finalGap;
                maximumUiGapPhase = static_cast<int>(phase);
                maximumUiGapStatus = model->statusText();
            }
            if (uiPulseCount <= 50 || maximumUiGap >= 500) {
                std::cerr << "responsiveness details: pulses=" << uiPulseCount
                          << ", maxGapMs=" << maximumUiGap
                          << ", phase=" << maximumUiGapPhase
                          << ", status=" << maximumUiGapStatus.toStdString()
                          << '\n';
            }
            okay &= Check(
                    uiPulseCount > 50 && maximumUiGap < 500,
                    "debugger preparation, execution, errors, or timeline "
                    "updates stalled the UI event loop");
            phase = Phase::Finished;
            poll.stop();
            application.exit(okay ? 0 : 1);
        }
    });

    QObject::connect(
            &viewer, &forevertas::viewer::RaceViewerController::stateChanged,
            &application, [&]() {
                if (phase != Phase::Loading || viewer.loading() ||
                    !viewer.loaded()) {
                    return;
                }
                okay &= Check(model->available() && HasRealEngineTree(*model),
                              "source tree is not the real adaptive reference "
                              "engine");
                originalApplyLine =
                        SourceLine(*model, QString::fromLatin1(kVehicleSource),
                                   kApplyControlsLine);
                okay &= Check(
                        !model->updateLine(
                                kApplyControlsLine,
                                originalApplyLine +
                                        QStringLiteral("\ninvalid")) &&
                                SourceLine(*model,
                                           QString::fromLatin1(kVehicleSource),
                                           kApplyControlsLine) ==
                                        originalApplyLine,
                        "multi-line input escaped the source-line edit "
                        "boundary");
                const QVariantMap functionLine = model->lines().at(28).toMap();
                const QString lightHighlight =
                        functionLine.value(QStringLiteral("highlighted"))
                                .toString();
                model->setDarkMode(true);
                const QString darkHighlight =
                        model->lines()
                                .at(28)
                                .toMap()
                                .value(QStringLiteral("highlighted"))
                                .toString();
                okay &= Check(lightHighlight.startsWith(
                                      QStringLiteral(
                                              "<pre style=\"margin:0; "
                                              "white-space:pre\">")) &&
                                      lightHighlight.endsWith(
                                              QStringLiteral("</pre>")) &&
                                      model->darkMode() &&
                                      darkHighlight != lightHighlight &&
                                      darkHighlight.contains(
                                              QStringLiteral("#80b9ef")),
                              "source highlighting did not preserve whitespace "
                              "or switch to the dark theme");
                model->setDarkMode(false);
                const int originalLineCount = model->lines().size();
                const qulonglong originalApplyId =
                        model->lines()
                                .at(kApplyControlsLine - 1)
                                .toMap()
                                .value(QStringLiteral("lineId"))
                                .toULongLong();
                okay &= Check(
                        model->toggleBreakpoint(
                                QString::fromLatin1(kVehicleSource),
                                kApplyControlsLine) &&
                                model->insertLineAfter(
                                        kApplyControlsLine - 1) ==
                                        kApplyControlsLine &&
                                model->updateLine(
                                        kApplyControlsLine,
                                        QStringLiteral(
                                                "printf(\"FB17 "
                                                "mapping=%d\\n\", 17);")),
                        "inserted source-line mapping setup failed");
                const QVariantList insertedLines = model->lines();
                const QVariantMap inserted =
                        insertedLines.at(kApplyControlsLine - 1).toMap();
                const QVariantMap shiftedOriginal =
                        insertedLines.at(kApplyControlsLine).toMap();
                okay &= Check(
                        insertedLines.size() == originalLineCount + 1 &&
                                inserted.value(QStringLiteral("inserted"))
                                        .toBool() &&
                                inserted.value(QStringLiteral("editable"))
                                        .toBool() &&
                                inserted.value(QStringLiteral("modified"))
                                        .toBool() &&
                                inserted.value(QStringLiteral("highlighted"))
                                        .toString()
                                        .contains(QStringLiteral("<span")) &&
                                shiftedOriginal
                                                .value(QStringLiteral("lineId"))
                                                .toULongLong() ==
                                        originalApplyId &&
                                shiftedOriginal
                                        .value(QStringLiteral("breakpoint"))
                                        .toBool() &&
                                shiftedOriginal
                                                .value(QStringLiteral("text"))
                                                .toString() ==
                                        originalApplyLine,
                        "insertion did not update highlighting, numbering, "
                        "stable identity, or breakpoint placement");
                okay &= Check(
                        model->deleteLine(kApplyControlsLine) &&
                                model->lines().size() == originalLineCount &&
                                model->lines()
                                                .at(kApplyControlsLine - 1)
                                                .toMap()
                                                .value(QStringLiteral("lineId"))
                                                .toULongLong() ==
                                        originalApplyId &&
                                model->lines()
                                        .at(kApplyControlsLine - 1)
                                        .toMap()
                                        .value(QStringLiteral("breakpoint"))
                                        .toBool() &&
                                !model->hasEdits() &&
                                model->toggleBreakpoint(
                                        QString::fromLatin1(kVehicleSource),
                                        kApplyControlsLine),
                        "removing an inserted line did not restore exact "
                        "numbering and stable breakpoint identity");
                okay &= Check(
                        model->insertLineAfter(0) == 1 &&
                                model->updateLine(
                                        1,
                                        QStringLiteral(
                                                "printf(\"first\\n\");")) &&
                                model->deleteLine(1) &&
                                !model->hasEdits(),
                        "insertion before the first source line was not "
                        "editable and reversible");
                okay &= Check(
                        model->deleteLine(1) && model->hasEdits(),
                        "deleting a non-executable source line was not tracked "
                        "as an editor change");
                model->resetEdits();
                okay &= Check(
                        model->lines().size() == originalLineCount &&
                                SourceLine(
                                        *model,
                                        QString::fromLatin1(kVehicleSource),
                                        1) ==
                                        QStringLiteral(
                                                "#include \"simulation/runtime/"
                                                "replay_vehicle_simulation.h\"") &&
                                !model->hasEdits(),
                        "reset did not restore a deleted non-executable source "
                        "line");
                okay &= Check(
                        model->deleteLine(kApplyControlsLine) &&
                                model->lines().size() ==
                                        originalLineCount - 1 &&
                                model->hasEdits(),
                        "deleting an original source statement was not "
                        "tracked");
                model->resetEdits();
                okay &= Check(
                        model->lines().size() == originalLineCount &&
                                SourceLine(
                                        *model,
                                        QString::fromLatin1(kVehicleSource),
                                        kApplyControlsLine) ==
                                        originalApplyLine &&
                                !model->hasEdits() &&
                                model->insertLineAfter(originalLineCount) < 0 &&
                                !model->deleteLine(0) &&
                                !model->deleteLine(originalLineCount + 1),
                        "reset or invalid insertion/deletion boundaries "
                        "corrupted the immutable source");
                okay &= Check(
                        !originalApplyLine.isEmpty() &&
                                lightHighlight.contains(
                                        QStringLiteral("<span")) &&
                                model->toggleBreakpoint(
                                        QString::fromLatin1(kVehicleSource),
                                        kApplyControlsLine) &&
                                model->toggleBreakpoint(
                                        QString::fromLatin1(kVehicleSource),
                                        kRelocatedBreakpointRequestLine) &&
                                model->updateLine(
                                        kApplyControlsLine,
                                        QStringLiteral(
                                                "  car_.ApplyControlInput("
                                                "CSceneVehicleCar::"
                                                "SControlInput{0.0f, 0.0f, "
                                                "0.0f});")),
                        "future real-engine source edit setup failed");
                const QVariantMap editedLine =
                        model->lines().at(kApplyControlsLine - 1).toMap();
                const QVariantMap editedBreakpointEntry =
                        FileEntry(*model, QString::fromLatin1(kVehicleSource));
                okay &= Check(
                        editedLine.value(QStringLiteral("modified")).toBool() &&
                                editedLine.value(QStringLiteral("breakpoint"))
                                        .toBool() &&
                                editedLine.value(QStringLiteral("text"))
                                                .toString()
                                                .startsWith(
                                                        QStringLiteral("  ")) &&
                                editedLine.value(
                                                   QStringLiteral("highlighted"))
                                                .toString()
                                                .contains(QStringLiteral(
                                                        "\">  car_"
                                                        ".ApplyControlInput")) &&
                                editedLine.value(QStringLiteral("original"))
                                                .toString() ==
                                        originalApplyLine &&
                                HasModifiedEntry(
                                        *model,
                                        QString::fromLatin1(kVehicleSource)) &&
                                editedBreakpointEntry
                                        .value(QStringLiteral("modified"))
                                        .toBool() &&
                                editedBreakpointEntry
                                        .value(QStringLiteral("breakpoint"))
                                        .toBool(),
                        "source indentation, line edit, or persistent file "
                        "breakpoint tracking failed");
                phase = Phase::WaitingInitialFrame;
                responsivenessClock.start();
                lastUiPulse = 0;
                maximumUiGap = 0;
                maximumUiGapPhase = -1;
                maximumUiGapStatus.clear();
                uiPulseCount = 0;
                uiPulse.start();
                QElapsedTimer startTimer;
                startTimer.start();
                okay &= Check(model->hasEdits() &&
                                      viewer.startSimulationDebugger(),
                              "native reference debugger did not start");
                okay &= Check(startTimer.elapsed() < 100,
                              "starting the debugger blocked the UI thread");
                QElapsedTimer restartTimer;
                restartTimer.start();
                okay &= Check(
                        viewer.startSimulationDebugger() &&
                                restartTimer.elapsed() < 100,
                        "replacing a starting debugger session blocked or "
                        "lost the pending restart");
                poll.start();
            });

    // Keep the in-process diagnostic watchdog below CTest's 180 s hard
    // timeout, but do not make it stricter than the integration-test budget.
    QTimer::singleShot(165000, &application, [&]() {
        std::cerr << "native simulation debugger test timed out; phase="
                  << static_cast<int>(phase)
                  << ", status=" << model->statusText().toStdString()
                  << ", error=" << model->editError().toStdString()
                  << ", file=" << model->activeFilePath().toStdString()
                  << ", line=" << model->activeLine()
                  << ", tick=" << model->executionTick()
                  << ", frames=" << viewer.tickCount() << '\n';
        application.exit(1);
    });

    viewer.setPreviewInputScript(QStringLiteral("0.00 press up"));
    viewer.loadMap(QString::fromLocal8Bit(argv[1]),
                   QString::fromLocal8Bit(argv[2]),
                   QStringLiteral("reference"));
    return application.exec();
}
