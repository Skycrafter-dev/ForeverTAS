#ifndef FOREVERTAS_SEARCHES_SETTING_LABELS_H
#define FOREVERTAS_SEARCHES_SETTING_LABELS_H

// Settings by the names the window shows them under, so a refused setting is
// named as the user sees it.

#include "searches/option_configuration.h"

#include <functional>
#include <optional>
#include <string>

namespace forevertas {

// The window's name for a component's setting ("" when it has none).
std::string SettingLabel(const std::string &componentId, const std::string &key);

// Why a component refuses its settings, in plain words. When its own reason
// is that a setting could not be read, the setting at fault is found (each
// one put back to its default in turn) and named with its value.
std::string ExplainSettingsProblem(
        const std::string &componentId,
        const OptionSettings &settings,
        const OptionSettings &defaults,
        const std::string &problem,
        const std::function<std::optional<std::string>(const OptionSettings &)> &validate);

}  // namespace forevertas

#endif
