// Remote searching (docs/REMOTE.md): a worker on this computer over a
// WebSocket, or through the relay (FTAS_REMOTE_RELAY), or a group's lent
// machines (FTAS_REMOTE_GROUP). The same search on one local device and on
// one remote worker gives the same result (score, inputs, winning iteration
// and count), GPU or CPU, and searches on several devices run to their end.
// The GPU parts need CUDA here; the CPU parts run where there is none (or
// with FTAS_REMOTE_CPU=1).
#include "app/compute_modules.h"
#include "app/remote/remote_codec.h"
#include "app/app_settings.h"
#include "app/remote/remote_groups.h"
#include "app/remote/remote_search.h"
#include "diagnostics/diagnostics_log.h"
#include "app/remote/remote_hub.h"
#include "app/remote/remote_worker.h"
#include "mutations/input_event_formatter.h"
#include "searches/algorithm_registry.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QStandardPaths>
#include <QUuid>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <functional>
#include <iostream>
#include <thread>

namespace {

using namespace forevertas;
using namespace forevertas::app::remote;

void Process(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

SearchRequest Request(const char *packs, const char *replay) {
    SearchRequest request{packs, replay};
    request.backend = PhysicsBackend::Cuda;
    request.parallelSampleCount = 4096u;
    OptionConfiguration velocity{kVelocityEvaluationId,
                                 FindEvaluationTarget(kVelocityEvaluationId)->defaultSettings};
    velocity.settings["minTimeMs"] = velocity.settings["maxTimeMs"] = "1000";
    request.evaluationTarget = velocity;
    request.modifiers.clear();
    for (const char *id : {kInputDeletionModifierId, kSmoothSteeringModifierId}) {
        OptionConfiguration modifier{id, FindModifier(id)->defaultSettings};
        modifier.settings["minTimeMs"] = "10";
        modifier.settings["maxTimeMs"] = "990";
        request.modifiers.push_back(modifier);
    }
    request.searchAlgorithm.settings["autoPromoteBest"] = "false";
    request.simulationHorizonMs = 5000u;
    return request;
}

// FTAS_REMOTE_GROUP=1 (with the app as the 3rd argument): ForeverTAS@home on the relay. Two lent machines (one
// joining by its invite on the command line, one lent as the window lends, its address in its environment), a
// search with the group on both, two searches at once sharing them (one machine moved), a machine that dies
// mid-search, and the credit the relay counted.
int GroupTest(const char *packs, const char *replay, const QString &app) {
    std::cout << std::unitbuf;
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_LOG")) diagnostics::UseLogFile(stderr);
    QNetworkAccessManager network;
    const auto ask = [&](const QString &group, const QString &action, const Member &member, QJsonObject body) {
        QJsonObject answer;
        QString failure = QStringLiteral("no answer");
        QEventLoop waiting;
        GroupRequest(&network, group, action, member, std::move(body), [&](const QJsonObject &a, const QString &e) {
            answer = a;
            failure = e;
            waiting.quit();
        });
        QTimer::singleShot(15000, &waiting, &QEventLoop::quit);
        waiting.exec();
        if (!failure.isEmpty()) throw std::runtime_error(action.toStdString() + ": " + failure.toStdString());
        return answer;
    };
    const auto someone = [](const QString &name) {
        return Member{QUuid::createUuid().toString(QUuid::Id128), QUuid::createUuid().toString(QUuid::Id128), name};
    };
    const Member alice = someone(QStringLiteral("Alice")), bob = someone(QStringLiteral("Bob")),
                 lender = someone(QStringLiteral("Lender"));
    const QJsonObject created = ask(QString(), QStringLiteral("create"), alice, {{"name", "remote test"}});
    const QString group = created.value("id").toString(), invite = created.value("invite").toString();
    std::cout << "group " << group.toStdString() << '\n';
    QElapsedTimer clock;
    clock.start();
    ask(group, QStringLiteral("join"), bob, {{"invite", invite}});
    ask(group, QStringLiteral("join"), lender, {{"invite", invite}});

    // the machines: ForeverTAS --worker INVITE (it joins as this computer's member), and one lent from a window
    std::vector<std::unique_ptr<QProcess>> machines;
    // FTAS_REMOTE_LEND: what they lend (default gpu; cpu: as a computer with no GPU; gpu,cpu: both)
    const QString lends = qEnvironmentVariable("FTAS_REMOTE_LEND", QStringLiteral("gpu"));
    const std::uint64_t unit = lends.contains("gpu") ? 4096u : 256u;   // attempts: a GPU's are many more
    const auto lend = [&](bool byInvite) {
        auto process = std::make_unique<QProcess>();
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        if (!byInvite)
            environment.insert("FOREVERTAS_WORKER_URLS", GroupUrl(group, "worker", lender).toString(QUrl::FullyEncoded));
        process->setProcessEnvironment(environment);
        if (byInvite) {
            process->start(app, {"--worker", invite, "--packs", packs, "--name", "test machine", "--lend", lends,
                                 "--cpu-threads", "4"});
        } else {
            // as the window lends: in bubblewrap where it is installed
            const LendCommand command = LenderCommand(app, QString::fromLocal8Bit(packs),
                                                      {app::ComputeModules::InstallDirectory(),
                                                       QFileInfo(app).absoluteDir().absolutePath() + "/.."},
                                                      {"--lend", lends, "--cpu-threads", "4"});
            bool sandboxed = command.sandboxed;
#if defined(_WIN32)
            QString why;
            sandboxed = SandboxLender(process.get(), {QString::fromLocal8Bit(packs), app::ComputeModules::InstallDirectory(),
                                                      QFileInfo(app).absolutePath()}, &why);
            if (!sandboxed) std::cout << "(no AppContainer: " << why.toStdString() << ")\n";
#endif
            std::cout << "lent " << (sandboxed ? (command.sandboxed ? "in bubblewrap" : "in an AppContainer") : "without a sandbox")
                      << '\n';
            process->start(command.program, command.arguments);
        }
        machines.push_back(std::move(process));
    };
    lend(true);
    lend(false);

    app::ComputeModules &modules = app::ComputeModules::Instance();
    RemoteHub aliceHub, bobHub;
    std::map<std::string, RemoteHub *> hubs{{"alice", &aliceHub}, {"bob", &bobHub}};
    modules.SetRemoteRunner([&hubs](const SearchRequest &request, const SearchRunControl *control) {
        return hubs.at(request.remoteDevice)->runOnGroup(request, control);
    });
    aliceHub.openGroup(GroupUrl(group, "client", alice));
    bobHub.openGroup(GroupUrl(group, "client", bob));
    QElapsedTimer waiting;
    waiting.start();
    while ((!aliceHub.groupOpen() || !bobHub.groupOpen()) && waiting.elapsed() < 10000) Process(20);
    if (!aliceHub.groupOpen() || !bobHub.groupOpen()) {
        std::cerr << "the searches did not connect to the group\n";
        return 1;
    }
    SearchRequest request = Request(packs, replay);
    // FTAS_REMOTE_APPROXIMATE: the group's searches on approximate physics (the machines' twin), promoting
    request.approximatePhysics = qEnvironmentVariableIsSet("FTAS_REMOTE_APPROXIMATE");
    if (request.approximatePhysics) request.searchAlgorithm.settings["autoPromoteBest"] = "true";
    std::atomic_bool stopAlice{false};
    std::atomic<std::size_t> sharedAt{0u};   // how often both searches held a machine at once
    const auto search = [&](const std::string &who, std::uint64_t limit, std::size_t *mostMachines) {
        SearchRunControl control;
        if (limit != 0u) control.iterationLimit = limit;
        else control.stopRequested = [&stopAlice] { return stopAlice.load(); };
        std::optional<SearchResult> result;
        std::string failure;
        std::thread running([&] {
            try {
                SearchDevice device{PhysicsBackend::Cuda, 4096u, false, who};
                result = modules.RunSearch(request, {device}, &control);
            } catch (const std::exception &error) {
                failure = error.what();
            }
        });
        // (the hubs answer on the main thread: only it pumps; another thread just waits)
        const bool main = QThread::currentThread() == QCoreApplication::instance()->thread();
        while (main && !result && failure.empty()) {
            Process(20);
            if (mostMachines != nullptr) *mostMachines = std::max<std::size_t>(*mostMachines, static_cast<std::size_t>(hubs.at(who)->groupMachines()));
            if (static_cast<std::size_t>(aliceHub.groupMachines()) == 1u && static_cast<std::size_t>(bobHub.groupMachines()) == 1u) ++sharedAt;
        }
        running.join();
        if (!failure.empty()) throw std::runtime_error(who + ": " + failure);
        return *result;
    };
    int failures = 0;
    try {
        // one search: both machines
        std::size_t most = 0;
        const SearchResult one = search("alice", 8u * unit, &most);
        std::cout << "(" << clock.elapsed() << " ms) one search: " << one.iterations << " attempts on up to " << most << " machines, best " << one.bestScore << '\n';
        if (most != 2u || one.iterations < 8u * unit) {
            std::cerr << "the search did not run on both machines\n";
            ++failures;
        }
        // two at once: one machine each (Alice's gives one back)
        std::size_t bobMost = 0;
        std::optional<SearchResult> aliceResult;
        std::string aliceFailure;
        std::atomic_bool aliceDone{false};
        std::thread aliceSearch([&] {
            try {
                aliceResult = search("alice", 0u, nullptr);
            } catch (const std::exception &error) {
                aliceFailure = error.what();
            }
            aliceDone = true;
        });
        while (static_cast<std::size_t>(aliceHub.groupMachines()) < 2u && waiting.elapsed() < 40000) Process(20);
        const SearchResult bobResult = search("bob", 8u * unit, &bobMost);
        // a machine dies while Alice's runs on it; hers ends a moment later
        const std::size_t shared = sharedAt.load();
        machines.back()->kill();
        Process(2000);
        stopAlice = true;
        while (!aliceDone) Process(20);
        aliceSearch.join();
        std::cout << "(" << clock.elapsed() << " ms) two searches: Bob's " << bobResult.iterations << " attempts on up to " << bobMost
                  << " machine(s), one each " << shared << " times; Alice's " << (aliceResult ? aliceResult->iterations : 0u)
                  << " attempts (a machine killed) " << aliceFailure << '\n';
        if (bobMost != 1u || shared == 0u || !aliceResult) {
            std::cerr << "the two searches did not share the machines\n";
            ++failures;
        }
        const QJsonObject status = ask(group, "status", alice, {});
        double attempts = 0.0;
        for (const QJsonValue &credit : status.value("credits").toArray()) attempts += credit.toObject().value("attempts").toDouble();
        std::cout << "credit: " << attempts << " attempts counted\n";
        if (attempts <= 0.0) {
            std::cerr << "the relay counted no credit\n";
            ++failures;
        }
    } catch (const std::exception &error) {
        std::cerr << "group test failed: " << error.what() << '\n';
        ++failures;
    }
    for (auto &machine : machines) {
        machine->kill();
        machine->waitForFinished(3000);
    }
    if (failures == 0) std::cout << "a group searches on its lent machines\n";
    return failures == 0 ? 0 : 1;
}

// FTAS_REMOTE_GROUP_EXTERNAL=1: a group whose machine is started elsewhere (another computer, another system:
// ForeverTAS --worker INVITE, printed here); a search on it, every best it sends re-simulated here.
int ExternalGroupTest(const char *packs, const char *replay) {
    std::cout << std::unitbuf;
    QNetworkAccessManager network;
    QJsonObject created;
    QString failure = QStringLiteral("no answer");
    const Member window{QUuid::createUuid().toString(QUuid::Id128), QUuid::createUuid().toString(QUuid::Id128), "Window"};
    {
        QEventLoop waiting;
        GroupRequest(&network, QString(), "create", window, {{"name", "cross-system test"}},
                     [&](const QJsonObject &answer, const QString &error) {
                         created = answer;
                         failure = error;
                         waiting.quit();
                     });
        waiting.exec();
    }
    if (!failure.isEmpty()) {
        std::cerr << "no group: " << failure.toStdString() << '\n';
        return 1;
    }
    std::cout << "waiting for a machine: ForeverTAS --worker " << created.value("invite").toString().toStdString() << std::endl;
    RemoteHub hub;
    hub.openGroup(GroupUrl(created.value("id").toString(), "client", window));
    app::ComputeModules &modules = app::ComputeModules::Instance();
    modules.SetRemoteRunner([&hub](const SearchRequest &request, const SearchRunControl *control) {
        return hub.runOnGroup(request, control);
    });
    QElapsedTimer waiting;
    waiting.start();
    while (!hub.groupOpen() && waiting.elapsed() < 10000) Process(20);
    SearchRequest request = Request(packs, replay);
    SearchRunControl control;
    control.iterationLimit = 4096u;
    std::optional<SearchResult> result;
    std::string error;
    std::thread running([&] {
        try {
            result = modules.RunSearch(request, {SearchDevice{PhysicsBackend::Cuda, 4096u, false, "group"}}, &control);
        } catch (const std::exception &e) {
            error = e.what();
        }
    });
    int most = 0;
    while (!result && error.empty()) {
        Process(20);
        most = std::max(most, hub.groupMachines());
    }
    running.join();
    std::cout << "a search on " << most << " machine(s) elsewhere: "
              << (result ? std::to_string(result->iterations) + " attempts, best " + std::to_string(result->bestScore)
                         : "failed: " + error)
              << '\n';
    if (!result || most < 1) return 1;
    std::cout << "a machine elsewhere searches for this computer\n";
    return 0;
}

// FTAS_REMOTE_MULTI=1 (the app as the 3rd argument): one machine lent to a group and to the public pool at once
// (ForeverTAS --worker INVITE,public): a public search gets it; a search of the group takes it (the group first);
// once that ends, the public search gets it back.
int MultiTest(const char *packs, const char *replay, const QString &app) {
    std::cout << std::unitbuf;
    QNetworkAccessManager network;
    const auto ask = [&](const QString &group, const QString &action, const Member &member, QJsonObject body) {
        QJsonObject answer;
        QString failure = QStringLiteral("no answer");
        QEventLoop waiting;
        GroupRequest(&network, group, action, member, std::move(body), [&](const QJsonObject &a, const QString &e) {
            answer = a;
            failure = e;
            waiting.quit();
        });
        waiting.exec();
        if (!failure.isEmpty()) throw std::runtime_error(action.toStdString() + ": " + failure.toStdString());
        return answer;
    };
    const Member window{QUuid::createUuid().toString(QUuid::Id128), QUuid::createUuid().toString(QUuid::Id128), "Window"};
    int failures = 0;
    const auto check = [&failures](bool ok, const std::string &what) {
        std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
        if (!ok) ++failures;
    };
    try {
        const QJsonObject created = ask(QString(), "create", window, {{"name", "multi test"}});
        ask("public", "join", window, {});
        const QString group = created.value("id").toString();
        QProcess machine;
        machine.setProcessChannelMode(QProcess::ForwardedChannels);
        machine.start(app, {"--worker", "public," + created.value("invite").toString(), "--packs", packs, "--name",
                            "multi machine", "--lend", "cpu", "--cpu-threads", "2"});
        RemoteHub groupHub, poolHub;
        std::map<std::string, RemoteHub *> hubs{{"group", &groupHub}, {"public", &poolHub}};
        app::ComputeModules &modules = app::ComputeModules::Instance();
        modules.SetRemoteRunner([&hubs](const SearchRequest &request, const SearchRunControl *control) {
            return hubs.at(request.remoteDevice)->runOnGroup(request, control);
        });
        groupHub.openGroup(GroupUrl(group, "client", window));
        poolHub.openGroup(GroupUrl("public", "client", window));
        const auto waitFor = [](const std::function<bool()> &done, int milliseconds) {
            QElapsedTimer timer;
            timer.start();
            while (!done() && timer.elapsed() < milliseconds) Process(20);
            return done();
        };
        waitFor([&] { return groupHub.groupOpen() && poolHub.groupOpen(); }, 10000);
        SearchRequest request = Request(packs, replay);
        std::atomic_bool stopPool{false}, stopGroup{false}, poolDone{false}, groupDone{false};
        std::optional<SearchResult> poolResult, groupResult;
        const auto run = [&](const std::string &who, std::atomic_bool &stop, std::atomic_bool &done,
                             std::optional<SearchResult> &out) {
            return std::thread([&, who] {
                SearchRunControl control;
                control.stopRequested = [&stop] { return stop.load(); };
                try {
                    out = modules.RunSearch(request, {SearchDevice{PhysicsBackend::Cuda, 4096u, false, who}}, &control);
                } catch (const std::exception &error) {
                    std::cerr << who << ": " << error.what() << '\n';
                }
                done = true;
            });
        };
        std::thread pool = run("public", stopPool, poolDone, poolResult);
        check(waitFor([&] { return poolHub.groupMachines() == 1; }, 25000), "a public search gets the machine");
        std::thread groupSearch = run("group", stopGroup, groupDone, groupResult);
        check(waitFor([&] { return groupHub.groupMachines() == 1 && poolHub.groupMachines() == 0; }, 25000),
              "a search of its group takes it (the group first)");
        Process(3000);
        stopGroup = true;
        waitFor([&] { return groupDone.load(); }, 60000);
        groupSearch.join();
        check(waitFor([&] { return poolHub.groupMachines() == 1; }, 25000), "after it, the public search gets it back");
        stopPool = true;
        waitFor([&] { return poolDone.load(); }, 60000);
        pool.join();
        machine.kill();
        machine.waitForFinished(3000);
        ask("public", "leave", window, {});
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    if (failures == 0) std::cout << "one machine lends to a group and to the public pool, the group first\n";
    return failures == 0 ? 0 : 1;
}

// FTAS_REMOTE_LENDING=1 (the app as the 3rd argument): the window's side of a group, RemoteSearch as the
// Preferences use it: a group made, this computer lent to it (CPU threads), the lent machine stopped at once
// while a search of its own runs on its GPU and back after, a search with the group, the group left.
int LendingTest(const char *packs, const char *replay, const QString &app) {
    std::cout << std::unitbuf;
    QStandardPaths::setTestModeEnabled(true);   // (its own settings)
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    app::AppSettings::Instance().clear();
    RemoteSearch window;
    window.setLenderApp(app);
    window.setPacksDirectory(QString::fromLocal8Bit(packs));
    window.setLendGpu(false);
    window.setLendCpuThreads(2);
    const auto waitFor = [](const std::function<bool()> &done, int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < milliseconds) Process(50);
        return done();
    };
    const auto group = [&window]() { return window.groups().isEmpty() ? QVariantMap{} : window.groups().front().toMap(); };
    // the relay's view, asked again every second
    const auto machines = [&](int wanted, int milliseconds) {
        QElapsedTimer asked;
        return waitFor([&] {
            if (!asked.isValid() || asked.elapsed() > 1000) {
                window.refreshGroups();
                asked.restart();
            }
            return group().value("machines").toInt() == wanted;
        }, milliseconds);
    };
    int failures = 0;
    const auto check = [&failures](bool ok, const std::string &what) {
        std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
        if (!ok) ++failures;
    };
    window.createGroup(QStringLiteral("lending test"));
    check(waitFor([&] { return !group().isEmpty(); }, 15000), "a group is made from the window");
    const QString id = group().value("id").toString();
    window.setLending(id, true);
    check(machines(1, 25000), "lending: the group sees this computer");
    QElapsedTimer busy;
    busy.start();
    window.setLocalGpuBusy(true);
    const qint64 pausing = busy.elapsed();
    check(pausing < 200, "a search here on the GPU pauses the lending at once (" + std::to_string(pausing) + " ms)");
    check(machines(0, 20000), "while it runs, the group sees no machine of this computer");
    window.setLocalGpuBusy(false);
    check(machines(1, 25000), "after it, this computer is lent again");
    // a search with the group, as the window starts one: the group is one device
    window.setSearching(id, true);
    check(waitFor([&] { return !window.devices().empty(); }, 15000), "searching with the group: a device");
    try {
        SearchRequest request = Request(packs, replay);
        request.backend = PhysicsBackend::OptimizedCpu;
        SearchRunControl control;
        control.iterationLimit = 1024u;
        std::atomic<std::uint64_t> seen{0u};
        control.statisticsChanged = [&seen](const SearchStatisticsUpdate &statistics) { seen = statistics.iterations; };
        std::optional<SearchResult> result;
        std::string failure;
        const std::vector<SearchDevice> devices = window.devices();
        std::thread running([&] {
            try {
                result = app::ComputeModules::Instance().RunSearch(request, devices, &control);
            } catch (const std::exception &error) {
                failure = error.what();
            }
        });
        // the connection drops while it runs: the window connects again and the search goes on
        bool dropped = false;
        while (!result && failure.empty()) {
            Process(20);
            if (!dropped && seen.load() >= 256u) {
                window.dropGroupConnections();
                dropped = true;
                std::cout << "(the group's connection dropped after " << seen.load() << " attempts)\n";
            }
        }
        running.join();
        check(dropped, "the connection dropped mid-search");
        check(failure.empty() && result && result->iterations >= 1024u,
              "a search with the group runs on the lent machine, through a dropped connection" +
                      (result ? " (" + std::to_string(result->iterations) + " attempts)" : ": " + failure));
    } catch (const std::exception &error) {
        check(false, std::string("the search with the group: ") + error.what());
    }
    window.leaveGroup(id);
    check(window.groups().isEmpty(), "the group is left");
    Process(500);
    app::AppSettings::Instance().clear();
    if (failures == 0) std::cout << "the window lends and searches with a group\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral(FOREVERTAS_VERSION));   // as the app's
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: forevertas-remote-tests PACKS REPLAY [FOREVERTAS (the worker as its own process)]\n";
        return 2;
    }
    const char *packs = argv[1], *replay = argv[2];
    app::ComputeModules &modules = app::ComputeModules::Instance();
    // with no GPU here (FTAS_REMOTE_AS_APP: as a window on a computer with none) only the CPU parts run
    const bool localGpu = modules.Status(PhysicsBackend::Cuda).ready;
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_LOG")) diagnostics::UseLogFile(stderr);
#if defined(_WIN32)
    // FTAS_REMOTE_SANDBOX_PROBE=1 (Windows, the app as the 3rd argument): a worker in the AppContainer tries a room
    // that does not exist; all it says is shown (its answer proves it reaches the relay)
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_SANDBOX_PROBE") && argc == 4) {
        QProcess probe;
        probe.setProcessChannelMode(QProcess::MergedChannels);
        QString why;
        const bool sandboxed = SandboxLender(&probe, {QString::fromLocal8Bit(packs), QFileInfo(QString::fromLocal8Bit(argv[3])).absolutePath()}, &why);
        std::cout << (sandboxed ? "in an AppContainer" : "no AppContainer: " + why.toStdString()) << std::endl;
        // FTAS_REMOTE_PROBE_LENT=1: started as a window lends (a group's address in its environment, offscreen, CPU)
        QStringList arguments{"--worker", qEnvironmentVariable("FTAS_REMOTE_PROBE_TARGET", "ZZZZZZZZ"), "--packs",
                              QString::fromLocal8Bit(packs)};
        if (qEnvironmentVariableIsSet("FTAS_REMOTE_PROBE_LENT")) {
            QProcessEnvironment environment = probe.processEnvironment();
            environment.insert("FOREVERTAS_WORKER_URLS",
                               RelayUrl().resolved(QUrl("/groups/ZZZZZZZZ/worker?member=x&secret=y")).toString().replace("https:", "wss:"));
            if (qEnvironmentVariable("FTAS_REMOTE_PROBE_LENT") == "2") environment.insert("QT_QPA_PLATFORM", "offscreen");
            probe.setProcessEnvironment(environment);
            arguments = {"--worker", "lent", "--packs", QString::fromLocal8Bit(packs), "--lend", "cpu", "--cpu-threads", "2"};
        }
        probe.start(QString::fromLocal8Bit(argv[3]), arguments);
        probe.waitForFinished(30000);
        std::cout << probe.readAll().toStdString() << "exit " << probe.exitCode() << std::endl;
        return 0;
    }
#endif
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_MULTI") && argc == 4) {
        if (qEnvironmentVariableIsSet("FTAS_REMOTE_LOG")) diagnostics::UseLogFile(stderr);
        return MultiTest(packs, replay, QString::fromLocal8Bit(argv[3]));
    }
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_GROUP_EXTERNAL")) {
        if (qEnvironmentVariableIsSet("FTAS_REMOTE_LOG")) diagnostics::UseLogFile(stderr);
        return ExternalGroupTest(packs, replay);
    }
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_LENDING")) {
        if (argc != 4) {
            std::cerr << "FTAS_REMOTE_LENDING needs the app as the 3rd argument\n";
            return 2;
        }
        return LendingTest(packs, replay, QString::fromLocal8Bit(argv[3]));
    }
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_GROUP")) {
        if (argc != 4) {
            std::cerr << "FTAS_REMOTE_GROUP needs the app as the 3rd argument\n";
            return 2;
        }
        return GroupTest(packs, replay, QString::fromLocal8Bit(argv[3]));
    }
    RemoteHub hub;
    // FTAS_REMOTE_RELAY=1: through a room on the relay (the internet), else a server here
    const bool relay = qEnvironmentVariableIsSet("FTAS_REMOTE_RELAY");
    quint16 port = 0;
    QString joinAt;
    if (relay) {
        QString code;
        QObject::connect(&hub, &RemoteHub::roomChanged, [&code](const QString &opened) { code = opened; });
        QObject::connect(&hub, &RemoteHub::roomFailed, [](const QString &why) {
            std::cerr << "no room: " << why.toStdString() << '\n';
        });
        hub.openRoom();
        QElapsedTimer opening;
        opening.start();
        while (code.isEmpty() && opening.elapsed() < 15000) Process(20);
        if (code.isEmpty()) return 1;
        std::cout << "room " << code.toStdString() << " on " << RelayUrl().toString().toStdString() << '\n';
        joinAt = code;
    } else {
        port = hub.listen();
        if (port == 0) return 1;
        joinAt = QStringLiteral("ws://127.0.0.1:%1").arg(port);
    }
    modules.SetRemoteRunner([&hub](const SearchRequest &request, const SearchRunControl *control) {
        return hub.runOnWorker(QString::fromStdString(request.remoteDevice), request, control);
    });

    // the worker: the app itself (ForeverTAS --worker), or on a thread of its own
    QProcess process;
    if (argc == 4) {
        process.setProcessChannelMode(QProcess::ForwardedChannels);
        process.start(QString::fromLocal8Bit(argv[3]),
                      {QStringLiteral("--worker"), joinAt, QStringLiteral("--packs"), QString::fromLocal8Bit(packs),
                       QStringLiteral("--cpu-threads"), QStringLiteral("4")});
    }
    QThread thread;
    auto *worker = new RemoteWorker(
            QString::fromLocal8Bit(packs),
            [&modules](const SearchRequest &request, const SearchRunControl *control) {
                return modules.RunSearch(request, control);
            },
            QJsonObject{{"gpus", localGpu ? QJsonArray{QJsonObject{{"backend", "cuda"}, {"name", "here"}}} : QJsonArray{}},
                        {"cpu", QJsonObject{{"name", "here"}, {"threads", 4}}}});
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    // FTAS_REMOTE_EXTERNAL=1 (with FTAS_REMOTE_RELAY): a worker started elsewhere joins (another computer,
    // another system: ForeverTAS --worker CODE)
    const bool external = qEnvironmentVariableIsSet("FTAS_REMOTE_EXTERNAL");
    if (argc == 3 && !external) thread.start();
    if (!external)
        QMetaObject::invokeMethod(worker, [worker, joinAt] { worker->connectTo(WorkerUrl(joinAt)); }, Qt::QueuedConnection);
    else
        std::cout << "waiting for a worker: ForeverTAS --worker " << joinAt.toStdString() << std::endl;
    QElapsedTimer waiting;
    waiting.start();
    while (hub.workers().isEmpty() && waiting.elapsed() < (external ? 40000 : 10000)) Process(20);
    if (hub.workers().isEmpty()) {
        std::cerr << "the worker did not join\n";
        thread.quit();
        thread.wait();
        return 1;
    }
    const QString remote = hub.workers().front();
    const bool workerGpu = !hub.workerHello(remote).value("gpus").toArray().isEmpty();
    std::cout << "(the worker joined after " << waiting.elapsed() << " ms: "
              << (workerGpu ? "GPU and CPU" : "CPU only") << ")\n";

    const SearchRequest request = Request(packs, replay);
    const auto search = [&](std::vector<SearchDevice> devices, const SearchRequest &request,
                            std::uint64_t limit = 3u * 4096u) {
        SearchRunControl control;
        control.iterationLimit = limit;
        if (qEnvironmentVariableIsSet("FTAS_REMOTE_AS_APP")) {   // what the window's search sets
            control.reuseLoadedSandbox = true;
            control.sampleImprovementTimelines = true;
            control.sampleBestTimeline = true;
            control.improvementTimelineSampled = [](const SearchLiveUpdate &) {};
            control.progressChanged = [](const SearchProgress &progress) {
                std::cout << "window stage " << static_cast<int>(progress.stage) << '\n';
            };
        }
        QElapsedTimer sinceStart;
        sinceStart.start();
        std::atomic<qint64> firstLive{-1}, firstStatistics{-1};
        control.liveChanged = [&](const SearchLiveUpdate &) {
            qint64 none = -1;
            firstLive.compare_exchange_strong(none, sinceStart.elapsed());
        };
        control.statisticsChanged = [&](const SearchStatisticsUpdate &) {
            qint64 none = -1;
            firstStatistics.compare_exchange_strong(none, sinceStart.elapsed());
        };
        std::optional<SearchResult> result;
        std::string failure;
        std::thread running([&] {
            try {
                result = modules.RunSearch(request, devices, &control);
            } catch (const std::exception &error) {
                failure = error.what();
            }
        });
        while (!result && failure.empty()) Process(20);   // the hub answers the worker meanwhile
        running.join();
        if (qEnvironmentVariableIsSet("FTAS_REMOTE_AS_APP"))
            std::cout << "first live " << firstLive << " ms, first statistics " << firstStatistics << " ms, done "
                      << sinceStart.elapsed() << " ms\n";
        if (!failure.empty()) throw std::runtime_error(failure);
        return *result;
    };
    int failures = 0;
    // FTAS_REMOTE_AS_APP=1: the remote device as the window makes it (calibrating from 1), timed
    if (qEnvironmentVariableIsSet("FTAS_REMOTE_AS_APP")) {
        SearchRequest asApp = request;
        asApp.parallelSampleCount = 1u;
        asApp.calibrateCudaParallelSampleCount = true;
        asApp.approximatePhysics = qEnvironmentVariableIsSet("FTAS_REMOTE_APPROXIMATE");
        asApp.searchAlgorithm.settings["autoPromoteBest"] = "true";
        QElapsedTimer timing;
        timing.start();
        try {
            const SearchResult result = search({SearchDevice{PhysicsBackend::Cuda, 1u, true, remote.toStdString()}}, asApp);
            std::cout << "as the app: " << result.iterations << " attempts in " << timing.elapsed() << " ms\n";
        } catch (const std::exception &error) {
            std::cerr << "as the app: failed after " << timing.elapsed() << " ms: " << error.what() << '\n';
            ++failures;
        }
    }
    // CPU devices (FTAS_REMOTE_CPU=1, or no GPU here; each part fits a minute): one thread here and one on the
    // worker try the same candidates (the same result); the worker's CPU on four threads beside this computer's
    // on two, promoting, runs to its end; approximate physics on a worker's CPU runs
    // (FTAS_REMOTE_CPU=0: not even without a GPU here, for the parts below)
    const bool cpuParts = qEnvironmentVariableIsSet("FTAS_REMOTE_CPU") ? qEnvironmentVariable("FTAS_REMOTE_CPU") != "0"
                                                                       : !localGpu;
    if (cpuParts) try {
        std::cout.precision(17);
        SearchRequest cpu = request;
        cpu.backend = PhysicsBackend::OptimizedCpu;
        cpu.parallelSampleCount = 1u;
        constexpr std::uint64_t kCpuAttempts = 256u;
        QElapsedTimer part;
        part.start();
        const SearchResult here = search({SearchDevice{PhysicsBackend::OptimizedCpu, 1u, false, {}}}, cpu, kCpuAttempts);
        const SearchResult there = search({SearchDevice{PhysicsBackend::OptimizedCpu, 1u, false, remote.toStdString()}},
                                          cpu, kCpuAttempts);
        std::cout << "(" << part.restart() << " ms) ";
        std::cout << "CPU: local " << here.iterations << " attempts, best " << here.bestScore << "; remote "
                  << there.iterations << " attempts, best " << there.bestScore << '\n';
        if (here.bestScore != there.bestScore || here.winningIterationIndex != there.winningIterationIndex ||
            EncodeEvents(here.bestInputs) != EncodeEvents(there.bestInputs)) {
            std::cerr << "the remote worker's CPU search differs from the same search here\n";
            ++failures;
        }
        SearchRequest promoting = cpu;
        promoting.backend = PhysicsBackend::MultiThreadedCpu;
        promoting.parallelSampleCount = 2u;
        promoting.searchAlgorithm.settings["autoPromoteBest"] = "true";
        const SearchResult both = search({SearchDevice{PhysicsBackend::MultiThreadedCpu, 2u, false, {}},
                                          SearchDevice{PhysicsBackend::MultiThreadedCpu, 4u, false, remote.toStdString()}},
                                         promoting, 4u * kCpuAttempts);
        std::cout << "(" << part.restart() << " ms) ";
        std::cout << "CPU here + worker CPU: " << both.iterations << " attempts, best " << both.bestScore << '\n';
        if (both.iterations < 4u * kCpuAttempts) {
            std::cerr << "the search on both CPUs did not run all its attempts\n";
            ++failures;
        }
        // a custom (scripted) target: its attempts are ranked in order across every device
        SearchRequest scripted = promoting;
        OptionConfiguration custom{kScriptedTargetEvaluationId, FindEvaluationTarget(kScriptedTargetEvaluationId)->defaultSettings};
        custom.settings["minTimeMs"] = custom.settings["maxTimeMs"] = "1000";
        custom.settings["script"] = "max car.speed";
        scripted.evaluationTarget = custom;
        const SearchResult customResult = search({SearchDevice{PhysicsBackend::MultiThreadedCpu, 2u, false, {}},
                                                  SearchDevice{PhysicsBackend::MultiThreadedCpu, 4u, false, remote.toStdString()}},
                                                 scripted, 2u * kCpuAttempts);
        std::cout << "(" << part.restart() << " ms) ";
        std::cout << "custom target, CPU here + worker CPU: " << customResult.iterations << " attempts, best "
                  << customResult.bestScore << '\n';
        if (customResult.iterations < 2u * kCpuAttempts) {
            std::cerr << "the custom-target search on both CPUs did not run all its attempts\n";
            ++failures;
        }
#if FOREVERVALIDATOR_HAS_TWIN
        SearchRequest approximate = promoting;
        approximate.approximatePhysics = true;
        const SearchResult twin = search({SearchDevice{PhysicsBackend::MultiThreadedCpu, 4u, false, remote.toStdString()}},
                                         approximate, kCpuAttempts);
        std::cout << "(" << part.restart() << " ms) ";
        std::cout << "approximate on the worker's CPU: " << twin.iterations << " attempts, best " << twin.bestScore << '\n';
#endif
    } catch (const std::exception &error) {
        std::cerr << "remote CPU search failed: " << error.what() << '\n';
        ++failures;
    }
    // one side has a GPU, the other not: this computer's GPU beside the worker's CPU, or the worker's GPU alone
    // (its best re-simulated here, on this system)
    if (!cpuParts && localGpu != workerGpu) try {
        std::cout.precision(17);
        QElapsedTimer part;
        part.start();
        SearchRequest mixed = request;
        mixed.searchAlgorithm.settings["autoPromoteBest"] = "true";
        const SearchResult result = localGpu
                ? search({SearchDevice{PhysicsBackend::Cuda, 4096u, false, {}},
                          SearchDevice{PhysicsBackend::MultiThreadedCpu, 4u, false, remote.toStdString()}}, mixed)
                : search({SearchDevice{PhysicsBackend::Cuda, 4096u, false, remote.toStdString()}}, request);
        std::cout << "(" << part.elapsed() << " ms) " << (localGpu ? "GPU here + worker CPU: " : "the worker's GPU alone: ")
                  << result.iterations << " attempts, best " << result.bestScore << '\n';
        if (result.iterations < 3u * 4096u) {
            std::cerr << "the search did not run all its attempts\n";
            ++failures;
        }
    } catch (const std::exception &error) {
        std::cerr << "the mixed search failed: " << error.what() << '\n';
        ++failures;
    }
    if (cpuParts || !localGpu || !workerGpu) {
        std::cout << (cpuParts && localGpu ? "the GPU parts skipped (FTAS_REMOTE_CPU)\n"
                      : !localGpu ? "no CUDA GPU here: the GPU parts skipped\n"
                                  : "the worker lends no GPU: the GPU parts skipped\n");
    } else try {
        SearchDevice local{PhysicsBackend::Cuda, 4096u, false, {}};
        SearchDevice onWorker{PhysicsBackend::Cuda, 4096u, false, remote.toStdString()};
        // one device each, exact and approximate: the same id blocks, the same result
        for (const bool approximate : {false, true}) {
            SearchRequest each = request;
            each.approximatePhysics = approximate;
            SearchRunControl hereControl;
            hereControl.iterationLimit = 3u * 4096u;
            QElapsedTimer timing;
            timing.start();
            const SearchResult here = RunSearchOnDevices(
                    each, {local},
                    [&](const SearchRequest &deviceRequest, const SearchRunControl *deviceControl) {
                        return modules.RunSearch(deviceRequest, deviceControl);
                    },
                    &hereControl);
            const qint64 hereMs = timing.restart();
            const SearchResult there = search({onWorker}, each);
            std::cout << "(here " << hereMs << " ms, there " << timing.elapsed() << " ms)\n";
            const bool same = here.bestScore == there.bestScore && here.iterations == there.iterations &&
                              here.winningIterationIndex == there.winningIterationIndex &&
                              EncodeEvents(here.bestInputs) == EncodeEvents(there.bestInputs) &&
                              there.bestState.timeMs == here.bestState.timeMs;
            std::cout << (approximate ? "approximate" : "exact") << ": local " << here.iterations
                      << " attempts, best " << here.bestScore << "; remote " << there.iterations << " attempts, best "
                      << there.bestScore << '\n';
            if (!same) {
                std::cerr << "the remote worker's " << (approximate ? "approximate" : "exact")
                          << " search differs from the same search here\n";
                ++failures;
            }
        }
        // both at once, promoting: the shared best goes to the worker
        SearchRequest promoting = request;
        promoting.searchAlgorithm.settings["autoPromoteBest"] = "true";
        const SearchResult both = search({local, onWorker}, promoting);
        std::cout << "local + remote: " << both.iterations << " attempts, best " << both.bestScore << '\n';
        // (its candidates differ from one device's: its best may too)
        if (both.iterations != 3u * 4096u) {
            std::cerr << "the search on both devices did not run all its attempts\n";
            ++failures;
        }
        // a worker with another game's packs refuses
        if (argc == 3) {
            QTemporaryDir other;
            QFile pack(other.filePath(QStringLiteral("Stadium.pak")));
            if (!pack.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot write a test pack");
            pack.write("not the same game");
            pack.close();
            QThread otherThread;
            auto *stranger = new RemoteWorker(other.path(), [&modules](const SearchRequest &r, const SearchRunControl *c) {
                return modules.RunSearch(r, c);
            }, QJsonObject{});
            stranger->moveToThread(&otherThread);
            QObject::connect(&otherThread, &QThread::finished, stranger, &QObject::deleteLater);
            otherThread.start();
            const QStringList before = hub.workers();
            QMetaObject::invokeMethod(stranger, [stranger, joinAt] { stranger->connectTo(WorkerUrl(joinAt)); },
                                      Qt::QueuedConnection);
            QElapsedTimer joining;
            joining.start();
            while (hub.workers().size() == before.size() && joining.elapsed() < 10000) Process(20);
            QString strangerId;
            for (const QString &id : hub.workers())
                if (!before.contains(id)) strangerId = id;
            std::string refusal;
            try {
                search({SearchDevice{PhysicsBackend::Cuda, 4096u, false, strangerId.toStdString()}}, request);
            } catch (const std::exception &error) {
                refusal = error.what();
            }
            std::cout << "another game's packs: " << refusal << '\n';
            if (refusal.find("another version") == std::string::npos) {
                std::cerr << "a worker with other packs did not refuse\n";
                ++failures;
            }
            otherThread.quit();
            otherThread.wait();
        }
    } catch (const std::exception &error) {
        std::cerr << "remote search failed: " << error.what() << '\n';
        ++failures;
    }
    if (argc == 3) {
        thread.quit();
        thread.wait();
    } else {
        delete worker;
        process.kill();   // (on Windows a process without a window ignores terminate())
        process.waitForFinished(5000);
    }
    if (failures == 0) std::cout << "a remote worker searches as a local GPU does\n";
    return failures == 0 ? 0 : 1;
}
