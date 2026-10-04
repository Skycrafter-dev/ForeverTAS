#ifndef FOREVERTAS_SEARCHES_TREE_SEARCH_H
#define FOREVERTAS_SEARCHES_TREE_SEARCH_H

#include "mutations/input_mutator.h"
#include "searches/option_configuration.h"
#include "searches/search_algorithm.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forevertas {

inline constexpr std::uint32_t kMinimumTreeSegmentCount = 1u;
inline constexpr std::uint32_t kMaximumTreeSegmentCount = 20u;
inline constexpr std::uint32_t kMaximumTreeLeafCount = 1u << 20u;
inline constexpr std::uint32_t kMaximumTreeFlatWorkerCount = 256u;

enum class TreeSegmentChoice : std::uint8_t {
    Random,
    // Favours segments whose trees improved (upper confidence bound).
    Bandit,
    // Favours later segments, whose branches are cheaper to simulate.
    Late,
};

struct TreeSearchSettings {
    std::uint32_t segmentCount = 10u;
    // Leaves each tree targets; 0 means 2^segmentCount.
    std::uint32_t leafCount = 0u;
    // Active segments each tree branches, chosen at random per tree; the
    // other active segments apply one shared draw. 0 branches all of them.
    std::uint32_t branchedSegmentCount = 0u;
    // Each tree branches a random number of segments from one up to that
    // limit instead, mixing small and large changes.
    bool varyBranchedSegmentCount = false;
    // Unbranched active segments keep the tree's base inputs instead, so
    // every leaf changes only its branched segments.
    bool keepUnbranchedSegments = false;
    // Multi-threaded CPU workers that run basic bruteforce attempts instead
    // of trees while sharing the promoted best (a portfolio). Other backends
    // ignore it.
    std::uint32_t flatWorkerCount = 0u;
    bool autoPromoteBest = false;
    // A varied branched-segment count halves its odds for every extra
    // segment instead of being uniform.
    bool geometricBranchedSegmentCount = false;
    // Adaptive intensity: trees branch one segment, doubling the count after
    // this many trees without improvement and resetting after one. 0 is off.
    // Only the adaptive escalation search sets it.
    std::uint32_t escalateAfterTrees = 0u;
    // Share of the drawn candidate's anchored items a tree keeps.
    std::uint32_t itemKeepPercent = 100u;
    // How a tree picks the segments it branches.
    TreeSegmentChoice segmentChoice = TreeSegmentChoice::Random;
    // Multi-threaded workers follow their own improvements for this many
    // seconds before adopting the shared best. 0 adopts it immediately.
    std::uint32_t migrationSeconds = 0u;
    // After an improving tree, this many trees branch the same segments
    // again, refining where the last gain came from. 0 is off.
    std::uint32_t repeatImprovedSegments = 0u;
};

OptionSettings DefaultTreeSearchOptionSettings();
// Parses settings that ValidateTreeSearchOptionSettings accepts.
std::optional<TreeSearchSettings> ParseTreeSearchSettings(
        const OptionSettings &settings);
std::optional<std::string> ValidateTreeSearchOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<SearchAlgorithm> CreateTreeSearch(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

inline constexpr std::uint32_t kMaximumEscalateAfterTrees = 1000000u;

// Adaptive escalation: a tree search that changes one segment per attempt
// and doubles the number of changed segments after escalateAfterTrees trees
// without improvement, resetting after one.
OptionSettings DefaultAdaptiveEscalationOptionSettings();
std::optional<TreeSearchSettings> ParseAdaptiveEscalationSettings(
        const OptionSettings &settings);
std::optional<std::string> ValidateAdaptiveEscalationOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<SearchAlgorithm> CreateAdaptiveEscalationSearch(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

// Splits the inclusive input range into at most segmentCount whole-tick
// segments whose lengths differ by at most one tick.
std::vector<MutationTimeRange> TreeSegmentRanges(
        std::int64_t firstInputTimeMs,
        std::int64_t lastInputTimeMs,
        std::uint32_t segmentCount,
        std::uint32_t tickDurationMs);

// Branches per active segment. Their product is the largest achievable
// value not above targetLeafCount; later segments receive any extra branch
// because their simulations are the cheapest to repeat.
std::vector<std::uint64_t> TreeBranchCounts(
        std::size_t activeSegmentCount,
        std::uint64_t targetLeafCount);

// Grows trees of candidates that share every simulated prefix. Each tree
// draws one ordinary candidate to fix how many items every modifier pass
// anchors in each segment; every branch then redraws only the items of its
// segment. Leaves keep ordinary candidate statistics, and each leaf is one
// attempt. CUDA/HIP use resident batches of random local segment mutations
// around the current best instead of CPU prefix-sharing trees.
class TreeSearch final : public SearchAlgorithm {
public:
    TreeSearch(std::uint32_t segmentCount, bool autoPromoteBest);
    explicit TreeSearch(TreeSearchSettings settings);

    SearchResult Run(const SearchExecutionContext &context) const override;

private:
    TreeSearchSettings settings_;
};

}  // namespace forevertas

#endif
