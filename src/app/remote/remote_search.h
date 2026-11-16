#ifndef FOREVERTAS_APP_REMOTE_REMOTE_SEARCH_H
#define FOREVERTAS_APP_REMOTE_REMOTE_SEARCH_H

// Remote searching in the window (docs/REMOTE.md): a room on the relay that
// other machines join with its code, the machines that joined, and which of
// them a search uses (each as one more device). And ForeverTAS@home: the
// groups this ForeverTAS is a member of, lending this computer to them and
// searching with their machines (a group is one more device).

#include "app/remote/remote_hub.h"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QString>
#include <QVariantList>

#include <map>
#include <memory>
#include <vector>

class QNetworkAccessManager;
class QProcess;

namespace forevertas::app::remote {

class RemoteSearch final : public QObject {
    Q_OBJECT
    // A room is open (or opening): machines can join with its code.
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY changed)
    Q_PROPERTY(QString code READ code NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    // [{id, name, backend, used}]: the machines that joined.
    Q_PROPERTY(QVariantList workers READ workers NOTIFY changed)
    // How a machine joins: the command for this room's code.
    Q_PROPERTY(QString workerCommand READ workerCommand NOTIFY changed)
    // The name the groups see for this ForeverTAS (its machines' credit).
    Q_PROPERTY(QString memberName READ memberName WRITE setMemberName NOTIFY groupsChanged)
    // [{id, name, invite, owner, isPublic, lend, search, lending, connected, machines, searches, yours,
    //   members, credits: [{name, you, attempts, hours}]}]: the groups joined.
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    // What the last group action said (a failure, or what happened).
    Q_PROPERTY(QString groupMessage READ groupMessage NOTIFY groupsChanged)
    // What this computer lends to groups: its GPU (if ForeverTAS searches on one here), CPU threads (0: none).
    Q_PROPERTY(bool lendGpu READ lendGpu WRITE setLendGpu NOTIFY groupsChanged)
    Q_PROPERTY(int lendCpuThreads READ lendCpuThreads WRITE setLendCpuThreads NOTIFY groupsChanged)
    Q_PROPERTY(int cpuThreads READ cpuThreads CONSTANT)
    Q_PROPERTY(bool hasGpu READ hasGpu NOTIFY groupsChanged)
    // It lends something.
    Q_PROPERTY(bool canLend READ canLend NOTIFY groupsChanged)
    // This computer: [{name, backend, ready, status}] its graphics cards ForeverTAS knows; its processor.
    Q_PROPERTY(QVariantList localGpus READ localGpus NOTIFY groupsChanged)
    Q_PROPERTY(QString cpuName READ cpuName CONSTANT)
    // The machines in the room: [{id, name, parts: [{key, name, detail, used}]}] (each GPU, the CPU's threads).
    Q_PROPERTY(QVariantList roomMachines READ roomMachines NOTIFY changed)

public:
    explicit RemoteSearch(QObject *parent = nullptr);
    ~RemoteSearch() override;

    bool open() const { return open_; }
    void setOpen(bool open);
    QString code() const { return hub_.roomCode(); }
    QString status() const { return status_; }
    QVariantList workers() const;
    QString workerCommand() const;
    Q_INVOKABLE void setUsed(const QString &id, bool used);

    QString memberName() const;
    void setMemberName(const QString &name);
    QVariantList groups() const;
    QString groupMessage() const { return groupMessage_; }
    bool canLend() const;
    QVariantList localGpus() const;
    QString cpuName() const;
    QVariantList roomMachines() const;
    bool hasGpu() const;
    bool lendGpu() const;
    void setLendGpu(bool lend);
    int lendCpuThreads() const;
    void setLendCpuThreads(int threads);
    int cpuThreads() const;
    Q_INVOKABLE void createGroup(const QString &name);
    // An invite ("GROUPID-KEY"), or "public" for the public pool.
    Q_INVOKABLE void joinGroup(const QString &invite);
    Q_INVOKABLE void leaveGroup(const QString &id);
    Q_INVOKABLE void newInvite(const QString &id);
    Q_INVOKABLE void setLending(const QString &id, bool lend);
    Q_INVOKABLE void setSearching(const QString &id, bool search);
    Q_INVOKABLE void refreshGroups();

    // Where lent machines find the game (the window's Packs folder), and
    // whether this computer searches on its GPU now (lending waits meanwhile).
    void setPacksDirectory(const QString &directory);
    void setLocalGpuBusy(bool busy);
    // The ForeverTAS lent machines run (default: this one; tests: the app beside them).
    void setLenderApp(const QString &app) { lenderApp_ = app; }
    // Tests: the groups' connections drop, as when the network or the relay fails.
    void dropGroupConnections();

    // The machines a search uses, as devices (each calibrates its own batches).
    std::vector<SearchDevice> devices() const;
    RemoteHub &hub() { return hub_; }

signals:
    void changed();
    void groupsChanged();
    // The machines a search would use changed.
    void devicesChanged();

private:
    RemoteHub hub_;
    bool open_ = false;
    QString status_;
    QSet<QString> unused_;

    struct Joined {
        QString id, name, invite;
        bool owner = false, lend = false, search = false;
        QJsonObject status;
        std::unique_ptr<RemoteHub> hub;   // searching with it
    };
    void load();
    void save() const;
    void joined(const QJsonObject &answer, const QString &invite, bool owner);
    void updateGroup(Joined &group);   // its hub as its choices say, and the lending
    // One lent machine for every group lent to (its desk takes them one search at a time, the public pool last).
    void updateLending();
    void startLender(const QStringList &ids);
    void stopLender();
    void restartLenders();
    QNetworkAccessManager *network();

    std::map<QString, Joined> groups_;
    QString groupMessage_;
    QString packsDirectory_;
    QString lenderApp_;
    QProcess *lender_ = nullptr;
    QStringList lentTo_;   // the groups it lends to
    bool localGpuBusy_ = false;
    QNetworkAccessManager *network_ = nullptr;
    QTimer statusTimer_;
};

}  // namespace forevertas::app::remote

#endif
