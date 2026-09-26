#include "app/update_release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {

QJsonObject asset(const QString &version, const QString &name,
                  const QString &digest = QString(64, QLatin1Char('a'))) {
    Q_UNUSED(version)
    return QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("sha256"), digest}};
}

QByteArray releaseJson(const QString &version, const QJsonArray &assets,
                       int schema = 1) {
    return QJsonDocument(QJsonObject{
            {QStringLiteral("schema"), schema},
            {QStringLiteral("version"), version},
            {QStringLiteral("release_url"),
             QStringLiteral("https://github.com/Skycrafter-dev/ForeverTAS/"
                            "releases/tag/v%1").arg(version)},
            {QStringLiteral("assets"), assets}}).toJson();
}

} // namespace

class UpdateReleaseTests final : public QObject {
    Q_OBJECT

private slots:
    void listsOnlyVerifiedMatchingPackages() {
        const QString version = QStringLiteral("0.2.4");
        const QString base = QStringLiteral("ForeverTAS-0.2.4-linux-");
        const auto catalog = forevertas::app::ParseUpdateCatalog(
                releaseJson(version, QJsonArray{
                        asset(version, base + QStringLiteral("universal-x86_64.AppImage")),
                        asset(version, base + QStringLiteral("nvidia-sm75-x86_64.AppImage")),
                        asset(version, base + QStringLiteral("amd-rx7000-rx9000-x86_64.AppImage")),
                        asset(version, base + QStringLiteral("nvidia-sm86-x86_64.AppImage"),
                              QStringLiteral("bad")),
                        asset(version, QStringLiteral("ForeverTAS-0.2.4-windows-nvidia-sm75-x86_64-Setup.exe"))}),
                QStringLiteral("0.2.3"), QStringLiteral("linux"),
                QStringLiteral("x86_64"));
        QVERIFY(catalog.has_value());
        QCOMPARE(catalog->packages.size(), 3);
        QCOMPARE(catalog->packages.at(1).packageId, QStringLiteral("nvidia-sm75"));
        QCOMPARE(catalog->packages.at(1).sha256,
                 QByteArray::fromHex(QByteArray(64, 'a')));
    }

    void sameVersionAllowsSwitchButOlderDoesNot() {
        const QString name = QStringLiteral(
                "ForeverTAS-0.2.3-windows-universal-x86_64-Setup.exe");
        QVERIFY(forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.2.3"),
                            QJsonArray{asset(QStringLiteral("0.2.3"), name)}),
                QStringLiteral("0.2.3"), QStringLiteral("windows"),
                QStringLiteral("x86_64")));
        QVERIFY(!forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.2.3"),
                            QJsonArray{asset(QStringLiteral("0.2.3"), name)}),
                QStringLiteral("0.2.4"), QStringLiteral("windows"),
                QStringLiteral("x86_64")));
    }

    void rejectsUnsupportedSchemaAndUntrustedUrl() {
        const QString name = QStringLiteral(
                "ForeverTAS-0.2.4-linux-universal-x86_64.AppImage");
        QVERIFY(!forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.2.4"),
                            QJsonArray{asset(QStringLiteral("0.2.4"), name)}, 2),
                QStringLiteral("0.2.3"), QStringLiteral("linux"),
                QStringLiteral("x86_64")));
        QByteArray json = releaseJson(QStringLiteral("0.2.4"),
                                     QJsonArray{asset(QStringLiteral("0.2.4"), name)});
        json.replace("https://github.com/", "http://github.com/");
        QVERIFY(!forevertas::app::ParseUpdateCatalog(
                json, QStringLiteral("0.2.3"), QStringLiteral("linux"),
                QStringLiteral("x86_64")));
    }

    void choosesExactHardwarePackageOrUniversal() {
        const QStringList ids{QStringLiteral("universal"),
                              QStringLiteral("nvidia-sm75"),
                              QStringLiteral("amd-rx7000-rx9000")};
        QCOMPARE(forevertas::app::RecommendedPackageId(75, false, ids),
                 QStringLiteral("nvidia-sm75"));
        QCOMPARE(forevertas::app::RecommendedPackageId(86, false, ids),
                 QStringLiteral("universal"));
        QCOMPARE(forevertas::app::RecommendedPackageId(0, true, ids),
                 QStringLiteral("amd-rx7000-rx9000"));
        QCOMPARE(forevertas::app::RecommendedPackageId(0, false, ids),
                 QStringLiteral("universal"));
    }
};

QTEST_GUILESS_MAIN(UpdateReleaseTests)
#include "update_release_tests.moc"
