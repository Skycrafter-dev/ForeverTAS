#ifndef FOREVERTAS_APP_SEARCH_COMPLETION_H
#define FOREVERTAS_APP_SEARCH_COMPLETION_H

#include "searches/search_algorithm.h"

#include <QMetaType>
#include <QString>

#include <memory>
#include <chrono>
#include <vector>

namespace forevertas::app {

struct SearchCompletion {
    std::uint64_t searchId = 0u;
    QString summary;
    QString inputsText;
    QString packsDirectory;
    QString replayPath;
    QString simulationBackendId;
    std::vector<SandboxInputEvent> bestInputs;
    std::vector<SearchTimelineFrame> bestTimeline;
};

using SearchCompletionPtr = std::shared_ptr<const SearchCompletion>;

struct SearchImprovement {
    std::chrono::steady_clock::time_point generatedAt = std::chrono::steady_clock::now();
    std::uint64_t searchId = 0u;
    std::uint64_t improvementNumber = 0u;
    QString packsDirectory;
    QString replayPath;
    QString simulationBackendId;
    std::vector<SearchTimelineFrame> timeline;
    std::vector<SandboxInputEvent> inputs;
};

using SearchImprovementPtr = std::shared_ptr<const SearchImprovement>;

}  // namespace forevertas::app

Q_DECLARE_METATYPE(forevertas::app::SearchCompletionPtr)
Q_DECLARE_METATYPE(forevertas::app::SearchImprovementPtr)

#endif
