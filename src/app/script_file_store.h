#ifndef FOREVERTAS_APP_SCRIPT_FILE_STORE_H
#define FOREVERTAS_APP_SCRIPT_FILE_STORE_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace forevertas::app {

class ScriptFileStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString rootDirectory READ rootDirectory CONSTANT)

  public:
    explicit ScriptFileStore(QObject *parent = nullptr);
    explicit ScriptFileStore(const QString &rootDirectory,
                             QObject *parent = nullptr);

    QString rootDirectory() const;

    Q_INVOKABLE QVariantList files(const QString &kind) const;
    Q_INVOKABLE QVariantMap save(const QString &kind, const QString &name,
                                 const QString &content,
                                 bool overwrite = false) const;
    Q_INVOKABLE QVariantMap load(const QString &kind,
                                 const QString &name) const;

  private:
    QString directoryForKind(const QString &kind) const;
    QVariantMap resolvedFile(const QString &kind, const QString &name) const;

    QString rootDirectory_;
};

} // namespace forevertas::app

#endif
