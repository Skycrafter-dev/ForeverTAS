#ifndef FOREVERTAS_APP_REMOTE_GROUP_SEARCH_H
#define FOREVERTAS_APP_REMOTE_GROUP_SEARCH_H

// A lent machine's part of a group's search (docs/REMOTE.md, "Groups"): each thing it lends (a GPU, its CPU threads) runs the search on its
// own, as a ForeverTAS started there would, from its own range of candidates.
// While it searches it says nothing; it shares its best only when that beats
// the search's overall best it knows, at most once a waiting period (the
// latest at the period's end); with promotion on it takes a better overall
// best as its baseline. At the end (stop, or moved to another search) it hands
// in its count, and its best if that still beats the overall one.

#include "searches/search_runner.h"

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>


namespace forevertas::app::remote {

class GroupSearch {
public:
    using SearchRunner = std::function<SearchResult(const SearchRequest &, const SearchRunControl *)>;
    using Send = std::function<void(const QJsonObject &)>;

    // job: the relay's {search, slot, cooldownMs, best, job: {request, replayName, replay, ...}}; lends: this
    // machine's {gpus, cpu}. Throws when the job cannot be read.
    // (finished: called, on a thread of its own, once it has handed in its count)
    GroupSearch(const QJsonObject &job, const QString &packDirectory, const QJsonObject &lends, SearchRunner run,
                Send send, std::function<void()> finished = {});
    ~GroupSearch();   // cancels what still runs

    QString search() const { return search_; }
    // a better overall best of the search (its sample, as the machine that found it reported it)
    void overall(const QJsonObject &best);
    void cooldown(int milliseconds);
    // the search ends here: the final count follows
    void stop();
    bool finished() const { return finished_.load(); }

private:
    struct Part;
    void share();     // the sharing thread: a best each period, the speed once
    void supervise(); // waits for every part, then the final count
    bool beatsOverall(const SearchLiveUpdate &live);

    QString search_;
    QString folder_;   // its replay's (removed at the end)
    std::optional<SearchRequest> request_;
    Send send_;
    std::function<void()> finishedCallback_;
    std::function<bool(const SearchLiveUpdate &, const SearchLiveUpdate &)> improves_;
    bool promote_ = true;
    std::vector<std::unique_ptr<Part>> parts_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    std::mutex lock_;
    std::condition_variable wake_;
    std::optional<SearchLiveUpdate> best_;     // this machine's (the best of its parts)
    std::optional<SearchLiveUpdate> overall_;  // the search's, as last heard
    std::optional<std::vector<SandboxInputEvent>> lastSent_;
    std::chrono::milliseconds cooldown_{1000};
    bool cooldownChanged_ = false;
    std::atomic_bool stop_{false};
    std::atomic_bool cancel_{false};
    std::atomic_bool finished_{false};
    std::thread sharer_;
    std::thread supervisor_;
};

}  // namespace forevertas::app::remote

#endif
