#ifndef FOREVERTAS_APP_SEARCH_CONFIGURATION_MODEL_H
#define FOREVERTAS_APP_SEARCH_CONFIGURATION_MODEL_H

#include "searches/search_runner.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <optional>
#include <vector>

namespace forevertas::app {

struct SearchComponentConfiguration {
    OptionConfiguration searchAlgorithm;
    std::vector<OptionConfiguration> modifiers;
    OptionConfiguration evaluationTarget;
};

struct SearchConfigurationValidation {
    std::optional<SearchComponentConfiguration> configuration;
    QString error;
};

class SearchConfigurationModel final {
public:
    SearchConfigurationModel();

    QVariantList searchAlgorithmOptions() const;
    QVariantList modifierOptions() const;
    QVariantList evaluationTargetOptions() const;

    QString searchAlgorithmId() const;
    QString evaluationTargetId() const;
    QVariantMap searchAlgorithmSettings() const;
    QVariantList modifierPasses() const;
    QVariantMap evaluationTargetSettings() const;
    QVariantMap evaluationTargetSettingsFor(const QString &id) const;

    bool setSearchAlgorithmId(const QString &value);
    bool setEvaluationTargetId(const QString &value);
    bool setSearchAlgorithmSetting(const QString &key, const QString &value);
    bool setEvaluationTargetSetting(const QString &key,
                                    const QString &value,
                                    bool persist = true);

    bool addModifierPass(const QString &id);
    bool removeModifierPass(int index);
    bool moveModifierPass(int fromIndex, int toIndex);
    bool setModifierPassId(int index, const QString &id);
    bool setModifierPassSetting(int index,
                                const QString &key,
                                const QString &value);
    bool randomizeModifierSeeds(std::uint32_t entropy);

    SearchConfigurationValidation validate(
            std::uint32_t tickDurationMs,
            std::uint32_t simulationHorizonMs) const;

private:
    void loadSearchAlgorithmSettings();
    void loadModifierPasses();
    void persistModifierPasses() const;
    void loadEvaluationTargetSettings();
    void persistOptionSetting(const QString &category,
                              const QString &optionId,
                              const QString &key,
                              const QString &value) const;

    QString searchAlgorithmId_;
    QString evaluationTargetId_;
    QVariantMap searchAlgorithmSettings_;
    QVariantList modifierPasses_;
    QVariantMap evaluationTargetSettings_;
};

}  // namespace forevertas::app

#endif
