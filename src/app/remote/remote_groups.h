#ifndef FOREVERTAS_APP_REMOTE_REMOTE_GROUPS_H
#define FOREVERTAS_APP_REMOTE_REMOTE_GROUPS_H

// ForeverTAS@home's groups (docs/REMOTE.md): this ForeverTAS as a member (an
// id and a secret made once, a name), a group's addresses on the relay, and
// the relay's group requests.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QProcess;

namespace forevertas::app::remote {

struct Member {
    QString id;
    QString secret;
    QString name;
};

// This ForeverTAS's member (made and kept in its settings on first use).
Member CurrentMember();
void SetMemberName(const QString &name);

// The group an invite is for ("GROUPID-KEY", or "public"), or "" if it is not one.
QString GroupOfInvite(const QString &invite);

// A group's address: https://RELAY/groups/ID/ACTION, or for "worker" and
// "client" its WebSocket with the member's credentials.
QUrl GroupUrl(const QString &group, const QString &action, const Member &member);

// A group request (create, join, invite, leave: POST with the member's
// credentials and `body`; status: GET): its answer, or why it failed.
// How this ForeverTAS lends a computer to a group: `app --worker lent --packs
// PACKS`, on Linux inside bubblewrap when it is installed (the system
// read-only, /tmp and the home folder empty but for the Packs, the GPU modules
// and the app; the network kept for the relay). The group's address goes in
// the environment (FOREVERTAS_WORKER_URL). sandboxed: whether it is.
struct LendCommand {
    QString program;
    QStringList arguments;
    bool sandboxed = false;
};
// (`lends`: what it lends, as `--lend gpu,cpu --cpu-threads N`)
LendCommand LenderCommand(const QString &app, const QString &packs, const QStringList &readOnly,
                          const QStringList &lends = {});

// Windows: the lent worker `process` (not started yet) runs in an AppContainer,
// Windows' sandbox for apps: no access to the user's files but `readable`
// (read and run: the Packs, the app, its GPU modules; the folders of the
// libraries this app runs with are added), its own temporary folder, and the
// internet (the relay). False, with why, when it cannot be made; elsewhere
// nothing to do (Linux: LenderCommand's bubblewrap), false.
bool SandboxLender(QProcess *process, QStringList readable, QString *why);

using GroupReply = std::function<void(const QJsonObject &answer, const QString &error)>;
void GroupRequest(QNetworkAccessManager *network, const QString &group, const QString &action,
                  const Member &member, QJsonObject body, GroupReply reply);

}  // namespace forevertas::app::remote

#endif
