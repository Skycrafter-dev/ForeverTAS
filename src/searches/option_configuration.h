#ifndef FOREVERTAS_SEARCHES_OPTION_CONFIGURATION_H
#define FOREVERTAS_SEARCHES_OPTION_CONFIGURATION_H

#include <map>
#include <string>

namespace forevertas {

using OptionSettings = std::map<std::string, std::string>;

struct OptionConfiguration {
    std::string id;
    OptionSettings settings;
};

inline bool operator==(const OptionConfiguration &lhs,
                       const OptionConfiguration &rhs) {
    return lhs.id == rhs.id && lhs.settings == rhs.settings;
}

}  // namespace forevertas

#endif
