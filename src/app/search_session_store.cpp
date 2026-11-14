#include "app/search_session_store.h"
#include "app/compact_number_format.h"

#include "mutations/input_event_formatter.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/result_input_script.h"
#include "viewer/map_identity.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>
#include <stdexcept>
#include <map>
#include <limits>
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

QJsonObject Column(const QString &label, const char *kind, const char *better) {
    return {{"label", label}, {"kind", kind}, {"better", better}};
}

// A target's result columns: the label, how the value is shown ("speed" m/s,
// "time" ms, "finishTime" ns, "distance" m, "number", "integer") and which way
// is better ("lower", "higher", or "" for a column that only says when).
// Saved with every restart; the values are the matching Metrics entries.
QJsonArray ColumnsFor(const std::string &targetId, const OptionSettings &settings) {
    const auto setting = [&settings](const char *key) {
        const auto found = settings.find(key);
        return found == settings.end() ? std::string() : found->second;
    };
    const QJsonObject at = Column(QStringLiteral("At"), "time", "");
    if (targetId == kScriptedTargetEvaluationId) {
        QJsonArray columns;
        const QStringList lines = QString::fromStdString(setting("script"))
                                          .split(QLatin1Char('\n'));
        for (const QString &source : lines) {
            const QString line = source.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            QJsonObject column = Column(
                    QStringLiteral("Objective %1").arg(columns.size() + 1), "number", "higher");
            column.insert("tooltip", line);
            column.insert("objective", columns.size());
            columns.append(column);
        }
        return columns;
    }
    if (targetId == kVelocityEvaluationId) {
        return {Column(setting("mode") == "projected" ? QStringLiteral("Projection")
                                                      : QStringLiteral("Speed"),
                       "speed", "higher"),
                at};
    }
    if (targetId == kPointTargetEvaluationId) {
        return {Column(QStringLiteral("Distance"), "distance", "lower"), at};
    }
    if (targetId == kDistanceTargetEvaluationId) {
        // The score orders reaching before distance (see DistanceParts):
        // shown as what it is made of.
        return {Column(QStringLiteral("Reached at"), "time", "lower"),
                Column(QStringLiteral("Checkpoints before"), "integer", "lower"),
                Column(QStringLiteral("Distance"), "distance", "lower"), at};
    }
    if (targetId == kPoseTargetEvaluationId) {
        return {Column(QStringLiteral("Pose error"), "number", "lower"), at};
    }
    if (targetId == kVolumeEntryEvaluationId || targetId == kCustomVolumeEntryEvaluationId) {
        if (setting("measure") == "speed") {
            return {Column(QStringLiteral("Entry speed"), "speed", "higher"), at};
        }
        return {Column(QStringLiteral("Entry time"), "time", "lower")};
    }
    if (targetId == kStuntPointsEvaluationId) {
        return {Column(QStringLiteral("Stunt points"), "integer", "higher")};
    }
    if (targetId == kPreciseFinishTimeEvaluationId) {
        return {Column(QStringLiteral("Finish time"), "finishTime", "lower")};
    }
    if (targetId == kCheckpointTimeEvaluationId) {
        return {Column(QStringLiteral("Checkpoint time"), "time", "lower")};
    }
    if (targetId == kTimeEvaluationId) {
        return {Column(QStringLiteral("Time"), "time",
                       setting("goal") == "latest" ? "higher" : "lower")};
    }
    return {Column(QStringLiteral("Score"), "number", "")};
}

// What a session searched, for its name: the result's label (a script's
// target is named as such).
QString TargetLabel(const std::string &targetId, const QJsonArray &columns) {
    if (targetId == kScriptedTargetEvaluationId) return QStringLiteral("Script");
    return columns.isEmpty() ? QString() : columns.first().toObject().value("label").toString();
}

QJsonArray Columns(const SearchRequest &request) {
    return ColumnsFor(request.evaluationTarget.id, request.evaluationTarget.settings);
}

// The distance target's score (distance_target_evaluator.cpp): a run that
// reaches the target scores its reach time in ms; one that does not,
// 1e12 + 1e6 per event still before the target (the target itself counts
// one) + its closest distance in metres.
struct DistanceParts {
    QJsonValue reachedMs;   // null when not reached
    double eventsBefore = 0.0;
    double distance = 0.0;
};

DistanceParts SplitDistanceScore(double score) {
    constexpr double kNotReached = 1e12;
    constexpr double kPerEvent = 1e6;
    DistanceParts parts;
    if (score < kNotReached) {
        parts.reachedMs = score;
        return parts;
    }
    const double rest = score - kNotReached;
    const double events = std::floor(rest / kPerEvent);
    parts.eventsBefore = std::max(0.0, events - 1.0);
    parts.distance = rest - events * kPerEvent;
    return parts;
}

template<typename Result>
QJsonArray Metrics(const SearchRequest &request,
                   const Result &result) {
    QJsonArray metrics;
    if (request.evaluationTarget.id == kScriptedTargetEvaluationId) {
        for (double value : result.metricValues) metrics.append(value);
        return metrics;
    }
    if (request.evaluationTarget.id == kDistanceTargetEvaluationId) {
        const DistanceParts parts = SplitDistanceScore(result.bestScore);
        metrics.append(parts.reachedMs);
        metrics.append(parts.eventsBefore);
        metrics.append(parts.distance);
        metrics.append(result.bestEvaluationTimeMs);
        return metrics;
    }
    for (const QJsonValue &column : Columns(request)) {
        metrics.append(column.toObject().value("label").toString() == QStringLiteral("At")
                               ? result.bestEvaluationTimeMs : result.bestScore);
    }
    return metrics;
}

template<typename Result>
QJsonArray ObjectiveScores(const Result &result) {
    QJsonArray scores;
    for (double score : result.objectiveScores) scores.append(score);
    return scores;
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
                         {"targetLabel", TargetLabel(request.evaluationTarget.id,
                                                     Columns(request))},
                         {"approximate", request.approximatePhysics},
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

namespace {

// A restart's record (restart-N.json), also the live row's source.
template<typename Result>
QJsonObject CycleMetadata(const SearchRequest &request,
                          std::uint64_t restartNumber,
                          const Result &result,
                          bool aborted,
                          const QString &inputName) {
    QJsonObject metadata{
            {"format", 1},
            {"aborted", aborted},
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
            {"columns", Columns(request)}};
    if (result.approximate) {
        metadata.insert("approximate", QJsonObject{
                {"score", result.approximate->score},
                {"description", QString::fromStdString(
                                        result.approximate->description)},
                {"driftMm", result.approximate->driftMeters
                                    ? QJsonValue(*result.approximate->driftMeters * 1000.0)
                                    : QJsonValue()}});
    }
    return metadata;
}

// A record as a table row: the columns of records saved before they were
// stored, the attempt counts as text.
QVariantMap RowFromMetadata(const QJsonObject &metadata) {
    auto row = metadata.toVariantMap();
    if (!metadata.contains("columns")) {
        const std::string targetId = metadata.value("targetId").toString().toStdString();
        const EvaluationTargetRegistration *registration = FindEvaluationTarget(targetId);
        OptionSettings settings = registration != nullptr ? registration->defaultSettings
                                                          : OptionSettings{};
        const QString first = metadata.value("metricLabels").toArray().first().toString();
        if (first.startsWith(QStringLiteral("Projection"))) settings["mode"] = "projected";
        if (targetId == kScriptedTargetEvaluationId) {
            QStringList lines;
            for (const QJsonValue &label : metadata.value("metricLabels").toArray())
                lines.append(label.toString());
            settings["script"] = lines.join(QLatin1Char('\n')).toStdString();
        }
        row.insert(QStringLiteral("columns"), ColumnsFor(targetId, settings).toVariantList());
    }
    // Distance targets saved before their score was split: the "Distance"
    // column held the raw score.
    if (metadata.value("targetId").toString().toStdString() == kDistanceTargetEvaluationId &&
        row.value(QStringLiteral("columns")).toList().size() == 2) {
        const QJsonArray metrics = metadata.value("metrics").toArray();
        const DistanceParts parts = SplitDistanceScore(metrics.at(0).toDouble());
        row.insert(QStringLiteral("columns"),
                   ColumnsFor(kDistanceTargetEvaluationId, {}).toVariantList());
        row.insert(QStringLiteral("metrics"),
                   QVariantList{parts.reachedMs.toVariant(), parts.eventsBefore, parts.distance,
                                metrics.at(1).toVariant()});
    }
    const auto count = row.value(QStringLiteral("attemptsExact"), row.value(QStringLiteral("attempts"))).toULongLong();
    row.insert(QStringLiteral("attemptsExact"), QString::number(count));
    row.insert(QStringLiteral("attemptsText"), FormatCompactNumber(static_cast<double>(count)));
    row.insert(QStringLiteral("attemptsExactText"), FormatExactCount(count));
    return row;
}

template<typename Result>
void SaveCycleResult(
        const SearchSessionLocation &session,
        const SearchRequest &request,
        std::uint64_t restartNumber,
        const Result &result,
        bool aborted) {
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
    const std::string inputScript = FormatResultInputScript(
            result.bestInputs, request.evaluationTarget.id, result.bestState);
    AtomicWrite(inputPath, QByteArray(inputScript.data(),
                                      static_cast<qsizetype>(inputScript.size())));
    const QJsonObject metadata = CycleMetadata(request, restartNumber, result, aborted, inputName);
    AtomicWrite(metadataPath, QJsonDocument(metadata).toJson());
}

}  // namespace

void SearchSessionStore::SaveCycle(const SearchSessionLocation &session,
                                   const SearchRequest &request,
                                   std::uint64_t restartNumber,
                                   const SearchResult &result) {
    SaveCycleResult(session, request, restartNumber, result, false);
}

void SearchSessionStore::SaveAbortedCycle(const SearchSessionLocation &session,
                                          const SearchRequest &request,
                                          std::uint64_t restartNumber,
                                          const SearchLiveUpdate &result) {
    SaveCycleResult(session, request, restartNumber, result, true);
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
                !QDirIterator(entry.absoluteFilePath(), {QStringLiteral("restart-*.json")},
                              QDir::Files).hasNext()) continue;
            sessions.append(Session(entry.absoluteFilePath()));
        }
    }
    return sessions;
}

QVariantList SearchSessionStore::Cycles(const QString &directory) {
    return ReadCyclePage(directory).rows;
}

QVariantMap SearchSessionStore::Session(const QString &directory) {
    const auto metadata = ReadObject(directory + QStringLiteral("/session.json"));
    // "13 Nov 06:10 · Speed · Approximate": local time (the year when not this
    // one), what was searched, and the physics when it was not exact
    const QDateTime created = QDateTime::fromString(
            metadata.value("createdUtc").toString(), Qt::ISODateWithMs).toLocalTime();
    QStringList parts;
    if (created.isValid()) {
        parts.append(QLocale(QLocale::English).toString(
                created, created.date().year() == QDate::currentDate().year()
                                 ? QStringLiteral("d MMM HH:mm")
                                 : QStringLiteral("d MMM yyyy HH:mm")));
    } else {
        parts.append(metadata.value("createdUtc").toString());
    }
    const std::string targetId = metadata.value("targetId").toString().toStdString();
    QString target = metadata.value("targetLabel").toString();
    if (target.isEmpty()) {
        const EvaluationTargetRegistration *registration = FindEvaluationTarget(targetId);
        target = TargetLabel(targetId, ColumnsFor(targetId, registration != nullptr
                                                                    ? registration->defaultSettings
                                                                    : OptionSettings{}));
    }
    if (!target.isEmpty()) parts.append(target);
    if (metadata.value("approximate").toBool()) parts.append(QStringLiteral("Approximate"));
    return {{"directory", directory}, {"label", parts.join(QStringLiteral(" \u00b7 "))},
            {"packsDirectory", metadata.value("packsDirectory").toString()},
            {"replayPath", metadata.value("replayPath").toString()},
            {"horizonMs", metadata.value("horizonMs").toInt()},
            {"targetId", metadata.value("targetId").toString()}};
}

QVariantMap SearchSessionStore::Cycle(const QString &directory, std::uint64_t restart) {
    const auto metadata = ReadObject(directory + QStringLiteral("/restart-%1.json")
            .arg(static_cast<qulonglong>(restart), 6, 10, QLatin1Char('0')));
    if (metadata.isEmpty()) return {};
    return RowFromMetadata(metadata);
}

QVariantMap SearchSessionStore::LiveRow(const SearchRequest &request,
                                        std::uint64_t restart,
                                        const SearchLiveUpdate &live) {
    QVariantMap row = RowFromMetadata(CycleMetadata(request, restart, live, false, {}));
    row.insert(QStringLiteral("running"), true);
    row.insert(QStringLiteral("improvements"),
               static_cast<qulonglong>(live.mutationImprovementCount));
    if (live.lastImprovementElapsed) {
        row.insert(QStringLiteral("lastImprovementMs"),
                   static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           *live.lastImprovementElapsed).count()));
    }
    return row;
}

SearchSessionStore::CyclePage SearchSessionStore::ReadCyclePage(
        const QString &directory, std::optional<std::uint64_t> anchor, bool older) {
    std::map<std::uint64_t, bool> numbers;
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max(), maximum = 0;
    QDirIterator files(directory, {QStringLiteral("restart-*.json")}, QDir::Files);
    // Scan names only, retaining one page. Read JSON only for that page.
    while (files.hasNext()) {
        files.next();
        const auto name = files.fileName();
        bool valid = false;
        const auto number = name.mid(8, name.size() - 13).toULongLong(&valid);
        if (!valid) continue;
        minimum = std::min(minimum, static_cast<std::uint64_t>(number));
        maximum = std::max(maximum, static_cast<std::uint64_t>(number));
        if (anchor && (older ? number >= *anchor : number <= *anchor)) continue;
        numbers.emplace(number, true);
        if (numbers.size() > kCyclePageSize)
            numbers.erase(older ? numbers.begin() : std::prev(numbers.end()));
    }
    CyclePage page;
    for (const auto &[number, unused] : numbers) {
        const auto row = Cycle(directory, number);
        if (!row.isEmpty()) page.rows.append(row);
    }
    if (!numbers.empty()) {
        page.hasOlder = minimum < numbers.begin()->first;
        page.hasNewer = maximum > numbers.rbegin()->first;
    }
    return page;
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
