#include "app/search_session_store.h"
#include "app/compact_number_format.h"

#include "mutations/input_event_formatter.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "viewer/map_identity.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <stdexcept>
#include <utility>

namespace forevertas::app {
namespace {

template<typename T, typename E>
T Require(forevervalidator::DiscriminatedResult<T, E> result,
          const char *operation) {
    if (!result) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 result.Error().diagnostic);
    }
    return std::move(result).Value();
}

QString SafeMapName(QString name) {
    for (qsizetype i = 0; i < name.size(); ++i) {
        if (!name[i].isLetterOrNumber() && name[i] != '-' &&
            name[i] != '_') name[i] = '_';
    }
    name = name.left(64);
    return name.isEmpty() ? QStringLiteral("map") : name;
}

void AtomicWrite(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        throw std::runtime_error(
                QStringLiteral("Could not save %1: %2")
                        .arg(path, file.errorString()).toStdString());
    }
}

QJsonObject ReadObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
            file.readAll(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
            ? document.object() : QJsonObject{};
}

QJsonArray Metrics(const SearchRequest &request,
                   const SearchResult &result) {
    QJsonArray metrics;
    const std::string &targetId = request.evaluationTarget.id;
    if (targetId == kScriptedTargetEvaluationId) {
        for (double value : result.metricValues) metrics.append(value);
        return metrics;
    }
    metrics.append(result.bestScore);
    if (targetId == kVelocityEvaluationId ||
        targetId == kPointTargetEvaluationId ||
        targetId == kPoseTargetEvaluationId) {
        metrics.append(result.bestEvaluationTimeMs);
    }
    return metrics;
}

QJsonArray ObjectiveScores(const SearchResult &result) {
    QJsonArray scores;
    for (double score : result.objectiveScores) scores.append(score);
    return scores;
}

QJsonArray MetricLabels(const SearchRequest &request) {
    QJsonArray labels;
    const std::string &targetId = request.evaluationTarget.id;
    if (targetId != kScriptedTargetEvaluationId) {
        if (targetId == kVelocityEvaluationId) {
            const auto mode = request.evaluationTarget.settings.find("mode");
            labels.append(mode != request.evaluationTarget.settings.end() &&
                                  mode->second == "projected"
                                  ? QStringLiteral("Projection (m/s)")
                                  : QStringLiteral("Speed (m/s)"));
        } else if (targetId == kStuntPointsEvaluationId) {
            labels.append(QStringLiteral("Stunt points"));
        } else if (targetId == kPreciseFinishTimeEvaluationId) {
            labels.append(QStringLiteral("Finish upper bound (ns)"));
        } else if (targetId == kVolumeEntryEvaluationId ||
                   targetId == kCustomVolumeEntryEvaluationId) {
            labels.append(QStringLiteral("Entry time (ms)"));
        } else if (targetId == kPointTargetEvaluationId) {
            labels.append(QStringLiteral("Distance (m)"));
        } else if (targetId == kPoseTargetEvaluationId) {
            labels.append(QStringLiteral("Pose error"));
        } else {
            labels.append(QStringLiteral("Score"));
        }
        if (targetId == kVelocityEvaluationId ||
            targetId == kPointTargetEvaluationId ||
            targetId == kPoseTargetEvaluationId) {
            labels.append(QStringLiteral("At (ms)"));
        }
        return labels;
    }
    const auto script = request.evaluationTarget.settings.find("script");
    if (script == request.evaluationTarget.settings.end()) return labels;
    const QStringList lines = QString::fromStdString(script->second)
                                      .split(QLatin1Char('\n'));
    for (const QString &source : lines) {
        const QString line = source.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        labels.append(line);
    }
    return labels;
}

QJsonArray ModifierSeeds(const SearchRequest &request) {
    QJsonArray seeds;
    for (const OptionConfiguration &modifier : request.modifiers) {
        const auto seed = modifier.settings.find("seed");
        seeds.append(seed == modifier.settings.end()
                             ? QJsonValue{} : QJsonValue(
                                   QString::fromStdString(seed->second)));
    }
    return seeds;
}

}  // namespace

QString SearchSessionStore::Root(const QString &overrideRoot) {
    if (!overrideRoot.isEmpty()) return overrideRoot;
    const QString environmentRoot = qEnvironmentVariable(
            "FOREVERTAS_AUTORESTARTS_ROOT");
    if (!environmentRoot.isEmpty()) {
        return environmentRoot;
    }
    return QStandardPaths::writableLocation(
                   QStandardPaths::DocumentsLocation) +
            QStringLiteral("/ForeverTAS/Scripts/Autorestarts");
}

SearchSessionLocation SearchSessionStore::Create(
        const SearchRequest &request, const QString &root) {
    SearchSessionLocation location = Identify(request);
    const QString mapDirectory = Root(root) + '/' +
            SafeMapName(location.mapName) + '-' + location.mapKey.right(16);
    const QString sessionId = QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd-HHmmss-zzz")) + '-' +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    location.directory = mapDirectory + '/' + sessionId;
    if (!QDir().mkpath(location.directory)) {
        throw std::runtime_error(
                QStringLiteral("Could not create session directory: %1")
                        .arg(location.directory).toStdString());
    }
    QJsonObject metadata{{"format", 1}, {"mapKey", location.mapKey},
                         {"mapName", location.mapName}, {"sessionId", sessionId},
                         {"packsDirectory", QString::fromStdString(
                                                      request.packDirectory)},
                         {"replayPath", QString::fromStdString(
                                               request.replayPath)},
                         {"horizonMs", static_cast<qint64>(
                                               request.simulationHorizonMs)},
                         {"targetId", QString::fromStdString(
                                                  request.evaluationTarget.id)},
                         {"createdUtc", QDateTime::currentDateTimeUtc()
                                                .toString(Qt::ISODateWithMs)}};
    AtomicWrite(location.directory + QStringLiteral("/session.json"),
                QJsonDocument(metadata).toJson());
    return location;
}

SearchSessionLocation SearchSessionStore::Identify(
        const SearchRequest &request) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const ReplayIdentity identity{request.replayPath};
    AssetSource source = Require(
            OpenInstalledPackDirectory(request.packDirectory),
            "opening Packs directory for session");
    AssetBytes replay = Require(
            ReadReplayFileUtf8(request.replayPath, identity),
            "reading replay for session");
    PhysicsSandboxOptions options;
    options.backend = ToForeverValidatorBackend(
            kAuxiliarySimulationBackend);
    options.tickDurationMs = kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = request.simulationHorizonMs;
    PhysicsSandbox sandbox = Require(
            CreatePhysicsSandbox(std::move(source), options),
            "creating session sandbox");
    Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity),
            "loading replay for session");
    const QString mapName = QString::fromUtf8(Require(
            sandbox.ReadMapName(), "reading map name"));
    const QString mapKey = viewer::CollisionSceneKey(Require(
            sandbox.ReadScene(), "reading map collision scene"));
    return {mapKey, mapName, {}};
}

void SearchSessionStore::SaveCycle(
        const SearchSessionLocation &session,
        const SearchRequest &request,
        std::uint64_t restartNumber,
        const SearchResult &result) {
    const QString stem = QStringLiteral("restart-%1")
                                 .arg(static_cast<qulonglong>(restartNumber),
                                      6, 10, QLatin1Char('0'));
    const QString inputName = stem + QStringLiteral(".txt");
    const QString metadataPath = session.directory + '/' + stem +
            QStringLiteral(".json");
    const QString inputPath = session.directory + '/' + inputName;
    if (QFileInfo::exists(metadataPath) || QFileInfo::exists(inputPath)) {
        throw std::runtime_error("Restart result already exists; refusing to overwrite it");
    }
    const std::string inputScript = FormatInputScript(result.bestInputs);
    AtomicWrite(inputPath, QByteArray(inputScript.data(),
                                      static_cast<qsizetype>(inputScript.size())));
    QJsonObject metadata{
            {"format", 1},
            {"restart", static_cast<qint64>(restartNumber)},
            {"attempts", static_cast<qint64>(result.iterations)},
            {"attemptsExact", QString::number(static_cast<qulonglong>(result.iterations))},
            {"elapsedMs", static_cast<qint64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                            result.elapsed).count())},
            {"inputFile", inputName},
            {"score", result.bestScore},
            {"evaluationTimeMs", result.bestEvaluationTimeMs},
            {"summary", QString::fromStdString(
                                result.bestEvaluationDescription)},
            {"targetId", QString::fromStdString(
                                 request.evaluationTarget.id)},
            {"modifierSeeds", ModifierSeeds(request)},
            {"metrics", Metrics(request, result)},
            {"objectiveScores", ObjectiveScores(result)},
            {"metricLabels", MetricLabels(request)}};
    AtomicWrite(metadataPath, QJsonDocument(metadata).toJson());
}

QVariantList SearchSessionStore::SessionsForMap(
        const QString &mapKey, const QString &root) {
    QVariantList sessions;
    const QDir base(Root(root));
    for (const QFileInfo &map : base.entryInfoList(
                 QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir directory(map.absoluteFilePath());
        for (const QFileInfo &entry : directory.entryInfoList(
                     QDir::Dirs | QDir::NoDotAndDotDot,
                     QDir::Time | QDir::Reversed)) {
            const QJsonObject metadata = ReadObject(
                    entry.absoluteFilePath() + QStringLiteral("/session.json"));
            if (metadata.value("mapKey").toString() != mapKey ||
                QDir(entry.absoluteFilePath())
                        .entryList({QStringLiteral("restart-*.json")},
                                   QDir::Files).isEmpty()) continue;
            sessions.append(QVariantMap{
                    {"directory", entry.absoluteFilePath()},
                    {"label", metadata.value("createdUtc").toString()},
                    {"packsDirectory", metadata.value("packsDirectory")
                                                 .toString()},
                    {"replayPath", metadata.value("replayPath").toString()},
                    {"horizonMs", metadata.value("horizonMs").toInt()},
                    {"targetId", metadata.value("targetId").toString()}});
        }
    }
    return sessions;
}

QVariantList SearchSessionStore::Cycles(const QString &directory) {
    QVariantList cycles;
    const QDir session(directory);
    for (const QFileInfo &entry : session.entryInfoList(
                 {QStringLiteral("restart-*.json")}, QDir::Files,
                 QDir::Name)) {
        const QJsonObject metadata = ReadObject(entry.absoluteFilePath());
        if (!metadata.isEmpty()) {
            QVariantMap row = metadata.toVariantMap();
            const auto count = row.value(QStringLiteral("attemptsExact"),
                                         row.value(QStringLiteral("attempts"))).toULongLong();
            row.insert(QStringLiteral("attemptsExact"), QString::number(count));
            row.insert(QStringLiteral("attemptsText"), FormatExactCount(count));
            cycles.append(row);
        }
    }
    return cycles;
}

QString SearchSessionStore::Inputs(const QString &directory,
                                   const QString &fileName) {
    if (QFileInfo(fileName).fileName() != fileName ||
        !fileName.endsWith(QStringLiteral(".txt"))) return {};
    QFile file(directory + '/' + fileName);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                         : QString{};
}

}  // namespace forevertas::app
