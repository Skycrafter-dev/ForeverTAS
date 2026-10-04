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

OptionSettings DefaultTreeSearchOptionSettings();
std::optional<std::string> ValidateTreeSearchOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<SearchAlgorithm> CreateTreeSearch(
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
// attempt. CPU physics backends only.
class TreeSearch final : public SearchAlgorithm {
public:
    TreeSearch(std::uint32_t segmentCount, bool autoPromoteBest);

    SearchResult Run(const SearchExecutionContext &context) const override;

private:
    std::uint32_t segmentCount_ = 10u;
    bool autoPromoteBest_ = false;
};

}  // namespace forevertas

#endif
