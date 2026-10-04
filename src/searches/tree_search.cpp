#include "searches/tree_search.h"

#include "mutations/input_event_utils.h"
#include "physics_backend.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace forevertas {
namespace {

using forevervalidator::DiscriminatedResult;
using forevervalidator::Vector3;
using forevervalidator::experimental::PhysicsSandbox;
using forevervalidator::experimental::PhysicsSandboxCarState;
using forevervalidator::experimental::PhysicsSandboxInputAction;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxState;
using forevervalidator::experimental::PhysicsSandboxStateView;

// The control and state helpers mirror BasicBruteForceSearch so both
// algorithms honour the same SearchRunControl contract.
template<typename T, typename Error>
T Require(DiscriminatedResult<T, Error> result, const char *operation) {
    if (!result) {
        std::string message = std::string(operation) + " failed";
        if (!result.Error().diagnostic.empty()) {
            message += ": " + result.Error().diagnostic;
        }
        throw std::runtime_error(std::move(message));
    }
    return std::move(result).Value();
}

bool SameVector(const Vector3 &left, const Vector3 &right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool SameCar(const PhysicsSandboxCarState &left,
             const PhysicsSandboxCarState &right) {
    return left.rotationX == right.rotationX &&
           left.rotationY == right.rotationY &&
           left.rotationZ == right.rotationZ &&
           left.rotationW == right.rotationW &&
           SameVector(left.position, right.position) &&
           SameVector(left.linearSpeed, right.linearSpeed) &&
           SameVector(left.angularSpeed, right.angularSpeed) &&
           SameVector(left.force, right.force) &&
           SameVector(left.torque, right.torque);
}

bool SameState(const PhysicsSandboxStateView &left,
               const PhysicsSandboxStateView &right) {
    return left.tick == right.tick && left.timeMs == right.timeMs &&
           left.mapEnvironment == right.mapEnvironment &&
           left.vehicleModel == right.vehicleModel &&
           left.playMode == right.playMode && SameCar(left.car, right.car) &&
           left.accelerate == right.accelerate &&
           left.brake == right.brake && left.steering == right.steering &&
           left.checkpointsCollected == right.checkpointsCollected &&
           left.checkpointsTotal == right.checkpointsTotal &&
           left.completedLaps == right.completedLaps &&
           left.totalLaps == right.totalLaps &&
           left.raceCompleted == right.raceCompleted &&
           left.finishTimeMs == right.finishTimeMs &&
           left.finishTime == right.finishTime &&
           left.respawnCount == right.respawnCount &&
           left.stuntsScore == right.stuntsScore;
}

bool SameInputs(const std::vector<PhysicsSandboxInputEvent> &left,
                const std::vector<PhysicsSandboxInputEvent> &right) {
    return left.size() == right.size() &&
            std::equal(left.begin(), left.end(), right.begin(),
                       [](const PhysicsSandboxInputEvent &first,
                          const PhysicsSandboxInputEvent &second) {
                           return SameInputEvent(first, second);
                       });
}

std::uint64_t HashInputs(const std::vector<PhysicsSandboxInputEvent> &events) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash = (hash ^ value) * 1099511628211ull;
    };
    for (const PhysicsSandboxInputEvent &event : events) {
        mix(static_cast<std::uint64_t>(event.timeMs));
        mix(static_cast<std::uint64_t>(event.action));
        mix(static_cast<std::uint64_t>(event.value.kind));
        mix(static_cast<std::uint64_t>(
                static_cast<std::int64_t>(event.value.analog)));
        mix(static_cast<std::uint64_t>(event.value.switchState));
    }
    return hash;
}

// Earliest input time at which applying the patch changes the inputs, or
// nothing when the patch reproduces them.
std::optional<std::int64_t> FirstPatchedChangeMs(
        const std::vector<PhysicsSandboxInputEvent> &inputs,
        const MutationWindowPatch &patch) {
    auto current = std::lower_bound(
            inputs.begin(), inputs.end(), patch.minimumTimeMs,
            [](const PhysicsSandboxInputEvent &event, std::int64_t timeMs) {
                return event.timeMs < timeMs;
            });
    const auto last = std::upper_bound(
            current, inputs.end(), patch.maximumTimeMs,
            [](std::int64_t timeMs, const PhysicsSandboxInputEvent &event) {
                return timeMs < event.timeMs;
            });
    auto replacement = patch.events.begin();
    for (; current != last && replacement != patch.events.end();
         ++current, ++replacement) {
        if (!SameInputEvent(*current, *replacement)) {
            return std::min<std::int64_t>(current->timeMs,
                                          replacement->timeMs);
        }
    }
    if (current != last) return current->timeMs;
    if (replacement != patch.events.end()) return replacement->timeMs;
    return std::nullopt;
}

void CheckCancellation(const SearchRunControl *control) {
    if (control != nullptr && control->cancellationRequested &&
        control->cancellationRequested()) {
        throw SearchCancelled();
    }
}

void BeginIteration(const SearchRunControl *control) {
    CheckCancellation(control);
    if (control != nullptr && control->beginIteration &&
        !control->beginIteration()) {
        throw SearchCancelled();
    }
}

bool StopRequested(const SearchRunControl *control) {
    return control != nullptr && control->stopRequested &&
            control->stopRequested();
}

bool IterationLimitReached(const SearchRunControl *control,
                           std::uint64_t iterations) {
    return control != nullptr && control->iterationLimit &&
            iterations >= *control->iterationLimit;
}

void ReportProgress(const SearchRunControl *control,
                    SearchProgressStage stage,
                    std::uint64_t completedWork) {
    if (control != nullptr && control->progressChanged) {
        control->progressChanged({stage, completedWork, 0u});
    }
}

PhysicsSandboxStateView AdvanceTo(PhysicsSandbox &sandbox,
                                  std::int64_t currentTimeMs,
                                  std::int64_t targetTimeMs,
                                  std::uint32_t tickDurationMs,
                                  const SearchRunControl *control) {
    if (currentTimeMs > targetTimeMs) {
        throw std::runtime_error("sandbox is already past the requested time");
    }
    if ((targetTimeMs - currentTimeMs) % tickDurationMs != 0) {
        throw std::runtime_error(
                "sandbox state time is not aligned to the tick duration");
    }
    if (currentTimeMs == targetTimeMs) {
        return Require(sandbox.ReadState(), "reading sandbox state");
    }
    constexpr std::uint32_t maxTicksPerAdvance = 128u;
    std::uint64_t ticksRemaining = static_cast<std::uint64_t>(
            (targetTimeMs - currentTimeMs) / tickDurationMs);
    PhysicsSandboxStateView state;
    while (ticksRemaining != 0u) {
        CheckCancellation(control);
        const std::uint32_t ticks = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(ticksRemaining, maxTicksPerAdvance));
        state = Require(sandbox.AdvanceTicks(ticks), "advancing sandbox");
        ticksRemaining -= ticks;
    }
    CheckCancellation(control);
    return state;
}

double WallClockSeconds() {
    return std::chrono::duration<double>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

std::optional<TreeSearchSettings> ParseTreeSettings(
        const OptionSettings &settings) {
    const auto segmentCount =
            ParseUnsignedDecimal32(settings.at("segmentCount"));
    const auto leafCount = ParseUnsignedDecimal32(settings.at("leafCount"));
    const auto branchedSegmentCount =
            ParseUnsignedDecimal32(settings.at("branchedSegmentCount"));
    const auto flatWorkerCount =
            ParseUnsignedDecimal32(settings.at("flatWorkerCount"));
    const std::string &unbranched = settings.at("unbranchedSegments");
    const auto varyBranchedSegmentCount =
            ParseBoolean(settings.at("varyBranchedSegmentCount"));
    const auto autoPromoteBest = ParseBoolean(settings.at("autoPromoteBest"));
    if (!segmentCount || !leafCount || !branchedSegmentCount ||
        !varyBranchedSegmentCount || !flatWorkerCount || !autoPromoteBest ||
        (unbranched != "draw" && unbranched != "keep") ||
        *segmentCount < kMinimumTreeSegmentCount ||
        *segmentCount > kMaximumTreeSegmentCount ||
        *leafCount > kMaximumTreeLeafCount ||
        *branchedSegmentCount > *segmentCount ||
        *flatWorkerCount > kMaximumTreeFlatWorkerCount) {
        return std::nullopt;
    }
    return TreeSearchSettings{*segmentCount,
                              *leafCount,
                              *branchedSegmentCount,
                              *varyBranchedSegmentCount,
                              unbranched == "keep",
                              *flatWorkerCount,
                              *autoPromoteBest};
}

std::uint64_t MixBits(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

struct BestIteration {
    std::optional<EvaluationSample> evaluation;
    SearchWinnerSource source = SearchWinnerSource::Baseline;
    std::optional<std::uint64_t> iterationIndex;
    std::size_t mutationCount = 0u;
    PhysicsSandboxStateView view;
    std::optional<PhysicsSandboxState> snapshot;
    std::vector<PhysicsSandboxInputEvent> inputs;
};

// Observation state carried along one root-to-leaf path. Branching clones
// it, so every leaf sees exactly the timeline a flat attempt would see.
struct Branch {
    std::unique_ptr<IterationEvaluationSession> session;
    PhysicsSandboxStateView state;
    std::vector<PhysicsSandboxInputEvent> inputs;
    std::optional<EvaluationSample> cumulativeSample;
    PhysicsSandboxStateView cumulativeView;
    bool completed = false;

    Branch Clone() const {
        return {session->Clone(),
                state,
                inputs,
                cumulativeSample,
                cumulativeView,
                completed};
    }
};

// Items each pass anchors in one segment, indexed [pass][slot].
using SegmentAllocation = std::vector<std::vector<std::uint32_t>>;

class TreeSearchRun final {
public:
    TreeSearchRun(const SearchExecutionContext &context,
                  TreeSearchSettings settings,
                  std::chrono::steady_clock::time_point started)
        : context_(context),
          sandbox_(context.sandbox),
          tick_(context.tickDurationMs),
          settings_(settings),
          started_(started),
          lastImprovementTimeSeconds_(context.searchStartedTimeSeconds),
          lastLiveReport_(started - std::chrono::milliseconds(100)) {}

    SearchResult Execute();

private:
    void Simulate(Branch &branch, std::int64_t targetTimeMs);
    void ObserveTick(Branch &branch);
    void RecordBest(const EvaluationSample &sample,
                    const Branch &branch,
                    const PhysicsSandboxStateView &view,
                    double currentTimeSeconds);
    void FinishAttempt(Branch &branch);
    void FinishLeaf(Branch &branch);
    bool RunTree();
    void Expand(std::size_t segmentIndex, Branch &branch);
    std::optional<MutationWindowPatch> DrawSegment(
            std::size_t segmentIndex,
            const Branch &parent,
            std::uint64_t parentGeneration,
            std::unordered_multimap<std::uint64_t,
                                    std::vector<PhysicsSandboxInputEvent>>
                    &siblings);
    std::uint64_t NextDrawIndex();
    void ReportLive(bool force);
    std::uint64_t ConditionIterations() const {
        return source_ == SearchWinnerSource::Baseline
                ? 0u
                : attemptIndex_ + 1u;
    }

    const SearchExecutionContext &context_;
    PhysicsSandbox &sandbox_;
    const std::uint32_t tick_;
    const TreeSearchSettings settings_;
    const std::chrono::steady_clock::time_point started_;

    EvaluationPlan plan_;
    std::int64_t earliestMutationTimeMs_ = 0;
    std::int64_t lastPatchTimeMs_ = 0;
    std::optional<PhysicsSandboxState> branchState_;
    PhysicsSandboxStateView branchView_;
    std::vector<PhysicsSandboxInputEvent> originalBaselineInputs_;
    std::vector<PhysicsSandboxInputEvent> mutationBaselineInputs_;
    std::vector<MutationTimeRange> segments_;
    std::uint64_t targetLeafCount_ = 1u;

    // Per-tree state.
    std::vector<PhysicsSandboxInputEvent> treeBaseInputs_;
    std::vector<SegmentAllocation> allocations_;
    std::vector<std::uint64_t> branchCounts_;
    bool abandonTree_ = false;
    bool improvedInTree_ = false;

    SearchWinnerSource source_ = SearchWinnerSource::Baseline;
    BestIteration best_;
    std::uint64_t iterations_ = 0u;
    std::uint64_t evaluatorCalls_ = 0u;
    std::uint64_t mutationImprovementCount_ = 0u;
    std::uint64_t totalMutationCount_ = 0u;
    std::optional<std::chrono::steady_clock::duration>
            lastImprovementElapsed_;
    double lastImprovementTimeSeconds_ = 0.0;
    std::chrono::steady_clock::time_point lastLiveReport_;

    std::uint64_t indexStride_ = 1u;
    std::uint64_t attemptIndex_ = 0u;
    std::uint64_t drawCount_ = 0u;
    std::uint64_t drawOffset_ = 0u;
    std::uint64_t generation_ = 0u;
    bool pruneRedundantAnalogInsertions_ = false;
};

std::uint64_t TreeSearchRun::NextDrawIndex() {
    // Random streams stay unique across interleaved CPU workers.
    if (drawCount_ > (std::numeric_limits<std::uint64_t>::max() -
                      drawOffset_) / indexStride_) {
        throw std::overflow_error("tree draw sequence exhausted");
    }
    return drawOffset_ + drawCount_++ * indexStride_;
}

void TreeSearchRun::ReportLive(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastLiveReport_ < std::chrono::milliseconds(100)) {
        return;
    }
    lastLiveReport_ = now;
    const SearchRunControl *control = context_.control;
    if (control == nullptr) {
        return;
    }
    if (control->statisticsChanged) {
        control->statisticsChanged({iterations_, now - started_});
    }
    if (!control->liveChanged || !best_.evaluation) {
        return;
    }
    control->liveChanged({
            best_.source,
            best_.iterationIndex,
            best_.mutationCount,
            best_.evaluation->score,
            best_.evaluation->timeMs,
            best_.evaluation->description,
            best_.view,
            best_.inputs,
            iterations_,
            evaluatorCalls_,
            mutationImprovementCount_,
            totalMutationCount_,
            now - started_,
            lastImprovementElapsed_,
            {},
            best_.evaluation->objectiveScores,
            best_.evaluation->metricValues});
}

void TreeSearchRun::RecordBest(const EvaluationSample &sample,
                               const Branch &branch,
                               const PhysicsSandboxStateView &view,
                               double currentTimeSeconds) {
    best_.evaluation = sample;
    best_.source = source_;
    best_.iterationIndex = source_ == SearchWinnerSource::Mutation
            ? std::optional<std::uint64_t>(attemptIndex_)
            : std::nullopt;
    best_.view = view;
    best_.snapshot = Require(sandbox_.CaptureState(),
                             "capturing improved state");
    best_.inputs = branch.inputs;
    best_.mutationCount = source_ == SearchWinnerSource::Mutation
            ? EffectiveInputChangeCount(originalBaselineInputs_,
                                        best_.inputs)
            : 0u;
    if (source_ == SearchWinnerSource::Mutation) {
        ++mutationImprovementCount_;
        lastImprovementElapsed_ = std::chrono::steady_clock::now() - started_;
        lastImprovementTimeSeconds_ = currentTimeSeconds;
        improvedInTree_ = true;
        ReportLive(true);
    } else {
        ReportLive(false);
    }
}

void TreeSearchRun::ObserveTick(Branch &branch) {
    CheckCancellation(context_.control);
    const PhysicsSandboxStateView previous = branch.state;
    branch.state = Require(sandbox_.AdvanceTicks(1u),
                           "advancing evaluation tick");
    const double currentTimeSeconds = WallClockSeconds();
    const ConditionExecutionContext executionContext{
            ConditionIterations(),
            lastImprovementTimeSeconds_,
            context_.searchStartedTimeSeconds,
            currentTimeSeconds};
    branch.session->SetExecutionContext(executionContext);
    const bool eligible = context_.condition == nullptr ||
            context_.condition->Evaluate(
                    previous, branch.state, executionContext);
    const std::optional<EvaluationSample> sample = eligible
            ? branch.session->Observe(previous, branch.state)
            : std::nullopt;
    if (eligible) ++evaluatorCalls_;
    if (sample) {
        if (!std::isfinite(sample->score) || !std::isfinite(sample->timeMs)) {
            throw std::runtime_error(
                    "iteration evaluator returned a non-finite result");
        }
        if (context_.evaluator.CompareAtEndOnly()) {
            branch.cumulativeSample = sample;
            branch.cumulativeView = branch.state;
        } else if (!best_.evaluation ||
                   ImprovesSearchResult(context_.evaluator,
                                        *sample,
                                        branch.inputs.size(),
                                        *best_.evaluation,
                                        best_.inputs.size())) {
            RecordBest(*sample, branch, branch.state, currentTimeSeconds);
        }
    }
    // A complete session has consumed every sample this attempt can give.
    if (branch.state.raceCompleted || branch.session->IsComplete()) {
        branch.completed = true;
    }
}

void TreeSearchRun::Simulate(Branch &branch, std::int64_t targetTimeMs) {
    while (!branch.completed &&
           static_cast<std::int64_t>(branch.state.timeMs) < targetTimeMs) {
        const std::int64_t currentTimeMs =
                static_cast<std::int64_t>(branch.state.timeMs);
        if (currentTimeMs + tick_ < plan_.startTimeMs) {
            // Ticks before the evaluation window are not observed.
            branch.state = AdvanceTo(
                    sandbox_,
                    currentTimeMs,
                    std::min<std::int64_t>(targetTimeMs,
                                           plan_.startTimeMs - tick_),
                    tick_,
                    context_.control);
            continue;
        }
        ObserveTick(branch);
    }
}

void TreeSearchRun::FinishAttempt(Branch &branch) {
    if (branch.cumulativeSample &&
        (!best_.evaluation ||
         ImprovesSearchResult(context_.evaluator,
                              *branch.cumulativeSample,
                              branch.inputs.size(),
                              *best_.evaluation,
                              best_.inputs.size()))) {
        RecordBest(*branch.cumulativeSample,
                   branch,
                   branch.state,
                   WallClockSeconds());
    }
    const SearchRunControl *control = context_.control;
    if (control != nullptr && control->attemptCompleted) {
        std::optional<SearchLiveUpdate> attempt;
        if (branch.cumulativeSample) {
            attempt.emplace();
            attempt->winnerSource = source_;
            attempt->winningIterationIndex =
                    source_ == SearchWinnerSource::Mutation
                    ? std::optional<std::uint64_t>(attemptIndex_)
                    : std::nullopt;
            attempt->winningMutationCount =
                    source_ == SearchWinnerSource::Mutation
                    ? EffectiveInputChangeCount(treeBaseInputs_,
                                                branch.inputs)
                    : 0u;
            attempt->bestScore = branch.cumulativeSample->score;
            attempt->bestEvaluationTimeMs = branch.cumulativeSample->timeMs;
            attempt->bestEvaluationDescription =
                    branch.cumulativeSample->description;
            attempt->bestState = branch.cumulativeView;
            attempt->bestInputs = branch.inputs;
            attempt->objectiveScores =
                    branch.cumulativeSample->objectiveScores;
            attempt->metricValues = branch.cumulativeSample->metricValues;
        }
        control->attemptCompleted(
                source_ == SearchWinnerSource::Mutation
                        ? std::optional<std::uint64_t>(attemptIndex_)
                        : std::nullopt,
                std::move(attempt));
    }
}

void TreeSearchRun::FinishLeaf(Branch &branch) {
    Simulate(branch, plan_.endTimeMs);
    FinishAttempt(branch);
    ++iterations_;
    totalMutationCount_ +=
            EffectiveInputChangeCount(treeBaseInputs_, branch.inputs);
    if (attemptIndex_ >
        std::numeric_limits<std::uint64_t>::max() - indexStride_) {
        throw std::overflow_error("iteration sequence exhausted");
    }
    attemptIndex_ += indexStride_;
    ReportLive(false);

    if (StopRequested(context_.control) ||
        IterationLimitReached(context_.control, iterations_)) {
        abandonTree_ = true;
        return;
    }
    if (!settings_.autoPromoteBest) {
        return;
    }
    // Promotion restarts from the new best instead of finishing branches
    // that were grown from the previous baseline.
    std::optional<std::vector<PhysicsSandboxInputEvent>> sharedBaseline;
    if (context_.control != nullptr &&
        context_.control->promotedBaselineInputs) {
        sharedBaseline = context_.control->promotedBaselineInputs();
    }
    if (sharedBaseline) {
        if (!SameInputs(*sharedBaseline, treeBaseInputs_)) {
            mutationBaselineInputs_ = std::move(*sharedBaseline);
            abandonTree_ = true;
        }
    } else if (improvedInTree_) {
        mutationBaselineInputs_ = best_.inputs;
        abandonTree_ = true;
    }
}

std::optional<MutationWindowPatch> TreeSearchRun::DrawSegment(
        std::size_t segmentIndex,
        const Branch &parent,
        std::uint64_t parentGeneration,
        std::unordered_multimap<std::uint64_t,
                                std::vector<PhysicsSandboxInputEvent>>
                &siblings) {
    const MutationTimeRange &range = segments_[segmentIndex];
    const MutationSegment segment{range, &allocations_[segmentIndex]};
    // A draw that repeats an earlier sibling would only repeat its leaves.
    constexpr std::size_t maximumDraws = 8u;
    for (std::size_t draw = 0u; draw < maximumDraws; ++draw) {
        MutationResult mutation = context_.mutator.Mutate(
                {parent.inputs,
                 NextDrawIndex(),
                 0u,
                 tick_,
                 range.minimumTimeMs,
                 true,
                 parentGeneration,
                 pruneRedundantAnalogInsertions_,
                 nullptr,
                 &segment});
        MutationWindowPatch patch;
        if (mutation.windowPatch) {
            patch = std::move(*mutation.windowPatch);
        } else {
            patch.minimumTimeMs = range.minimumTimeMs;
            patch.maximumTimeMs = lastPatchTimeMs_;
            for (const PhysicsSandboxInputEvent &event : mutation.inputs) {
                if (event.timeMs >= patch.minimumTimeMs &&
                    event.timeMs <= patch.maximumTimeMs) {
                    patch.events.push_back(event);
                }
            }
        }
        const std::int64_t horizonMs = context_.simulationHorizonMs;
        patch.minimumTimeMs = std::min(patch.minimumTimeMs, horizonMs);
        patch.maximumTimeMs = std::min(patch.maximumTimeMs, horizonMs);
        for (PhysicsSandboxInputEvent &event : patch.events) {
            event.timeMs = static_cast<std::int32_t>(
                    std::min<std::int64_t>(event.timeMs, horizonMs));
        }

        const std::uint64_t hash = HashInputs(patch.events);
        const auto [first, last] = siblings.equal_range(hash);
        const bool duplicate = std::any_of(
                first, last, [&](const auto &entry) {
                    return SameInputs(entry.second, patch.events);
                });
        if (!duplicate) {
            siblings.emplace(hash, patch.events);
            return patch;
        }
    }
    return std::nullopt;
}

void TreeSearchRun::Expand(std::size_t segmentIndex, Branch &branch) {
    if (segmentIndex == segments_.size() || branch.completed) {
        // A finished race cannot be changed by later segments, so the path
        // ends as one attempt.
        FinishLeaf(branch);
        return;
    }
    const MutationTimeRange &range = segments_[segmentIndex];
    const std::uint64_t branchCount = branchCounts_[segmentIndex];
    if (branchCount <= 1u) {
        // Every branch shares this simulation. Unbranched segments with
        // anchored items still apply one draw so each leaf keeps them.
        if (!allocations_[segmentIndex].empty()) {
            std::unordered_multimap<std::uint64_t,
                                    std::vector<PhysicsSandboxInputEvent>>
                    siblings;
            std::optional<MutationWindowPatch> patch = DrawSegment(
                    segmentIndex, branch, ++generation_, siblings);
            Require(sandbox_.ReplaceInputWindow(patch->minimumTimeMs,
                                                patch->maximumTimeMs,
                                                std::move(patch->events)),
                    "replacing tree segment inputs");
            branch.inputs = Require(sandbox_.ReadInputs(),
                                    "reading tree segment inputs");
        }
        Simulate(branch, range.maximumTimeMs);
        Expand(segmentIndex + 1u, branch);
        return;
    }

    const std::int64_t nodeTimeMs =
            static_cast<std::int64_t>(branch.state.timeMs);
    // States every child reaches before its first changed input, ordered
    // by time. Each child simulates the parent's timeline up to that input,
    // so later children resume from the latest such state instead of
    // repeating those ticks. Only unobserved ticks are shared, which keeps
    // every session observing exactly the ticks of a full simulation.
    std::vector<PhysicsSandboxState> prefix;
    prefix.push_back(Require(sandbox_.CaptureState(), "capturing tree node"));
    const Branch parent = branch.Clone();
    // Every draw at this node reads the same parent inputs, so modifiers
    // may reuse what they derived from them.
    const std::uint64_t parentGeneration = ++generation_;
    std::unordered_multimap<std::uint64_t,
                            std::vector<PhysicsSandboxInputEvent>>
            siblings;
    bool atNode = true;
    for (std::uint64_t child = 0u;
         child < branchCount && !abandonTree_;
         ++child) {
        std::optional<MutationWindowPatch> patch =
                DrawSegment(segmentIndex, parent, parentGeneration, siblings);
        if (!patch) continue;
        std::int64_t sharedUntilMs = std::min<std::int64_t>(
                {range.maximumTimeMs,
                 plan_.startTimeMs - tick_,
                 patch->maximumTimeMs - tick_});
        if (const std::optional<std::int64_t> changedMs =
                    FirstPatchedChangeMs(parent.inputs, *patch)) {
            sharedUntilMs = std::min(sharedUntilMs, *changedMs - tick_);
        }
        sharedUntilMs = std::max(sharedUntilMs, nodeTimeMs);
        const auto resume = std::upper_bound(
                prefix.begin() + 1, prefix.end(), sharedUntilMs,
                [](std::int64_t timeMs, const PhysicsSandboxState &state) {
                    return timeMs <
                            static_cast<std::int64_t>(state.View().timeMs);
                }) - 1;
        const std::int64_t resumeTimeMs =
                static_cast<std::int64_t>(resume->View().timeMs);
        // The sandbox still holds the node state until the first child
        // runs; later children restore a shared state.
        Branch current = atNode ? std::move(branch) : parent.Clone();
        if (!atNode) {
            current.state = Require(sandbox_.RestoreState(*resume),
                                    "restoring tree node");
        }
        atNode = false;
        if (resumeTimeMs > nodeTimeMs) {
            // Inputs before the resumed tick already match this child.
            patch->minimumTimeMs = resumeTimeMs + tick_;
            patch->events.erase(
                    patch->events.begin(),
                    std::lower_bound(
                            patch->events.begin(), patch->events.end(),
                            patch->minimumTimeMs,
                            [](const PhysicsSandboxInputEvent &event,
                               std::int64_t timeMs) {
                                return event.timeMs < timeMs;
                            }));
        }
        Require(sandbox_.ReplaceInputWindow(patch->minimumTimeMs,
                                            patch->maximumTimeMs,
                                            std::move(patch->events)),
                "replacing tree segment inputs");
        current.inputs = Require(sandbox_.ReadInputs(),
                                 "reading tree segment inputs");
        if (sharedUntilMs > resumeTimeMs) {
            Simulate(current, sharedUntilMs);
            prefix.insert(resume + 1,
                          Require(sandbox_.CaptureState(),
                                  "capturing shared tree state"));
        }
        Simulate(current, range.maximumTimeMs);
        Expand(segmentIndex + 1u, current);
    }
}

bool TreeSearchRun::RunTree() {
    treeBaseInputs_ = mutationBaselineInputs_;
    abandonTree_ = false;
    improvedInTree_ = false;

    // One ordinary candidate fixes how many items each pass anchors in
    // every segment.
    std::vector<MutationAnchor> anchors;
    static_cast<void>(context_.mutator.Mutate(
            {treeBaseInputs_,
             NextDrawIndex(),
             0u,
             tick_,
             earliestMutationTimeMs_,
             false,
             ++generation_,
             pruneRedundantAnalogInsertions_,
             &anchors,
             nullptr}));
    allocations_.assign(segments_.size(), {});
    for (const MutationAnchor &anchor : anchors) {
        // Items after the last observed tick cannot change any attempt.
        if (anchor.timeMs < segments_.front().minimumTimeMs ||
            anchor.timeMs > segments_.back().maximumTimeMs) {
            continue;
        }
        const auto segment = std::upper_bound(
                segments_.begin(), segments_.end(), anchor.timeMs,
                [](std::int64_t timeMs, const MutationTimeRange &range) {
                    return timeMs < range.minimumTimeMs;
                }) - 1;
        SegmentAllocation &allocation = allocations_[
                static_cast<std::size_t>(segment - segments_.begin())];
        if (allocation.size() <= anchor.passIndex) {
            allocation.resize(anchor.passIndex + 1u);
        }
        std::vector<std::uint32_t> &slots = allocation[anchor.passIndex];
        if (slots.size() <= anchor.slot) {
            slots.resize(anchor.slot + 1u, 0u);
        }
        ++slots[anchor.slot];
    }
    std::vector<std::size_t> branchedSegments;
    for (std::size_t index = 0u; index < segments_.size(); ++index) {
        if (!allocations_[index].empty()) branchedSegments.push_back(index);
    }
    if (branchedSegments.empty()) {
        // The drawn candidate changes nothing the target can observe.
        return false;
    }
    std::size_t branchedCount = settings_.branchedSegmentCount == 0u
            ? branchedSegments.size()
            : std::min<std::size_t>(branchedSegments.size(),
                                    settings_.branchedSegmentCount);
    if (branchedCount < branchedSegments.size() ||
        settings_.varyBranchedSegmentCount) {
        // A random subset of the active segments branches; the others
        // apply one shared draw or keep the base inputs. The anchors seed
        // the choice, so it follows the modifier seeds.
        std::uint64_t random = MixBits(drawCount_ ^ drawOffset_);
        for (const MutationAnchor &anchor : anchors) {
            random = MixBits(random ^ static_cast<std::uint64_t>(
                                              anchor.timeMs) ^
                             (std::uint64_t{anchor.passIndex} << 32u) ^
                             (std::uint64_t{anchor.slot} << 48u));
        }
        if (settings_.varyBranchedSegmentCount) {
            random = MixBits(random);
            branchedCount = 1u + random % branchedCount;
        }
        for (std::size_t index = 0u; index < branchedCount; ++index) {
            random = MixBits(random);
            std::swap(branchedSegments[index],
                      branchedSegments[index +
                                       random % (branchedSegments.size() -
                                                 index)]);
        }
        if (settings_.keepUnbranchedSegments) {
            for (std::size_t index = branchedCount;
                 index < branchedSegments.size(); ++index) {
                allocations_[branchedSegments[index]].clear();
            }
        }
        branchedSegments.resize(branchedCount);
        std::sort(branchedSegments.begin(), branchedSegments.end());
    }
    const std::vector<std::uint64_t> activeBranchCounts =
            TreeBranchCounts(branchedSegments.size(), targetLeafCount_);
    branchCounts_.assign(segments_.size(), 1u);
    for (std::size_t index = 0u; index < branchedSegments.size(); ++index) {
        branchCounts_[branchedSegments[index]] = activeBranchCounts[index];
    }

    Branch root;
    root.session = context_.evaluator.CreateSession();
    root.state = Require(sandbox_.RestoreState(*branchState_),
                         "restoring tree root");
    root.inputs = treeBaseInputs_;
    if (!SameInputs(treeBaseInputs_, originalBaselineInputs_)) {
        Require(sandbox_.ReplaceInputs(treeBaseInputs_),
                "applying promoted tree baseline");
        root.inputs = Require(sandbox_.ReadInputs(),
                              "reading promoted tree baseline");
    }
    Simulate(root, segments_.front().minimumTimeMs - tick_);
    Expand(0u, root);
    return true;
}

SearchResult TreeSearchRun::Execute() {
    const SearchRunControl *control = context_.control;
    earliestMutationTimeMs_ = std::min<std::int64_t>(
            context_.mutator.EarliestMutationTimeMs(),
            context_.simulationHorizonMs);
    if (earliestMutationTimeMs_ < static_cast<std::int64_t>(tick_) ||
        earliestMutationTimeMs_ % tick_ != 0) {
        throw std::invalid_argument(
                "modifier pipeline must begin on or after the first whole "
                "tick");
    }
    CheckCancellation(control);
    PhysicsSandboxStateView current = Require(
            sandbox_.ReadState(), "reading initial sandbox state");
    plan_ = context_.evaluator.Plan(
            context_.simulationHorizonMs, earliestMutationTimeMs_, tick_);
    if (control != nullptr && control->evaluationEndTimeLimitMs) {
        plan_.endTimeMs = std::min(plan_.endTimeMs,
                                   *control->evaluationEndTimeLimitMs);
    }
    if (plan_.startTimeMs < tick_ ||
        plan_.endTimeMs < plan_.startTimeMs ||
        plan_.endTimeMs > context_.simulationHorizonMs ||
        plan_.startTimeMs % tick_ != 0 ||
        plan_.endTimeMs % tick_ != 0) {
        throw std::invalid_argument(
                "evaluation target returned an invalid observation plan: "
                "mutation=" +
                std::to_string(earliestMutationTimeMs_) +
                " start=" + std::to_string(plan_.startTimeMs) +
                " end=" + std::to_string(plan_.endTimeMs) +
                " Simulation horizon=" +
                std::to_string(context_.simulationHorizonMs));
    }
    lastPatchTimeMs_ = std::min<std::int64_t>(
            context_.mutator.AffectedTimeRange().maximumTimeMs,
            context_.simulationHorizonMs);
    segments_ = TreeSegmentRanges(
            earliestMutationTimeMs_,
            std::min(lastPatchTimeMs_, plan_.endTimeMs),
            settings_.segmentCount,
            tick_);
    if (segments_.empty()) {
        throw std::invalid_argument(
                "tree search needs mutable inputs inside the evaluation "
                "window; the window ends before the first mutable input");
    }
    targetLeafCount_ = settings_.leafCount != 0u
            ? settings_.leafCount
            : std::uint64_t{1} << settings_.segmentCount;

    const std::int64_t branchTimeMs =
            std::min(earliestMutationTimeMs_, plan_.startTimeMs) - tick_;
    current = AdvanceTo(sandbox_, current.timeMs, branchTimeMs, tick_,
                        control);
    originalBaselineInputs_ = Require(sandbox_.ReadInputs(),
                                      "reading baseline inputs");
    branchState_ = Require(sandbox_.CaptureState(), "capturing branch state");
    branchView_ = current;
    // Same rule as BasicBruteForceSearch: without keyboard steering or stunt
    // scoring, analog insertion timestamps are unobservable.
    pruneRedundantAnalogInsertions_ = !current.stuntsScore &&
            std::none_of(originalBaselineInputs_.begin(),
                         originalBaselineInputs_.end(),
                         [](const PhysicsSandboxInputEvent &event) {
                             return event.action ==
                                            PhysicsSandboxInputAction::
                                                    SteerLeft ||
                                     event.action ==
                                             PhysicsSandboxInputAction::
                                                     SteerRight;
                         });

    ReportProgress(control, SearchProgressStage::Baseline, 0u);
    {
        Branch baseline;
        baseline.session = context_.evaluator.CreateSession();
        baseline.state = branchView_;
        baseline.inputs = originalBaselineInputs_;
        treeBaseInputs_ = originalBaselineInputs_;
        Simulate(baseline, plan_.endTimeMs);
        FinishAttempt(baseline);
    }
    ReportLive(true);
    ReportProgress(control, SearchProgressStage::Mutations, 0u);

    if (control != nullptr && control->iterationIndexStride == 0u) {
        throw std::invalid_argument(
                "iteration index stride must be greater than zero");
    }
    indexStride_ = control == nullptr ? 1u : control->iterationIndexStride;
    attemptIndex_ = control == nullptr ? 0u : control->iterationIndexOffset;
    drawOffset_ = attemptIndex_;
    source_ = SearchWinnerSource::Mutation;
    mutationBaselineInputs_ = originalBaselineInputs_;
    while (!StopRequested(control) &&
           !IterationLimitReached(control, iterations_)) {
        BeginIteration(control);
        static_cast<void>(RunTree());
    }

    CheckCancellation(control);
    ReportLive(true);
    if (!best_.evaluation || !best_.snapshot) {
        throw std::runtime_error(
                "no iteration satisfied the selected evaluation target");
    }
    const PhysicsSandboxStateView restored = Require(
            sandbox_.RestoreState(*best_.snapshot),
            "restoring global best state");
    if (!SameState(restored, best_.view)) {
        throw std::runtime_error(
                "restored global best does not match its captured state");
    }
    const bool mutationWon = best_.source == SearchWinnerSource::Mutation;
    if (mutationWon != (mutationImprovementCount_ > 0u)) {
        throw std::runtime_error(
                "mutation winner and improvement count are inconsistent");
    }
    return SearchResult{
            best_.source,
            best_.iterationIndex,
            best_.mutationCount,
            best_.evaluation->score,
            best_.evaluation->timeMs,
            best_.evaluation->description,
            best_.view,
            std::move(best_.inputs),
            {},
            iterations_,
            evaluatorCalls_,
            mutationImprovementCount_,
            totalMutationCount_,
            std::chrono::steady_clock::now() - started_,
            lastImprovementElapsed_,
            *best_.snapshot,
            best_.evaluation->objectiveScores,
            best_.evaluation->metricValues};
}

}  // namespace

OptionSettings DefaultTreeSearchOptionSettings() {
    return {{"segmentCount", "10"},
            {"leafCount", "0"},
            {"branchedSegmentCount", "0"},
            {"varyBranchedSegmentCount", "false"},
            {"unbranchedSegments", "draw"},
            {"flatWorkerCount", "0"},
            {"autoPromoteBest", "false"}};
}

std::optional<TreeSearchSettings> ParseTreeSearchSettings(
        const OptionSettings &settings) {
    if (ValidateOptionSettingKeys(settings,
                                  DefaultTreeSearchOptionSettings())) {
        return std::nullopt;
    }
    return ParseTreeSettings(settings);
}

std::optional<std::string> ValidateTreeSearchOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultTreeSearchOptionSettings())) {
        return keyError;
    }
    if (!ParseBoolean(settings.at("autoPromoteBest"))) {
        return "auto-promote best must be true or false";
    }
    const auto segmentCount =
            ParseUnsignedDecimal32(settings.at("segmentCount"));
    if (!segmentCount || *segmentCount < kMinimumTreeSegmentCount ||
        *segmentCount > kMaximumTreeSegmentCount) {
        return "segment count must be a whole number between " +
                std::to_string(kMinimumTreeSegmentCount) + " and " +
                std::to_string(kMaximumTreeSegmentCount);
    }
    const auto leafCount = ParseUnsignedDecimal32(settings.at("leafCount"));
    if (!leafCount || *leafCount > kMaximumTreeLeafCount) {
        return "leaves per tree must be 0 (automatic) or a whole number up "
               "to " + std::to_string(kMaximumTreeLeafCount);
    }
    const auto branchedSegmentCount =
            ParseUnsignedDecimal32(settings.at("branchedSegmentCount"));
    if (!branchedSegmentCount || *branchedSegmentCount > *segmentCount) {
        return "branched segments must be 0 (all) or a whole number up to "
               "the segment count";
    }
    if (!ParseBoolean(settings.at("varyBranchedSegmentCount"))) {
        return "varying branched segments must be true or false";
    }
    const std::string &unbranched = settings.at("unbranchedSegments");
    if (unbranched != "draw" && unbranched != "keep") {
        return "unbranched segments must be draw or keep";
    }
    const auto flatWorkerCount =
            ParseUnsignedDecimal32(settings.at("flatWorkerCount"));
    if (!flatWorkerCount ||
        *flatWorkerCount > kMaximumTreeFlatWorkerCount) {
        return "flat workers must be a whole number up to " +
                std::to_string(kMaximumTreeFlatWorkerCount);
    }
    if (tickDurationMs == 0u) {
        return "tick duration must be greater than zero";
    }
    return std::nullopt;
}

std::unique_ptr<SearchAlgorithm> CreateTreeSearch(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidateTreeSearchOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<TreeSearch>(*ParseTreeSettings(settings));
}

std::vector<MutationTimeRange> TreeSegmentRanges(
        std::int64_t firstInputTimeMs,
        std::int64_t lastInputTimeMs,
        std::uint32_t segmentCount,
        std::uint32_t tickDurationMs) {
    if (tickDurationMs == 0u || segmentCount == 0u ||
        lastInputTimeMs < firstInputTimeMs) {
        return {};
    }
    const std::int64_t tick = tickDurationMs;
    const std::int64_t windowTicks =
            (lastInputTimeMs - firstInputTimeMs) / tick + 1;
    const std::int64_t count =
            std::min<std::int64_t>(segmentCount, windowTicks);
    std::vector<MutationTimeRange> ranges;
    ranges.reserve(static_cast<std::size_t>(count));
    for (std::int64_t index = 0; index < count; ++index) {
        const std::int64_t begin =
                firstInputTimeMs + windowTicks * index / count * tick;
        const std::int64_t end =
                firstInputTimeMs + windowTicks * (index + 1) / count * tick;
        ranges.push_back({begin, end - tick});
    }
    return ranges;
}

std::vector<std::uint64_t> TreeBranchCounts(
        std::size_t activeSegmentCount,
        std::uint64_t targetLeafCount) {
    if (activeSegmentCount == 0u) {
        return {};
    }
    const auto power = [&](std::uint64_t base) {
        std::uint64_t product = 1u;
        for (std::size_t index = 0u; index < activeSegmentCount; ++index) {
            if (product > targetLeafCount / base) {
                return targetLeafCount + 1u;
            }
            product *= base;
        }
        return product;
    };
    std::uint64_t base = std::max<std::uint64_t>(
            1u,
            static_cast<std::uint64_t>(std::floor(std::pow(
                    static_cast<double>(targetLeafCount),
                    1.0 / static_cast<double>(activeSegmentCount)))));
    while (base > 1u && power(base) > targetLeafCount) --base;
    while (power(base + 1u) <= targetLeafCount) ++base;

    std::vector<std::uint64_t> counts(activeSegmentCount, base);
    std::uint64_t product = power(base);
    for (std::size_t index = activeSegmentCount; index-- > 0u;) {
        const std::uint64_t widened = product / base * (base + 1u);
        if (widened > targetLeafCount) break;
        counts[index] = base + 1u;
        product = widened;
    }
    return counts;
}

TreeSearch::TreeSearch(std::uint32_t segmentCount, bool autoPromoteBest)
    : settings_{segmentCount, 0u, 0u, false, false, 0u, autoPromoteBest} {}

TreeSearch::TreeSearch(TreeSearchSettings settings) : settings_(settings) {}

SearchResult TreeSearch::Run(const SearchExecutionContext &context) const {
    const auto started = std::chrono::steady_clock::now();
    if (context.tickDurationMs == 0u) {
        throw std::invalid_argument(
                "tick duration must be greater than zero");
    }
    if (IsGpuSimulationBackend(context.sandbox.Backend())) {
        throw std::invalid_argument(
                "tree search requires a CPU physics backend");
    }
    TreeSearchRun run(context, settings_, started);
    return run.Execute();
}

}  // namespace forevertas
