#include "app/search_configuration_model.h"

#include "input_timeline_time.h"
#include "searches/algorithm_registry.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>
#include <limits>
#include <memory>
#include <random>
#include <utility>

namespace forevertas::app {
namespace {

constexpr char kSearchAlgorithmKey[] = "selection/searchAlgorithm";
constexpr char kLegacyMutationAlgorithmKey[] = "selection/mutationAlgorithm";
constexpr char kModifierPassesKey[] = "composition/modifiers";
constexpr char kEvaluationTargetKey[] = "selection/evaluationTarget";

QString StoredValue(const char *key, const QString &fallback) {
    return QSettings().value(QLatin1String(key), fallback).toString();
}

QString OptionSettingPath(const QString &category,
                          const QString &optionId,
                          const QString &key) {
    return QStringLiteral("configuration/%1/%2/%3")
            .arg(category, optionId, key);
}

void RemoveRetiredSearchBudget() {
    const QString retiredKey = QString::fromLatin1(
            QByteArray::fromHex("617474656d7074436f756e74"));
    QSettings storage;
    storage.remove(QStringLiteral("search/") + retiredKey);
    storage.remove(OptionSettingPath(
            QStringLiteral("search"),
            QStringLiteral("basic-brute-force"),
            retiredKey));
    storage.remove(OptionSettingPath(
            QStringLiteral("search"),
            QStringLiteral("serial-brute-force"),
            retiredKey));
    storage.remove(QStringLiteral("search/minEvalTimeMs"));
    storage.remove(QStringLiteral("search/maxEvalTimeMs"));
}

OptionSettings ToOptionSettings(const QVariantMap &values) {
    OptionSettings settings;
    for (auto iterator = values.constBegin(); iterator != values.constEnd();
         ++iterator) {
        settings.emplace(iterator.key().toStdString(),
                         iterator.value().toString().toStdString());
    }
    return settings;
}

QVariantMap ToVariantMap(const OptionSettings &settings) {
    QVariantMap values;
    for (const auto &[key, value] : settings) {
        values.insert(QString::fromStdString(key),
                      QString::fromStdString(value));
    }
    return values;
}

QVariantMap ModifierPassValue(const ModifierRegistration &registration,
                              const QVariantMap &settings) {
    return QVariantMap{
            {QStringLiteral("id"), QString::fromStdString(registration.id)},
            {QStringLiteral("settings"), settings}};
}

template<typename Registration>
QVariantMap LoadOptionSettings(const QString &category,
                               const Registration &registration) {
    QSettings storage;
    QVariantMap values;
    for (const auto &[key, defaultValue] : registration.defaultSettings) {
        const QString qKey = QString::fromStdString(key);
        const QString path = OptionSettingPath(
                category, QString::fromStdString(registration.id), qKey);
        QString value;
        bool loaded = false;
        if (storage.contains(path)) {
            value = storage.value(path).toString();
            loaded = true;
        } else {
            for (const std::string &legacyId : registration.legacyIds) {
                const QString legacyPath = OptionSettingPath(
                        category, QString::fromStdString(legacyId), qKey);
                if (!storage.contains(legacyPath)) continue;
                value = storage.value(legacyPath).toString();
                storage.setValue(path, value);
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            const auto legacyKey =
                    registration.legacyPersistenceKeys.find(key);
            if (legacyKey != registration.legacyPersistenceKeys.end() &&
                storage.contains(QString::fromStdString(legacyKey->second))) {
                value = storage.value(
                                       QString::fromStdString(
                                               legacyKey->second))
                                .toString();
            } else {
                value = QString::fromStdString(defaultValue);
            }
        }
        values.insert(qKey, value);
    }
    return values;
}

template<typename Registration>
QVariantList OptionList(const std::vector<Registration> &registrations) {
    QVariantList options;
    options.reserve(static_cast<qsizetype>(registrations.size()));
    for (const Registration &registration : registrations) {
        options.push_back(QVariantMap{
                {QStringLiteral("id"),
                 QString::fromStdString(registration.id)},
                {QStringLiteral("label"),
                 QString::fromStdString(registration.displayName)},
                {QStringLiteral("settingsComponent"),
                 QString::fromStdString(registration.settingsComponent)}});
    }
    return options;
}

}  // namespace

SearchConfigurationModel::SearchConfigurationModel() {
    RemoveRetiredSearchBudget();
    const OptionConfiguration defaultSearch =
            DefaultSearchAlgorithmConfiguration();
    const OptionConfiguration defaultEvaluation =
            DefaultEvaluationTargetConfiguration();

    searchAlgorithmId_ = StoredValue(
            kSearchAlgorithmKey, QString::fromStdString(defaultSearch.id));
    evaluationTargetId_ = StoredValue(
            kEvaluationTargetKey,
            QString::fromStdString(defaultEvaluation.id));

    if (const SearchAlgorithmRegistration *const registration =
                FindSearchAlgorithm(searchAlgorithmId_.toStdString())) {
        const QString canonical = QString::fromStdString(registration->id);
        if (searchAlgorithmId_ != canonical) {
            searchAlgorithmId_ = canonical;
            QSettings().setValue(
                    QLatin1String(kSearchAlgorithmKey), canonical);
        }
    } else {
        searchAlgorithmId_ = QString::fromStdString(defaultSearch.id);
    }

    if (const EvaluationTargetRegistration *const registration =
                FindEvaluationTarget(evaluationTargetId_.toStdString())) {
        const QString canonical = QString::fromStdString(registration->id);
        if (evaluationTargetId_ != canonical) {
            evaluationTargetId_ = canonical;
            QSettings().setValue(
                    QLatin1String(kEvaluationTargetKey), canonical);
        }
    } else {
        evaluationTargetId_ = QString::fromStdString(defaultEvaluation.id);
    }

    loadSearchAlgorithmSettings();
    loadModifierPasses();
    loadEvaluationTargetSettings();
}

QVariantList SearchConfigurationModel::searchAlgorithmOptions() const {
    return OptionList(SearchAlgorithmRegistry());
}

QVariantList SearchConfigurationModel::modifierOptions() const {
    return OptionList(ModifierRegistry());
}

QVariantList SearchConfigurationModel::evaluationTargetOptions() const {
    return OptionList(EvaluationTargetRegistry());
}

QString SearchConfigurationModel::searchAlgorithmId() const {
    return searchAlgorithmId_;
}

QString SearchConfigurationModel::evaluationTargetId() const {
    return evaluationTargetId_;
}

QVariantMap SearchConfigurationModel::searchAlgorithmSettings() const {
    return searchAlgorithmSettings_;
}

QVariantList SearchConfigurationModel::modifierPasses() const {
    return modifierPasses_;
}

QVariantMap SearchConfigurationModel::evaluationTargetSettings() const {
    return evaluationTargetSettings_;
}

QVariantMap SearchConfigurationModel::evaluationTargetSettingsFor(
        const QString &id) const {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(id.toStdString());
    if (registration == nullptr) return {};
    return LoadOptionSettings(QStringLiteral("evaluation"), *registration);
}

bool SearchConfigurationModel::setSearchAlgorithmId(const QString &value) {
    const SearchAlgorithmRegistration *const registration =
            FindSearchAlgorithm(value.toStdString());
    const QString canonical = registration == nullptr
            ? value
            : QString::fromStdString(registration->id);
    if (searchAlgorithmId_ == canonical) return false;
    searchAlgorithmId_ = canonical;
    QSettings().setValue(QLatin1String(kSearchAlgorithmKey), canonical);
    loadSearchAlgorithmSettings();
    return true;
}

bool SearchConfigurationModel::setEvaluationTargetId(const QString &value) {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(value.toStdString());
    const QString canonical = registration == nullptr
            ? value
            : QString::fromStdString(registration->id);
    if (evaluationTargetId_ == canonical) return false;
    evaluationTargetId_ = canonical;
    QSettings().setValue(QLatin1String(kEvaluationTargetKey), canonical);
    loadEvaluationTargetSettings();
    return true;
}

bool SearchConfigurationModel::setSearchAlgorithmSetting(
        const QString &key,
        const QString &value) {
    const SearchAlgorithmRegistration *const registration =
            FindSearchAlgorithm(searchAlgorithmId_.toStdString());
    if (registration == nullptr ||
        registration->defaultSettings.find(key.toStdString()) ==
                registration->defaultSettings.end() ||
        searchAlgorithmSettings_.value(key).toString() == value) {
        return false;
    }
    searchAlgorithmSettings_.insert(key, value);
    persistOptionSetting(
            QStringLiteral("search"), searchAlgorithmId_, key, value);
    return true;
}

bool SearchConfigurationModel::setEvaluationTargetSetting(
        const QString &key,
        const QString &value,
        bool persist) {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(evaluationTargetId_.toStdString());
    if (registration == nullptr ||
        registration->defaultSettings.find(key.toStdString()) ==
                registration->defaultSettings.end() ||
        evaluationTargetSettings_.value(key).toString() == value) {
        return false;
    }
    evaluationTargetSettings_.insert(key, value);
    if (persist) {
        persistOptionSetting(
                QStringLiteral("evaluation"), evaluationTargetId_, key, value);
    }
    return true;
}

bool SearchConfigurationModel::addModifierPass(const QString &id) {
    const ModifierRegistration *const registration =
            FindModifier(id.toStdString());
    if (registration == nullptr) return false;
    modifierPasses_.push_back(ModifierPassValue(
            *registration, ToVariantMap(registration->defaultSettings)));
    persistModifierPasses();
    return true;
}

bool SearchConfigurationModel::removeModifierPass(int index) {
    if (index < 0 || index >= modifierPasses_.size()) return false;
    modifierPasses_.removeAt(index);
    persistModifierPasses();
    return true;
}

bool SearchConfigurationModel::moveModifierPass(int fromIndex, int toIndex) {
    if (fromIndex < 0 || fromIndex >= modifierPasses_.size() ||
        toIndex < 0 || toIndex >= modifierPasses_.size() ||
        fromIndex == toIndex) {
        return false;
    }
    modifierPasses_.move(fromIndex, toIndex);
    persistModifierPasses();
    return true;
}

bool SearchConfigurationModel::setModifierPassId(int index,
                                                 const QString &id) {
    if (index < 0 || index >= modifierPasses_.size()) return false;
    const ModifierRegistration *const registration =
            FindModifier(id.toStdString());
    if (registration == nullptr) return false;
    const QVariantMap existing = modifierPasses_.at(index).toMap();
    if (existing.value(QStringLiteral("id")).toString() ==
        QString::fromStdString(registration->id)) {
        return false;
    }
    modifierPasses_[index] = ModifierPassValue(
            *registration, ToVariantMap(registration->defaultSettings));
    persistModifierPasses();
    return true;
}

bool SearchConfigurationModel::setModifierPassSetting(
        int index,
        const QString &key,
        const QString &value) {
    if (index < 0 || index >= modifierPasses_.size()) return false;
    QVariantMap pass = modifierPasses_.at(index).toMap();
    const ModifierRegistration *const registration = FindModifier(
            pass.value(QStringLiteral("id")).toString().toStdString());
    if (registration == nullptr ||
        registration->defaultSettings.find(key.toStdString()) ==
                registration->defaultSettings.end()) {
        return false;
    }
    QVariantMap settings =
            pass.value(QStringLiteral("settings")).toMap();
    if (settings.value(key).toString() == value) return false;
    settings.insert(key, value);
    pass.insert(QStringLiteral("settings"), settings);
    modifierPasses_[index] = pass;
    persistModifierPasses();
    return true;
}

bool SearchConfigurationModel::randomizeModifierSeeds(
        std::uint32_t entropy) {
    std::mt19937 random(entropy);
    bool changed = false;
    for (QVariant &passValue : modifierPasses_) {
        QVariantMap pass = passValue.toMap();
        QVariantMap settings =
                pass.value(QStringLiteral("settings")).toMap();
        const auto seed = settings.find(QStringLiteral("seed"));
        if (seed == settings.end()) continue;

        std::uint32_t generated = random();
        if (QString::number(generated) == seed->toString()) {
            ++generated;
        }
        *seed = QString::number(generated);
        pass.insert(QStringLiteral("settings"), settings);
        passValue = pass;
        changed = true;
    }
    if (changed) {
        persistModifierPasses();
    }
    return changed;
}

SearchConfigurationValidation SearchConfigurationModel::validate(
        std::uint32_t tickDurationMs,
        std::uint32_t simulationHorizonMs) const {
    const SearchAlgorithmRegistration *const searchRegistration =
            FindSearchAlgorithm(searchAlgorithmId_.toStdString());
    if (searchRegistration == nullptr) {
        return {{}, QStringLiteral("Select a valid search algorithm.")};
    }
    const EvaluationTargetRegistration *const evaluationRegistration =
            FindEvaluationTarget(evaluationTargetId_.toStdString());
    if (evaluationRegistration == nullptr) {
        return {{}, QStringLiteral("Select a valid evaluation target.")};
    }

    const OptionSettings searchSettings =
            ToOptionSettings(searchAlgorithmSettings_);
    const OptionSettings evaluationSettings =
            ToOptionSettings(evaluationTargetSettings_);
    if (const auto error = searchRegistration->validateSettings(
                searchSettings, tickDurationMs)) {
        return {{}, QString::fromStdString(*error)};
    }
    if (const auto error = evaluationRegistration->validateSettings(
                evaluationSettings, tickDurationMs)) {
        return {{}, QString::fromStdString(*error)};
    }
    if (modifierPasses_.isEmpty()) {
        return {{}, QStringLiteral("Add at least one input modifier pass.")};
    }

    std::vector<OptionConfiguration> modifiers;
    std::int64_t earliestMutationTimeMs =
            std::numeric_limits<std::int64_t>::max();
    modifiers.reserve(static_cast<std::size_t>(modifierPasses_.size()));
    for (qsizetype index = 0; index < modifierPasses_.size(); ++index) {
        const QVariantMap pass = modifierPasses_.at(index).toMap();
        const QString id = pass.value(QStringLiteral("id")).toString();
        const ModifierRegistration *const registration =
                FindModifier(id.toStdString());
        if (registration == nullptr) {
            return {{}, QStringLiteral("Modifier pass %1 has an invalid type.")
                                .arg(index + 1)};
        }
        const OptionSettings settings = ToOptionSettings(
                pass.value(QStringLiteral("settings")).toMap());
        if (const auto error = registration->validateSettings(
                    settings, tickDurationMs)) {
            return {{}, QStringLiteral("Modifier pass %1: %2")
                                .arg(index + 1)
                                .arg(QString::fromStdString(*error))};
        }
        const OptionSettings executionSettings =
                ClampInputWindowToSimulationHorizon(
                        settings, tickDurationMs, simulationHorizonMs);
        const std::unique_ptr<InputMutator> mutator =
                registration->create(executionSettings, tickDurationMs);
        earliestMutationTimeMs = std::min(
                earliestMutationTimeMs,
                mutator->EarliestMutationTimeMs());
        modifiers.push_back({registration->id, settings});
    }
    const std::unique_ptr<IterationEvaluator> evaluator =
            evaluationRegistration->create(
                    evaluationSettings, tickDurationMs);
    const EvaluationPlan plan = evaluator->Plan(
            simulationHorizonMs,
            earliestMutationTimeMs,
            tickDurationMs);
    if (plan.startTimeMs < earliestMutationTimeMs) {
        return {{},
                QStringLiteral(
                        "Evaluation start time %1 ms precedes the first "
                        "modifier time at %2 ms.")
                        .arg(plan.startTimeMs)
                        .arg(earliestMutationTimeMs)};
    }
    if (plan.endTimeMs > simulationHorizonMs) {
        return {{},
                QStringLiteral(
                        "Evaluation maximum time %1 ms exceeds the "
                        "Simulation horizon of %2 ms.")
                        .arg(plan.endTimeMs)
                        .arg(simulationHorizonMs)};
    }

    return {SearchComponentConfiguration{
                    {searchRegistration->id, searchSettings},
                    std::move(modifiers),
                    {evaluationRegistration->id, evaluationSettings}},
            {}};
}

void SearchConfigurationModel::loadSearchAlgorithmSettings() {
    const SearchAlgorithmRegistration *const registration =
            FindSearchAlgorithm(searchAlgorithmId_.toStdString());
    searchAlgorithmSettings_ = registration == nullptr
            ? QVariantMap{}
            : LoadOptionSettings(QStringLiteral("search"), *registration);
}

void SearchConfigurationModel::loadModifierPasses() {
    modifierPasses_.clear();
    QSettings storage;
    const QJsonDocument document = QJsonDocument::fromJson(
            storage.value(QLatin1String(kModifierPassesKey)).toByteArray());
    if (document.isArray()) {
        for (const QJsonValue &value : document.array()) {
            if (!value.isObject()) continue;
            const QJsonObject object = value.toObject();
            const ModifierRegistration *const registration = FindModifier(
                    object.value(QStringLiteral("id"))
                            .toString()
                            .toStdString());
            if (registration == nullptr) continue;
            QVariantMap settings = ToVariantMap(registration->defaultSettings);
            const QJsonObject storedSettings =
                    object.value(QStringLiteral("settings")).toObject();
            for (auto iterator = storedSettings.constBegin();
                 iterator != storedSettings.constEnd();
                 ++iterator) {
                if (registration->defaultSettings.find(
                            iterator.key().toStdString()) ==
                    registration->defaultSettings.end()) {
                    continue;
                }
                settings.insert(iterator.key(), iterator.value().toString());
            }
            modifierPasses_.push_back(
                    ModifierPassValue(*registration, settings));
        }
    }
    if (!modifierPasses_.isEmpty()) return;

    const ModifierRegistration &fallback = ModifierRegistry().front();
    const QString legacyId = StoredValue(
            kLegacyMutationAlgorithmKey,
            QString::fromStdString(fallback.id));
    const ModifierRegistration *registration =
            FindModifier(legacyId.toStdString());
    if (registration == nullptr) registration = &fallback;
    modifierPasses_.push_back(ModifierPassValue(
            *registration,
            LoadOptionSettings(QStringLiteral("mutation"), *registration)));
    persistModifierPasses();
}

void SearchConfigurationModel::persistModifierPasses() const {
    QJsonArray array;
    for (const QVariant &value : modifierPasses_) {
        const QVariantMap pass = value.toMap();
        QJsonObject settings;
        const QVariantMap values =
                pass.value(QStringLiteral("settings")).toMap();
        for (auto iterator = values.constBegin(); iterator != values.constEnd();
             ++iterator) {
            settings.insert(iterator.key(), iterator.value().toString());
        }
        array.push_back(QJsonObject{
                {QStringLiteral("id"),
                 pass.value(QStringLiteral("id")).toString()},
                {QStringLiteral("settings"), settings}});
    }
    QSettings().setValue(
            QLatin1String(kModifierPassesKey),
            QJsonDocument(array).toJson(QJsonDocument::Compact));
}

void SearchConfigurationModel::loadEvaluationTargetSettings() {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(evaluationTargetId_.toStdString());
    evaluationTargetSettings_ = registration == nullptr
            ? QVariantMap{}
            : LoadOptionSettings(QStringLiteral("evaluation"), *registration);
}

void SearchConfigurationModel::persistOptionSetting(
        const QString &category,
        const QString &optionId,
        const QString &key,
        const QString &value) const {
    QSettings().setValue(OptionSettingPath(category, optionId, key), value);
}

}  // namespace forevertas::app
