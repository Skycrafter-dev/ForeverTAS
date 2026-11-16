#include "app/remote/group_search.h"

#include "app/remote/remote_codec.h"
#include "app/search_worker.h"
#include "diagnostics/diagnostics_log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <cstdio>
#include <iostream>
#include <random>

namespace forevertas::app::remote {

namespace {

// Each lent thing's range of candidates: far from the others' and from the searching window's own devices (which
// number theirs from 0).
constexpr unsigned kRangeBits = 44u;
constexpr unsigned kPartsPerMachine = 4u;

void Say(std::chrono::steady_clock::time_point started, const std::string &text) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "[%7.1f s] ", seconds);
    std::cout << stamp << text << std::endl;
}

}  // namespace

struct GroupSearch::Part {
    std::string name;
    std::optional<SearchRequest> request;
    SearchRunControl control;
    std::thread thread;
    std::atomic<std::uint64_t> iterations{0u};
    std::optional<SearchLiveUpdate> best;   // (under the search's lock)
    std::optional<std::vector<SandboxInputEvent>> adopted;   // the search's best it took last
    std::string failure;
    bool cancelled = false;
};

GroupSearch::GroupSearch(const QJsonObject &message, const QString &packDirectory, const QJsonObject &lends,
                         SearchRunner run, Send send, std::function<void()> finished)
    : search_(message.value("search").toString()), send_(std::move(send)), finishedCallback_(std::move(finished)) {
    const QJsonObject job = message.value("job").toObject();
    // a folder of its own under the temporary one (not a QTemporaryDir: on Windows its folder is the user's alone,
    // which a lender in an AppContainer may not write in)
    folder_ = QDir(QDir::tempPath()).filePath(QStringLiteral("ForeverTAS-group-%1")
                                                      .arg(QString::number(std::random_device{}(), 16)));
    const QString replayPath = QDir(folder_).filePath(QFileInfo(job.value("replayName").toString()).fileName());
    QFile replay(replayPath);
    if (!QDir().mkpath(folder_) || !replay.open(QIODevice::WriteOnly) ||
        replay.write(QByteArray::fromBase64(job.value("replay").toString().toLatin1())) < 0) {
        throw std::runtime_error("this machine cannot write the replay (" + replayPath.toStdString() + ": " +
                                 replay.errorString().toStdString() + ")");
    }
    replay.close();
    request_.emplace(DecodeRequest(job.value("request").toObject(), packDirectory.toStdString(), replayPath.toStdString()));
    improves_ = LiveImproves(*request_);
    const auto promoting = request_->searchAlgorithm.settings.find("autoPromoteBest");
    promote_ = promoting == request_->searchAlgorithm.settings.end() || promoting->second == "true";
    cooldown_ = std::chrono::milliseconds(std::max(1000, message.value("cooldownMs").toInt(1000)));
    if (message.value("best").isObject()) overall(message.value("best").toObject());
    const std::uint64_t slot = static_cast<std::uint64_t>(message.value("slot").toDouble());

    // what this machine lends, each searching on its own
    std::vector<std::pair<PhysicsBackend, std::uint32_t>> lent;
    for (const QJsonValue &gpu : lends.value("gpus").toArray()) {
        if (const auto backend = ParsePhysicsBackend(gpu.toObject().value("backend").toString().toStdString()))
            lent.emplace_back(*backend, 0u);
    }
    const int threads = lends.value("cpu").toObject().value("threads").toInt();
    if (threads > 0)
        lent.emplace_back(threads > 1 ? PhysicsBackend::MultiThreadedCpu : PhysicsBackend::OptimizedCpu,
                          static_cast<std::uint32_t>(threads));
    if (lent.empty()) throw std::runtime_error("this machine lends nothing");
    for (std::size_t index = 0; index < lent.size() && index < kPartsPerMachine; ++index) {
        auto part = std::make_unique<Part>();
        Part *const p = part.get();
        p->request = request_;
        p->request->backend = lent[index].first;
        p->request->remoteDevice.clear();
        if (lent[index].second > 0u) {
            p->request->parallelSampleCount = lent[index].second;
            p->request->calibrateCudaParallelSampleCount = false;
            p->name = std::to_string(lent[index].second) + " CPU threads";
        } else {
            p->request->parallelSampleCount = 1u;
            p->request->calibrateCudaParallelSampleCount = true;
            p->name = std::string(PhysicsBackendId(lent[index].first));
        }
        SearchRunControl &control = p->control;
        control.iterationIndexOffset = (slot * kPartsPerMachine + index + 1u) << kRangeBits;
        control.iterationIndexStride = 1u;
        control.sampleImprovementTimelines = false;
        control.sampleBestTimeline = false;
        control.reuseLoadedSandbox = false;
        control.stopRequested = [this] { return stop_.load() || cancel_.load(); };
        control.cancellationRequested = [this] { return cancel_.load(); };
        control.beginIteration = [this] { return !stop_.load() && !cancel_.load(); };
        control.statisticsChanged = [p](const SearchStatisticsUpdate &statistics) { p->iterations = statistics.iterations; };
        control.liveChanged = [this, p](const SearchLiveUpdate &live) {
            std::lock_guard<std::mutex> guard(lock_);
            if (!p->best || improves_(live, *p->best)) p->best = live;
            if (!best_ || improves_(live, *best_)) best_ = live;
        };
        // with promotion: a better overall best of the search becomes this part's baseline
        control.promotedBaselineInputs = [this, p]() -> std::optional<std::vector<SandboxInputEvent>> {
            std::lock_guard<std::mutex> guard(lock_);
            if (!promote_ || !overall_) return std::nullopt;
            if (p->best && !improves_(*overall_, *p->best)) return std::nullopt;
            if (!p->adopted || EncodeEvents(*p->adopted) != EncodeEvents(overall_->bestInputs)) {
                p->adopted = overall_->bestInputs;
                Say(started_, p->name + ": takes the search's best (" + std::to_string(overall_->bestScore) + ") as its baseline");
            }
            return overall_->bestInputs;
        };
        const std::string name = p->name;
        const std::string backend(PhysicsBackendId(p->request->backend));
        control.progressChanged = [this, name, backend, last = -1](const SearchProgress &progress) mutable {
            const int stage = static_cast<int>(progress.stage);
            if (stage == last) return;
            last = stage;
            Say(started_, name + ": " + SearchStageStatus(progress.stage, backend).toStdString());
        };
        parts_.push_back(std::move(part));
    }
    Say(started_, "a group's search began on " + std::to_string(parts_.size()) + " part(s) of this machine");
    for (auto &part : parts_) {
        Part *const p = part.get();
        p->thread = std::thread([this, p, run] {
            try {
                (void)run(*p->request, &p->control);
            } catch (const SearchCancelled &) {
                p->cancelled = true;
            } catch (const std::exception &error) {
                p->failure = error.what();
                Say(started_, p->name + " failed: " + p->failure);
            }
        });
    }
    sharer_ = std::thread([this] { share(); });
    supervisor_ = std::thread([this] { supervise(); });
}

GroupSearch::~GroupSearch() {
    cancel_ = true;
    stop_ = true;
    wake_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();
    QDir(folder_).removeRecursively();
}

bool GroupSearch::beatsOverall(const SearchLiveUpdate &live) {
    return !overall_ || improves_(live, *overall_);
}

void GroupSearch::overall(const QJsonObject &best) {
    const QJsonObject sample = best.value("sample").toObject();
    if (sample.isEmpty()) return;
    SearchLiveUpdate live = DecodeLive(sample);
    std::lock_guard<std::mutex> guard(lock_);
    if (!improves_) return;
    if (!overall_ || improves_(live, *overall_)) overall_ = std::move(live);
}

void GroupSearch::cooldown(int milliseconds) {
    std::lock_guard<std::mutex> guard(lock_);
    cooldown_ = std::chrono::milliseconds(std::max(1000, milliseconds));
    cooldownChanged_ = true;
    wake_.notify_all();
}

void GroupSearch::stop() {
    Say(started_, "told to stop");
    stop_ = true;
    wake_.notify_all();
}

// One send a period, at most: the latest best, if it beats the overall one and was not sent already; the speed
// once, with the first slot after warming up. Each machine's slots start at a random offset in its period.
void GroupSearch::share() {
    std::mt19937_64 random{std::random_device{}()};
    const auto pick = [&](std::chrono::milliseconds period) {
        return std::chrono::milliseconds(std::uniform_int_distribution<long long>(0, period.count())(random));
    };
    std::unique_lock<std::mutex> guard(lock_);
    auto next = std::chrono::steady_clock::now() + pick(cooldown_);
    // its speed: told once warmed up, then again only when it doubles or halves (a GPU calibrating, a CPU part
    // starting later): a few times a search
    double reported = 0.0;
    std::uint64_t lastIterations = 0u;
    auto lastTime = started_;
    while (!stop_.load()) {
        wake_.wait_until(guard, next, [&] { return stop_.load() || cooldownChanged_; });
        if (stop_.load()) break;
        if (cooldownChanged_) {
            cooldownChanged_ = false;
            next = std::chrono::steady_clock::now() + pick(cooldown_);
            continue;
        }
        next += cooldown_;
        const auto now = std::chrono::steady_clock::now();
        std::uint64_t iterations = 0u;
        for (const auto &part : parts_) iterations += part->iterations.load();
        const double seconds = std::chrono::duration<double>(now - lastTime).count();
        if (now - started_ >= std::chrono::seconds(3) && seconds >= 1.0) {
            const double speed = static_cast<double>(iterations - std::min(iterations, lastIterations)) / seconds;
            lastIterations = iterations;
            lastTime = now;
            if (speed > 0.0 && (reported == 0.0 || speed >= 2.0 * reported || speed <= 0.5 * reported)) {
                reported = speed;
                Say(started_, "its speed: " + std::to_string(static_cast<long long>(speed)) + " attempts a second");
                send_(QJsonObject{{"type", "speed"}, {"search", search_}, {"perSecond", speed}});
            }
        }
        if (best_ && beatsOverall(*best_) && (!lastSent_ || EncodeEvents(*lastSent_) != EncodeEvents(best_->bestInputs))) {
            lastSent_ = best_->bestInputs;
            send_(QJsonObject{{"type", "best"}, {"search", search_}, {"live", EncodeLive(*best_)}});
        }
    }
}

// Every part done (stopped, moved, failed): the count, and the best if it still beats the overall one.
void GroupSearch::supervise() {
    for (auto &part : parts_) part->thread.join();
    stop_ = true;
    wake_.notify_all();
    if (sharer_.joinable()) sharer_.join();
    std::uint64_t iterations = 0u;
    std::string failure;
    bool anyRan = false;
    for (const auto &part : parts_) {
        iterations += part->iterations.load();
        if (part->failure.empty()) anyRan = true;
        else failure = part->failure;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    if (!cancel_.load()) {
        QJsonObject final{{"search", search_}, {"iterations", static_cast<double>(iterations)}, {"seconds", seconds}};
        if (!anyRan) {
            final.insert("type", "error");
            final.insert("message", QString::fromStdString(failure));
        } else {
            final.insert("type", "final");
            std::lock_guard<std::mutex> guard(lock_);
            if (best_ && beatsOverall(*best_)) final.insert("live", EncodeLive(*best_));
        }
        send_(final);
    }
    Say(started_, "the group's search ended here: " + std::to_string(iterations) + " attempts");
    diagnostics::Log("group search %s: %llu attempts", qUtf8Printable(search_), static_cast<unsigned long long>(iterations));
    finished_ = true;
    if (finishedCallback_) finishedCallback_();
}

}  // namespace forevertas::app::remote
