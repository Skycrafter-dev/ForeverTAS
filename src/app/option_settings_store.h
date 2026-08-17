#ifndef FOREVERTAS_APP_OPTION_SETTINGS_STORE_H
#define FOREVERTAS_APP_OPTION_SETTINGS_STORE_H

#include "searches/option_configuration.h"

#include <QSettings>
#include <QString>
#include <QVariantMap>

#include <string>

namespace forevertas::app {

// Legacy per-option settings persistence ("configuration/<category>/
// <optionId>/<key>"). Used to migrate pre-block configurations and to
// seed the persistent target collections.
template<typename Registration>
QVariantMap LoadPersistedOptionSettings(const QString &category,
                                        const Registration &registration) {
    QSettings storage;
    QVariantMap values;
    for (const auto &[key, defaultValue] : registration.defaultSettings) {
        const QString qKey = QString::fromStdString(key);
        const QString path = QStringLiteral("configuration/%1/%2/%3")
                                     .arg(category,
                                          QString::fromStdString(
                                                  registration.id),
                                          qKey);
        QString value;
        bool loaded = false;
        if (storage.contains(path)) {
            value = storage.value(path).toString();
            loaded = true;
        } else {
            for (const std::string &legacyId : registration.legacyIds) {
                const QString legacyPath = QStringLiteral(
                                                   "configuration/%1/%2/%3")
                                                   .arg(category,
                                                        QString::fromStdString(
                                                                legacyId),
                                                        qKey);
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
                storage.contains(
                        QString::fromStdString(legacyKey->second))) {
                value = storage.value(
                                QString::fromStdString(
                                        legacyKey->second))
                                .toString();
                // Persist the migrated value like the legacy-id path
                // so the setting survives the old key disappearing.
                storage.setValue(path, value);
                loaded = true;
            } else {
                value = QString::fromStdString(defaultValue);
            }
        }
        values.insert(qKey, value);
    }
    return values;
}

inline OptionSettings ToOptionSettings(const QVariantMap &values) {
    OptionSettings settings;
    for (auto iterator = values.constBegin(); iterator != values.constEnd();
         ++iterator) {
        settings.emplace(iterator.key().toStdString(),
                         iterator.value().toString().toStdString());
    }
    return settings;
}

}  // namespace forevertas::app

#endif
