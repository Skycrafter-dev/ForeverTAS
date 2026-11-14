#include "app/app_settings.h"

#include "diagnostics/diagnostics_log.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <utility>

namespace forevertas::app {
namespace {

// QSettings' spelling of a key: no slashes at the ends.
QString Key(const QString &key) {
    qsizetype first = 0;
    qsizetype last = key.size();
    while (first < last && key.at(first) == u'/') ++first;
    while (last > first && key.at(last - 1) == u'/') --last;
    return key.mid(first, last - first);
}

bool Under(const QString &key, const QString &group) {
    return key.size() > group.size() && key.startsWith(group) && key.at(group.size()) == u'/';
}

// Changes made close together are saved together.
constexpr auto kGather = std::chrono::milliseconds(300);

}  // namespace

AppSettings &AppSettings::Instance() {
    static AppSettings *const instance = [] {
        auto *created = new AppSettings;
        // before QCoreApplication (and QSettings' paths) are gone
        qAddPostRoutine([] { Instance().shutDown(); });
        return created;
    }();
    return *instance;
}

AppSettings::~AppSettings() {
    shutDown();
}

void AppSettings::load() const {
    if (loaded_) return;
    loaded_ = true;
    const QSettings file;
    for (const QString &key : file.allKeys()) values_.insert(key, file.value(key));
}

QVariant AppSettings::value(const QString &key, const QVariant &fallback) const {
    std::lock_guard lock(mutex_);
    load();
    return values_.value(Key(key), fallback);
}

bool AppSettings::contains(const QString &key) const {
    std::lock_guard lock(mutex_);
    load();
    return values_.contains(Key(key));
}

void AppSettings::setValue(const QString &key, const QVariant &value) {
    std::unique_lock lock(mutex_);
    load();
    const QString name = Key(key);
    values_.insert(name, value);
    pending_.insert(name, value);
    changed(lock);
}

void AppSettings::remove(const QString &key) {
    std::unique_lock lock(mutex_);
    load();
    const QString name = Key(key);
    for (auto it = values_.begin(); it != values_.end();) {
        it = it.key() == name || Under(it.key(), name) ? values_.erase(it) : std::next(it);
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
        it = Under(it.key(), name) ? pending_.erase(it) : std::next(it);
    }
    pending_.insert(name, std::nullopt);
    changed(lock);
}

void AppSettings::clear() {
    std::unique_lock lock(mutex_);
    loaded_ = true;
    values_.clear();
    pending_.clear();
    clearPending_ = true;
    changed(lock);
}

QSettings::Status AppSettings::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void AppSettings::changed(std::unique_lock<std::mutex> &lock) {
    ++changes_;
    if (stopped_) {   // after shutDown: no thread left to do it
        lock.unlock();
        save();
        return;
    }
    if (!writer_.joinable()) {
        writer_ = std::thread([this] {
            std::unique_lock lock(mutex_);
            for (;;) {
                wake_.wait(lock, [this] { return stopping_ || saved_ != changes_; });
                if (!stopping_ && !hurry_) {
                    wake_.wait_for(lock, kGather, [this] { return stopping_ || hurry_; });
                }
                if (saved_ != changes_) {
                    lock.unlock();
                    save();
                    lock.lock();
                }
                if (stopping_ && saved_ == changes_) break;
            }
            lock.unlock();
            file_.reset();   // made on this thread, gone with it
        });
    }
    wake_.notify_one();
}

// Writes what changed since the last save, then waits for the disk.
void AppSettings::save() {
    std::unique_lock lock(mutex_);
    const std::uint64_t upTo = changes_;
    const bool clear = std::exchange(clearPending_, false);
    const QMap<QString, std::optional<QVariant>> changes = std::exchange(pending_, {});
    hurry_ = false;
    lock.unlock();

    const bool onWriter = writer_.joinable() && std::this_thread::get_id() == writer_.get_id();
    std::unique_ptr<QSettings> local;
    QSettings *file = nullptr;
    if (onWriter) {
        if (!file_) file_ = std::make_unique<QSettings>();
        file = file_.get();
    } else {
        local = std::make_unique<QSettings>();
        file = local.get();
    }
    if (clear) file->clear();
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (it.value()) {
            file->setValue(it.key(), *it.value());
        } else {
            file->remove(it.key());
        }
    }
    file->sync();
    const QSettings::Status status = file->status();
    if (status != QSettings::NoError) {
        diagnostics::Log("settings: saving %s failed (%s)", qUtf8Printable(file->fileName()),
                         status == QSettings::AccessError ? "cannot write" : "format");
    }

    lock.lock();
    saved_ = std::max(saved_, upTo);
    status_ = status;
    savedChanged_.notify_all();
}

void AppSettings::sync() {
    std::unique_lock lock(mutex_);
    if (saved_ == changes_) return;
    if (stopped_ || !writer_.joinable()) {
        lock.unlock();
        save();
        return;
    }
    const std::uint64_t target = changes_;
    hurry_ = true;
    wake_.notify_one();
    if (!savedChanged_.wait_for(lock, std::chrono::seconds(10), [&] { return saved_ >= target; })) {
        diagnostics::Log("settings: still not saved after 10 s");
    }
}

void AppSettings::shutDown() {
    std::unique_lock lock(mutex_);
    if (stopped_) return;
    stopping_ = true;
    wake_.notify_one();
    lock.unlock();
    if (writer_.joinable()) writer_.join();
    lock.lock();
    stopped_ = true;
    const bool left = saved_ != changes_;
    lock.unlock();
    if (left) save();
}

}  // namespace forevertas::app
