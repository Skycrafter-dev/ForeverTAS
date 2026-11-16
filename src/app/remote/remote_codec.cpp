#include "app/remote/remote_codec.h"

#include <QJsonValue>

#include <chrono>
#include <stdexcept>

namespace forevertas::app::remote {
namespace {

using forevervalidator::experimental::PhysicsSandboxCudaConditionInstruction;
using forevervalidator::experimental::PhysicsSandboxCudaConditionOpcode;
using forevervalidator::experimental::PhysicsSandboxCudaConditionValue;
using forevervalidator::experimental::PhysicsSandboxInputAction;
using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

// 64-bit counts travel as text (a JSON number keeps 53 bits)
QJsonValue U64(std::uint64_t value) {
    return QString::number(value);
}
std::uint64_t ToU64(const QJsonValue &value) {
    bool ok = false;
    const std::uint64_t parsed = value.toString().toULongLong(&ok);
    if (!ok) throw std::runtime_error("remote: a count is not a number");
    return parsed;
}
QJsonValue Duration(std::chrono::steady_clock::duration value) {
    return U64(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count()));
}
std::chrono::steady_clock::duration ToDuration(const QJsonValue &value) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(static_cast<std::int64_t>(ToU64(value))));
}
QJsonArray Doubles(const std::vector<double> &values) {
    QJsonArray array;
    for (const double value : values) array.append(value);
    return array;
}
std::vector<double> ToDoubles(const QJsonValue &value) {
    std::vector<double> values;
    for (const QJsonValue &item : value.toArray()) values.push_back(item.toDouble());
    return values;
}
QJsonObject Settings(const OptionConfiguration &configuration) {
    QJsonObject settings;
    for (const auto &[key, value] : configuration.settings)
        settings.insert(QString::fromStdString(key), QString::fromStdString(value));
    return QJsonObject{{"id", QString::fromStdString(configuration.id)}, {"settings", settings}};
}
OptionConfiguration ToSettings(const QJsonValue &value) {
    const QJsonObject object = value.toObject();
    OptionConfiguration configuration;
    configuration.id = object.value("id").toString().toStdString();
    const QJsonObject settings = object.value("settings").toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it)
        configuration.settings[it.key().toStdString()] = it.value().toString().toStdString();
    return configuration;
}
QJsonValue Approximate(const std::optional<ApproximateResult> &approximate) {
    if (!approximate) return QJsonValue();
    QJsonObject object{{"score", approximate->score},
                       {"description", QString::fromStdString(approximate->description)}};
    if (approximate->driftMeters) object.insert("drift", *approximate->driftMeters);
    return object;
}
std::optional<ApproximateResult> ToApproximate(const QJsonValue &value) {
    if (!value.isObject()) return std::nullopt;
    const QJsonObject object = value.toObject();
    ApproximateResult approximate;
    approximate.score = object.value("score").toDouble();
    approximate.description = object.value("description").toString().toStdString();
    if (object.contains("drift")) approximate.driftMeters = object.value("drift").toDouble();
    return approximate;
}

}  // namespace

QJsonArray EncodeEvents(const std::vector<SandboxInputEvent> &events) {
    QJsonArray array;
    for (const SandboxInputEvent &event : events) {
        array.append(QJsonArray{event.timeMs, static_cast<int>(event.action), static_cast<int>(event.value.kind),
                                static_cast<int>(event.value.switchState), event.value.analog});
    }
    return array;
}

std::vector<SandboxInputEvent> DecodeEvents(const QJsonArray &array) {
    std::vector<SandboxInputEvent> events;
    events.reserve(static_cast<std::size_t>(array.size()));
    for (const QJsonValue &value : array) {
        const QJsonArray item = value.toArray();
        if (item.size() != 5) throw std::runtime_error("remote: an input event is malformed");
        SandboxInputEvent event;
        event.timeMs = item.at(0).toInt();
        event.action = static_cast<PhysicsSandboxInputAction>(item.at(1).toInt());
        event.value.kind = static_cast<PhysicsSandboxInputValueKind>(item.at(2).toInt());
        event.value.switchState = static_cast<PhysicsSandboxSwitchState>(item.at(3).toInt());
        event.value.analog = item.at(4).toInt();
        events.push_back(event);
    }
    return events;
}

QJsonObject EncodeRequest(const SearchRequest &request) {
    QJsonArray modifiers;
    for (const OptionConfiguration &modifier : request.modifiers) modifiers.append(Settings(modifier));
    QJsonArray commands;
    for (const ParsedInputCommand &command : request.baseInputCommands) {
        commands.append(QJsonArray{QString::number(command.userTimeMs), static_cast<int>(command.action),
                                   static_cast<int>(command.value.kind), static_cast<int>(command.value.switchState),
                                   command.value.analog, static_cast<double>(command.sourceLine)});
    }
    QJsonObject object{{"backend", QString::fromLatin1(PhysicsBackendId(request.backend).data(),
                                                       static_cast<int>(PhysicsBackendId(request.backend).size()))},
                       {"parallelSampleCount", static_cast<double>(request.parallelSampleCount)},
                       {"calibrate", request.calibrateCudaParallelSampleCount},
                       {"search", Settings(request.searchAlgorithm)},
                       {"modifiers", modifiers},
                       {"target", Settings(request.evaluationTarget)},
                       {"baseInputs", commands},
                       {"approximate", request.approximatePhysics},
                       {"horizonMs", static_cast<double>(request.simulationHorizonMs)}};
    if (request.condition) {
        QJsonArray instructions;
        for (const PhysicsSandboxCudaConditionInstruction &instruction : request.condition->cuda.instructions) {
            instructions.append(QJsonArray{static_cast<int>(instruction.opcode), static_cast<int>(instruction.value),
                                           instruction.x, instruction.y, instruction.z});
        }
        object.insert("condition",
                      QJsonObject{{"instructions", instructions},
                                  {"lastImprovement", request.condition->cuda.lastImprovementTimeSeconds},
                                  {"lastRestart", request.condition->cuda.lastRestartTimeSeconds}});
    }
    return object;
}

SearchRequest DecodeRequest(const QJsonObject &object, const std::string &packDirectory,
                            const std::string &replayPath) {
    SearchRequest request(packDirectory, replayPath);
    const auto backend = ParsePhysicsBackend(object.value("backend").toString().toStdString());
    if (!backend) throw std::runtime_error("remote: unknown backend");
    request.backend = *backend;
    request.parallelSampleCount = static_cast<std::uint32_t>(object.value("parallelSampleCount").toDouble());
    request.calibrateCudaParallelSampleCount = object.value("calibrate").toBool();
    request.searchAlgorithm = ToSettings(object.value("search"));
    request.modifiers.clear();
    for (const QJsonValue &modifier : object.value("modifiers").toArray()) request.modifiers.push_back(ToSettings(modifier));
    request.evaluationTarget = ToSettings(object.value("target"));
    for (const QJsonValue &value : object.value("baseInputs").toArray()) {
        const QJsonArray item = value.toArray();
        if (item.size() != 6) throw std::runtime_error("remote: a base input is malformed");
        ParsedInputCommand command;
        command.userTimeMs = item.at(0).toString().toLongLong();
        command.action = static_cast<SandboxInputAction>(item.at(1).toInt());
        command.value.kind = static_cast<PhysicsSandboxInputValueKind>(item.at(2).toInt());
        command.value.switchState = static_cast<PhysicsSandboxSwitchState>(item.at(3).toInt());
        command.value.analog = item.at(4).toInt();
        command.sourceLine = static_cast<std::size_t>(item.at(5).toDouble());
        request.baseInputCommands.push_back(command);
    }
    request.approximatePhysics = object.value("approximate").toBool();
    request.simulationHorizonMs = static_cast<std::uint32_t>(object.value("horizonMs").toDouble());
    if (object.contains("condition")) {
        const QJsonObject condition = object.value("condition").toObject();
        ConditionProgram program;
        for (const QJsonValue &value : condition.value("instructions").toArray()) {
            const QJsonArray item = value.toArray();
            if (item.size() != 5) throw std::runtime_error("remote: a condition instruction is malformed");
            PhysicsSandboxCudaConditionInstruction instruction;
            instruction.opcode = static_cast<PhysicsSandboxCudaConditionOpcode>(item.at(0).toInt());
            instruction.value = static_cast<PhysicsSandboxCudaConditionValue>(item.at(1).toInt());
            instruction.x = item.at(2).toDouble();
            instruction.y = item.at(3).toDouble();
            instruction.z = item.at(4).toDouble();
            program.cuda.instructions.push_back(instruction);
        }
        program.cuda.lastImprovementTimeSeconds = condition.value("lastImprovement").toDouble();
        program.cuda.lastRestartTimeSeconds = condition.value("lastRestart").toDouble();
        request.condition = std::move(program);
    }
    return request;
}

QJsonObject EncodeShare(const SearchRunControl &control) {
    QJsonObject object{{"offset", U64(control.iterationIndexOffset)},
                       {"stride", U64(control.iterationIndexStride)},
                       {"improvementTimelines", control.sampleImprovementTimelines},
                       {"bestTimeline", control.sampleBestTimeline},
                       {"attempts", static_cast<bool>(control.attemptCompleted)}};
    if (control.iterationLimit) object.insert("limit", U64(*control.iterationLimit));
    if (control.evaluationEndTimeLimitMs)
        object.insert("evaluationEndLimitMs", QString::number(*control.evaluationEndTimeLimitMs));
    return object;
}

void DecodeShare(const QJsonObject &object, SearchRunControl *control) {
    control->iterationIndexOffset = ToU64(object.value("offset"));
    control->iterationIndexStride = ToU64(object.value("stride"));
    control->sampleImprovementTimelines = object.value("improvementTimelines").toBool();
    control->sampleBestTimeline = object.value("bestTimeline").toBool();
    control->reuseLoadedSandbox = false;
    if (object.contains("limit")) control->iterationLimit = ToU64(object.value("limit"));
    if (object.contains("evaluationEndLimitMs"))
        control->evaluationEndTimeLimitMs = object.value("evaluationEndLimitMs").toString().toLongLong();
}

QJsonObject EncodeLive(const SearchLiveUpdate &live) {
    QJsonObject object{{"source", static_cast<int>(live.winnerSource)},
                       {"mutationCount", U64(live.winningMutationCount)},
                       {"score", live.bestScore},
                       {"timeMs", live.bestEvaluationTimeMs},
                       {"description", QString::fromStdString(live.bestEvaluationDescription)},
                       {"inputs", EncodeEvents(live.bestInputs)},
                       {"iterations", U64(live.iterations)},
                       {"evaluatorCalls", U64(live.evaluatorCalls)},
                       {"improvements", U64(live.mutationImprovementCount)},
                       {"mutations", U64(live.totalMutationCount)},
                       {"elapsed", Duration(live.elapsed)},
                       {"objectives", Doubles(live.objectiveScores)},
                       {"metrics", Doubles(live.metricValues)},
                       {"approximate", Approximate(live.approximate)}};
    if (live.winningIterationIndex) object.insert("iteration", U64(*live.winningIterationIndex));
    if (live.lastImprovementElapsed) object.insert("lastImprovement", Duration(*live.lastImprovementElapsed));
    return object;
}

SearchLiveUpdate DecodeLive(const QJsonObject &object) {
    SearchLiveUpdate live;
    live.winnerSource = static_cast<SearchWinnerSource>(object.value("source").toInt());
    live.winningMutationCount = static_cast<std::size_t>(ToU64(object.value("mutationCount")));
    live.bestScore = object.value("score").toDouble();
    live.bestEvaluationTimeMs = object.value("timeMs").toDouble();
    live.bestEvaluationDescription = object.value("description").toString().toStdString();
    live.bestInputs = DecodeEvents(object.value("inputs").toArray());
    live.iterations = ToU64(object.value("iterations"));
    live.evaluatorCalls = ToU64(object.value("evaluatorCalls"));
    live.mutationImprovementCount = ToU64(object.value("improvements"));
    live.totalMutationCount = ToU64(object.value("mutations"));
    live.elapsed = ToDuration(object.value("elapsed"));
    live.objectiveScores = ToDoubles(object.value("objectives"));
    live.metricValues = ToDoubles(object.value("metrics"));
    live.approximate = ToApproximate(object.value("approximate"));
    if (object.contains("iteration")) live.winningIterationIndex = ToU64(object.value("iteration"));
    if (object.contains("lastImprovement")) live.lastImprovementElapsed = ToDuration(object.value("lastImprovement"));
    return live;
}

QJsonObject EncodeResult(const SearchResult &result) {
    QJsonObject object{{"source", static_cast<int>(result.winnerSource)},
                       {"mutationCount", U64(result.winningMutationCount)},
                       {"score", result.bestScore},
                       {"timeMs", result.bestEvaluationTimeMs},
                       {"description", QString::fromStdString(result.bestEvaluationDescription)},
                       {"inputs", EncodeEvents(result.bestInputs)},
                       {"iterations", U64(result.iterations)},
                       {"evaluatorCalls", U64(result.evaluatorCalls)},
                       {"improvements", U64(result.mutationImprovementCount)},
                       {"mutations", U64(result.totalMutationCount)},
                       {"elapsed", Duration(result.elapsed)},
                       {"objectives", Doubles(result.objectiveScores)},
                       {"metrics", Doubles(result.metricValues)},
                       {"approximate", Approximate(result.approximate)}};
    if (result.winningIterationIndex) object.insert("iteration", U64(*result.winningIterationIndex));
    if (result.lastImprovementElapsed) object.insert("lastImprovement", Duration(*result.lastImprovementElapsed));
    return object;
}

RemoteOutcome DecodeResult(const QJsonObject &object) {
    RemoteOutcome result;
    result.winnerSource = static_cast<SearchWinnerSource>(object.value("source").toInt());
    result.winningMutationCount = static_cast<std::size_t>(ToU64(object.value("mutationCount")));
    result.bestScore = object.value("score").toDouble();
    result.bestInputs = DecodeEvents(object.value("inputs").toArray());
    result.iterations = ToU64(object.value("iterations"));
    result.evaluatorCalls = ToU64(object.value("evaluatorCalls"));
    result.mutationImprovementCount = ToU64(object.value("improvements"));
    result.totalMutationCount = ToU64(object.value("mutations"));
    result.elapsed = ToDuration(object.value("elapsed"));
    result.approximate = ToApproximate(object.value("approximate"));
    if (object.contains("iteration")) result.winningIterationIndex = ToU64(object.value("iteration"));
    if (object.contains("lastImprovement")) result.lastImprovementElapsed = ToDuration(object.value("lastImprovement"));
    return result;
}

}  // namespace forevertas::app::remote
