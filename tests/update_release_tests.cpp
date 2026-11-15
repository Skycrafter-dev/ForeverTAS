#include "app/update_release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {

const QString kDigest(64, QLatin1Char('a'));

QJsonObject asset(const QString &name, const QString &digest = kDigest) {
    return QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("sha256"), digest}};
}

QJsonObject module(const QString &id, int sm, bool ptx = false, const QString &abi = QStringLiteral("abi1")) {
    return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("backend"), sm > 0 ? QStringLiteral("cuda") : QStringLiteral("hip")},
            {QStringLiteral("sm"), sm},
            {QStringLiteral("ptx"), ptx},
            {QStringLiteral("abi"), abi},
            {QStringLiteral("url"), QStringLiteral("https://github.com/Skycrafter-dev/ForeverTAS/"
                                                   "releases/download/compute/forevertas-compute-linux-%1-0123.tar.zst")
                                            .arg(id)},
            {QStringLiteral("sha256"), kDigest}};
}

QByteArray releaseJson(const QString &version, const QJsonArray &assets,
                       const QJsonArray &linuxModules = {}, int schema = 1) {
    return QJsonDocument(QJsonObject{
            {QStringLiteral("schema"), schema},
            {QStringLiteral("version"), version},
            {QStringLiteral("release_url"),
             QStringLiteral("https://github.com/Skycrafter-dev/ForeverTAS/"
                            "releases/tag/v%1").arg(version)},
            {QStringLiteral("assets"), assets},
            {QStringLiteral("compute"), QJsonObject{{QStringLiteral("linux"), linuxModules}}}}).toJson();
}

} // namespace

class UpdateReleaseTests final : public QObject {
    Q_OBJECT

private slots:
    void findsTheAppForThisPlatform() {
        const auto catalog = forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.3.0"), QJsonArray{
                        asset(QStringLiteral("ForeverTAS-0.3.0-windows-universal-x86_64-Setup.exe")),
                        asset(QStringLiteral("ForeverTAS-0.3.0-linux-universal-x86_64.AppImage"))}),
                QStringLiteral("linux"), QStringLiteral("x86_64"));
        QVERIFY(catalog.has_value() && catalog->app.has_value());
        QCOMPARE(catalog->app->assetName, QStringLiteral("ForeverTAS-0.3.0-linux-universal-x86_64.AppImage"));
        QCOMPARE(catalog->app->sha256, QByteArray::fromHex(kDigest.toLatin1()));
        QCOMPARE(catalog->app->downloadUrl.toString(),
                 QStringLiteral("https://github.com/Skycrafter-dev/ForeverTAS/releases/download/v0.3.0/"
                                "ForeverTAS-0.3.0-linux-universal-x86_64.AppImage"));
    }

    void keepsOnlyVerifiableModules() {
        QJsonObject foreign = module(QStringLiteral("cuda-sm75"), 75);
        foreign[QStringLiteral("url")] = QStringLiteral("https://example.com/m.tar.zst");
        QJsonObject unsigned_ = module(QStringLiteral("cuda-sm80"), 80);
        unsigned_[QStringLiteral("sha256")] = QStringLiteral("bad");
        const auto catalog = forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.3.0"), {},
                            QJsonArray{module(QStringLiteral("cuda-sm86"), 86), foreign, unsigned_,
                                       module(QStringLiteral("hip"), 0)}),
                QStringLiteral("linux"), QStringLiteral("x86_64"));
        QVERIFY(catalog.has_value());
        QCOMPARE(catalog->modules.size(), 2);
        QCOMPARE(catalog->modules.at(0).id, QStringLiteral("cuda-sm86"));
        QCOMPARE(catalog->modules.at(1).backend, QStringLiteral("hip"));
    }

    void rejectsUnsupportedSchemaAndUntrustedUrl() {
        const QJsonArray app{asset(QStringLiteral("ForeverTAS-0.3.0-linux-universal-x86_64.AppImage"))};
        QVERIFY(!forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.3.0"), app, {}, 2),
                QStringLiteral("linux"), QStringLiteral("x86_64")));
        QByteArray json = releaseJson(QStringLiteral("0.3.0"), app);
        json.replace("https://github.com/", "http://github.com/");
        QVERIFY(!forevertas::app::ParseUpdateCatalog(
                json, QStringLiteral("linux"), QStringLiteral("x86_64")));
    }

    void choosesTheModuleForTheGpu() {
        using forevertas::app::ChooseComputeModules;
        const auto catalog = forevertas::app::ParseUpdateCatalog(
                releaseJson(QStringLiteral("0.3.0"), {},
                            QJsonArray{module(QStringLiteral("cuda-sm61"), 61),
                                       module(QStringLiteral("cuda-sm80"), 80),
                                       module(QStringLiteral("cuda-sm86"), 86),
                                       module(QStringLiteral("cuda-sm120"), 120, true),
                                       module(QStringLiteral("hip"), 0)}),
                QStringLiteral("linux"), QStringLiteral("x86_64"));
        QVERIFY(catalog.has_value());
        const auto pick = [&](int capability, bool amd, const QString &abi = QStringLiteral("abi1")) {
            QStringList ids;
            for (const auto &chosen : ChooseComputeModules(catalog->modules, abi, capability, amd))
                ids << chosen.id;
            return ids.join(QLatin1Char('+'));
        };
        QCOMPARE(pick(86, false), QStringLiteral("cuda-sm86"));   // its own
        QCOMPARE(pick(89, false), QStringLiteral("cuda-sm86"));   // 8.6 code runs on 8.9
        QCOMPARE(pick(87, false), QStringLiteral("cuda-sm86"));
        QCOMPARE(pick(62, false), QStringLiteral("cuda-sm61"));
        QCOMPARE(pick(130, false), QStringLiteral("cuda-sm120")); // later GPU: PTX
        QCOMPARE(pick(75, false), QString());                     // no 7.x code, no PTX for it
        QCOMPARE(pick(0, true), QStringLiteral("hip"));
        QCOMPARE(pick(0, false), QString());
        QCOMPARE(pick(86, true), QStringLiteral("cuda-sm86+hip"));    // both cards: both
        QCOMPARE(pick(86, false, QStringLiteral("other")), QString()); // another app's modules
    }
};

QTEST_GUILESS_MAIN(UpdateReleaseTests)
#include "update_release_tests.moc"
