#ifndef FOREVERTAS_APP_OPTION_SETTINGS_STORE_H
#define FOREVERTAS_APP_OPTION_SETTINGS_STORE_H

#include "searches/option_configuration.h"

#include <QSettings>
#include <QString>
#include <QVariantMap>

#include <string>

namespace forevertas::app {

// Exact per-option settings format written by ForeverTAS v0.2.3.
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
        const QString value = storage.contains(path)
                ? storage.value(path).toString()
                : QString::fromStdString(defaultValue);
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
