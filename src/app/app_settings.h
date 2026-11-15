#pragma once

#include <QMap>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace forevertas::app {

// The app's settings (QSettings' file), kept in memory: reading and writing
// never wait for the disk. A thread of its own saves the changes a moment
// later (QSettings rewrites and fsyncs the whole file on every save, which on
// a busy disk froze the window for a fifth of a second).
//
// Same calls as QSettings. The whole file is read on first use; QSettings
// written directly after that is not seen.
class AppSettings final : public QObject {
    Q_OBJECT

public:
    static AppSettings &Instance();
    ~AppSettings() override;

    Q_INVOKABLE QVariant value(const QString &key, const QVariant &fallback = {}) const;
    Q_INVOKABLE bool contains(const QString &key) const;
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
    // the key and everything under it ("a" removes "a/b" too), as QSettings
    Q_INVOKABLE void remove(const QString &key);
    void clear();
    // Waits until every change is in the file (at exit).
    void sync();
    // The last save's outcome.
    QSettings::Status status() const;
    // Saves what is left and stops the saving thread; later changes are
    // saved at once on the caller's thread.
    void shutDown();

private:
    AppSettings() = default;
    void load() const;
    void changed(std::unique_lock<std::mutex> &lock);
    void save();

    mutable std::mutex mutex_;
    mutable bool loaded_ = false;
    mutable QVariantMap values_;
    // not saved yet: a value, or nullopt for a removed key
    QMap<QString, std::optional<QVariant>> pending_;
    bool clearPending_ = false;
    std::uint64_t changes_ = 0;
    std::uint64_t saved_ = 0;
    QSettings::Status status_ = QSettings::NoError;
    std::condition_variable wake_;
    std::condition_variable savedChanged_;
    bool hurry_ = false;   // sync() waits: no gathering
    bool stopping_ = false;
    bool stopped_ = false;
    std::thread writer_;
    std::unique_ptr<QSettings> file_;   // the writer's
};

}  // namespace forevertas::app
