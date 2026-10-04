#include "mutations/input_event_formatter.h"
#include <forevervalidator/experimental/search_limits.h>

#include "input_timeline_time.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace forevertas {
namespace {

using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

constexpr std::int64_t kInputScriptTickMs =
        static_cast<std::int64_t>(kInputTimelineTickDurationMs);

std::string_view Trim(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1u);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1u);
    }
    return value;
}

std::string Lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

std::vector<std::string_view> Tokens(std::string_view line) {
    std::vector<std::string_view> result;
    while (true) {
        line = Trim(line);
        if (line.empty()) return result;
        const std::size_t end = line.find_first_of(" \t\r\n");
        result.push_back(line.substr(0u, end));
        if (end == std::string_view::npos) return result;
        line.remove_prefix(end);
    }
}

std::optional<std::int64_t> ParseScriptTime(std::string_view token) {
    if (token.empty() || token.front() == '-' || token.front() == '+') {
        return std::nullopt;
    }
    const std::size_t decimal = token.find('.');
    if (decimal != std::string_view::npos &&
        token.find('.', decimal + 1u) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view wholeToken = token.substr(0u, decimal);
    const std::string_view fractionToken =
            decimal == std::string_view::npos
            ? std::string_view{}
            : token.substr(decimal + 1u);
    if (wholeToken.empty() ||
        (decimal != std::string_view::npos && fractionToken.empty()) ||
        fractionToken.size() > 3u) {
        return std::nullopt;
    }
    const auto allDigits = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](char character) {
            return std::isdigit(static_cast<unsigned char>(character)) != 0;
        });
    };
    if (!allDigits(fractionToken)) {
        return std::nullopt;
    }

    std::int64_t seconds = 0;
    std::string_view remaining = wholeToken;
    for (std::size_t componentIndex = 0u; ; ++componentIndex) {
        const std::size_t colon = remaining.find(':');
        const std::string_view part = remaining.substr(0u, colon);
        if (componentIndex >= 3u || part.empty() || !allDigits(part)) {
            return std::nullopt;
        }
        std::int64_t component = 0;
        const auto parsed = std::from_chars(
                part.data(), part.data() + part.size(), component);
        constexpr auto limit = std::numeric_limits<std::int64_t>::max() / 1000;
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
            component > limit || (componentIndex > 0u && component >= 60) ||
            seconds > (limit - component) / 60) {
            return std::nullopt;
        }
        seconds = seconds * 60 + component;
        if (colon == std::string_view::npos) break;
        remaining.remove_prefix(colon + 1u);
    }
    std::int64_t milliseconds = 0;
    if (!fractionToken.empty()) {
        const auto fractionParsed = std::from_chars(
                fractionToken.data(),
                fractionToken.data() + fractionToken.size(),
                milliseconds);
        if (fractionParsed.ec != std::errc{} ||
            fractionParsed.ptr != fractionToken.data() +
                    fractionToken.size()) {
            return std::nullopt;
        }
        for (std::size_t index = fractionToken.size(); index < 3u; ++index) {
            milliseconds *= 10;
        }
    }
    if (seconds > (std::numeric_limits<std::int64_t>::max() - milliseconds) / 1000) {
        return std::nullopt;
    }
    const std::int64_t result = seconds * 1000 + milliseconds;
    if (result % kInputScriptTickMs != 0) return std::nullopt;
    return result;
}

std::optional<AnalogInputState> ParseAnalog(std::string_view token) {
    std::int64_t parsedValue = 0;
    const auto parsed = std::from_chars(
            token.data(), token.data() + token.size(), parsedValue);
    if (token.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != token.data() + token.size() ||
        parsedValue < kAnalogInputMinimum ||
        parsedValue > kAnalogInputMaximum) {
        return std::nullopt;
    }
    return static_cast<AnalogInputState>(parsedValue);
}

std::optional<SandboxInputAction> SwitchAction(std::string_view token) {
    const std::string action = Lower(token);
    if (action == "up") return SandboxInputAction::Accelerate;
    if (action == "down") return SandboxInputAction::Brake;
    if (action == "left") return SandboxInputAction::SteerLeft;
    if (action == "right") return SandboxInputAction::SteerRight;
    if (action == "enter") return SandboxInputAction::Respawn;
    return std::nullopt;
}

std::string LineError(std::size_t line, std::string message) {
    return "Line " + std::to_string(line) + ": " + std::move(message);
}

std::string FormatMilliseconds(std::int64_t timeMs) {
    const bool negative = timeMs < 0;
    const std::uint64_t magnitude = negative
            ? static_cast<std::uint64_t>(-(timeMs + 1)) + 1u
            : static_cast<std::uint64_t>(timeMs);
    const std::uint64_t hundredths = magnitude / 10u;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    if (negative) output << '-';
    output << hundredths / 100u << '.' << std::setfill('0')
           << std::setw(2) << hundredths % 100u << " s";
    return output.str();
}

bool IsStructuralAction(SandboxInputAction action) {
    return action == SandboxInputAction::RaceRunning ||
            action == SandboxInputAction::FinishLine ||
            action == SandboxInputAction::Unmapped;
}

std::string FormatTime(std::int32_t timeMs,
                       std::int32_t startTimeMs) {
    const std::int64_t userTimeMs = UserTimelineTimeFromSimulationTime(
            static_cast<std::int64_t>(timeMs),
            static_cast<std::uint32_t>(kInputScriptTickMs),
            static_cast<std::int64_t>(startTimeMs));
    const std::int64_t scriptTimeMs = userTimeMs == 0
            ? 0
            : ((userTimeMs + kInputScriptTickMs - 1) /
               kInputScriptTickMs) * kInputScriptTickMs;
    const std::int64_t hundredths = scriptTimeMs / 10;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << hundredths / 100 << '.' << std::setfill('0')
           << std::setw(2) << hundredths % 100;
    return output.str();
}

std::string_view SwitchCommand(
        forevervalidator::experimental::PhysicsSandboxInputAction action) {
    switch (action) {
    case SandboxInputAction::Accelerate:
        return "up";
    case SandboxInputAction::Brake:
        return "down";
    case SandboxInputAction::SteerLeft:
        return "left";
    case SandboxInputAction::SteerRight:
        return "right";
    case SandboxInputAction::Respawn:
        return "enter";
    case SandboxInputAction::Unmapped:
    case SandboxInputAction::Gas:
    case SandboxInputAction::Steer:
    case SandboxInputAction::RaceRunning:
    case SandboxInputAction::FinishLine:
        return {};
    }
    return {};
}

}  // namespace

InputScriptParseResult ParseInputScript(std::string_view script) {
    constexpr std::size_t maximumScriptBytes =
            forevervalidator::experimental::kMaximumSearchInputEvents * 128u;
    if (script.size() > maximumScriptBytes)
        return {{}, "Input script exceeds the 128 MiB limit; reduce input density or comments."};
    InputScriptParseResult result;
    std::size_t lineNumber = 1u;
    while (true) {
        const std::size_t newline = script.find('\n');
        std::string_view line = script.substr(0u, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1u);

        const std::size_t hashComment = line.find('#');
        const std::size_t slashComment = line.find("//");
        const std::size_t comment = std::min(hashComment, slashComment);
        if (comment != std::string_view::npos) line = line.substr(0u, comment);
        const std::vector<std::string_view> tokens = Tokens(line);
        if (!tokens.empty()) {
            if (tokens.size() != 3u) {
                result.error = LineError(
                        lineNumber,
                        "expected '<time> <command> <value>'.");
                return result;
            }
            const std::optional<std::int64_t> time =
                    ParseScriptTime(tokens[0]);
            if (!time) {
                result.error = LineError(
                        lineNumber,
                        "time must be non-negative and 10 ms-aligned: "
                        "seconds, mm:ss.mmm or hh:mm:ss.mmm.");
                return result;
            }

            ParsedInputCommand command;
            command.userTimeMs = *time;
            command.sourceLine = lineNumber;
            const std::string verb = Lower(tokens[1]);
            if (verb == "steer" || verb == "gas") {
                const std::optional<AnalogInputState> analog =
                        ParseAnalog(tokens[2]);
                if (!analog) {
                    result.error = LineError(
                            lineNumber,
                            "analog value must be an integer in "
                            "[-65536, 65536].");
                    return result;
                }
                command.action = verb == "steer"
                        ? SandboxInputAction::Steer
                        : SandboxInputAction::Gas;
                command.value.kind = PhysicsSandboxInputValueKind::Analog;
                command.value.analog = *analog;
            } else if (verb == "press" || verb == "rel" ||
                       verb == "release") {
                const std::optional<SandboxInputAction> action =
                        SwitchAction(tokens[2]);
                if (!action) {
                    result.error = LineError(
                            lineNumber,
                            "switch must be up, down, left, right, or enter.");
                    return result;
                }
                const bool pressed = verb == "press";
                if (*action == SandboxInputAction::Respawn && !pressed) {
                    result.error = LineError(
                            lineNumber,
                            "enter only supports the press command.");
                    return result;
                }
                command.action = *action;
                command.value.kind = PhysicsSandboxInputValueKind::Switch;
                command.value.switchState = pressed
                        ? PhysicsSandboxSwitchState::Pressed
                        : PhysicsSandboxSwitchState::Released;
            } else {
                result.error = LineError(
                        lineNumber,
                        "command must be press, rel, release, steer, or gas.");
                return result;
            }
            if (result.commands.size() >= forevervalidator::experimental::kMaximumSearchInputEvents)
                return {{}, LineError(lineNumber,
                        "input script exceeds the 1048576-command limit; reduce input density.")};
            result.commands.push_back(command);
        }

        if (newline == std::string_view::npos) break;
        script.remove_prefix(newline + 1u);
        ++lineNumber;
    }
    return result;
}

InputScriptBaselineResult BuildInputScriptBaseline(
        const std::vector<SandboxInputEvent> &fixedInputs,
        const std::vector<ParsedInputCommand> &commands,
        std::uint32_t tickDurationMs) {
    InputScriptBaselineResult result;
    const auto structuralCount = static_cast<std::size_t>(std::count_if(
            fixedInputs.begin(), fixedInputs.end(), [](const SandboxInputEvent &event) {
                return IsStructuralAction(event.action);
            }));
    constexpr auto limit = forevervalidator::experimental::kMaximumSearchInputEvents;
    if (structuralCount > limit || commands.size() > limit - structuralCount)
        return {{}, "Baseline exceeds the 1048576-event limit; reduce input density."};
    std::int64_t originMs = 0;
    for (const SandboxInputEvent &event : fixedInputs) {
        if (event.action == SandboxInputAction::RaceRunning) {
            originMs = event.timeMs;
            break;
        }
    }

    struct MaterializedCommand {
        SandboxInputEvent event;
        std::size_t sourceLine = 0u;
    };
    std::vector<MaterializedCommand> materialized;
    materialized.reserve(commands.size());
    for (const ParsedInputCommand &command : commands) {
        const std::optional<std::int64_t> translated =
                SimulationTimelineTimeFromUserTime(
                        command.userTimeMs, tickDurationMs, originMs);
        if (!translated ||
            *translated > std::numeric_limits<std::int32_t>::max()) {
            result.error = LineError(
                    command.sourceLine,
                    "input time " + FormatMilliseconds(command.userTimeMs) +
                            " cannot be represented on the simulation "
                            "timeline.");
            return result;
        }
        materialized.push_back({
                {static_cast<std::int32_t>(*translated),
                 command.action,
                 command.value},
                command.sourceLine});
    }
    std::stable_sort(
            materialized.begin(),
            materialized.end(),
            [](const MaterializedCommand &left,
               const MaterializedCommand &right) {
                return left.event.timeMs < right.event.timeMs;
            });

    std::vector<SandboxInputEvent> controls;
    controls.reserve(materialized.size());
    std::size_t groupStart = 0u;
    for (const MaterializedCommand &command : materialized) {
        if (controls.empty() || controls.back().timeMs != command.event.timeMs) {
            groupStart = controls.size();
        }
        auto duplicate = std::find_if(
                controls.begin() + groupStart,
                controls.end(),
                [&command](const SandboxInputEvent &existing) {
                    return existing.timeMs == command.event.timeMs &&
                            existing.action == command.event.action;
                });
        if (duplicate != controls.end()) {
            *duplicate = command.event;
        } else {
            controls.push_back(command.event);
        }
    }

    result.events.reserve(fixedInputs.size() + controls.size());
    for (const SandboxInputEvent &event : fixedInputs) {
        if (event.timeMs < originMs || IsStructuralAction(event.action)) {
            result.events.push_back(event);
        }
    }
    result.events.insert(
            result.events.end(), controls.begin(), controls.end());
    std::stable_sort(
            result.events.begin(),
            result.events.end(),
            [](const SandboxInputEvent &left,
               const SandboxInputEvent &right) {
                return left.timeMs < right.timeMs;
            });
    return result;
}

std::string FormatInputScript(
        const std::vector<SandboxInputEvent> &events,
        std::optional<std::uint64_t> lastSimulationInputTimeMs) {
    std::vector<const SandboxInputEvent *> sortedEvents;
    sortedEvents.reserve(events.size());
    for (const SandboxInputEvent &event : events) {
        sortedEvents.push_back(&event);
    }
    std::stable_sort(
            sortedEvents.begin(),
            sortedEvents.end(),
            [](const SandboxInputEvent *left,
               const SandboxInputEvent *right) {
                return left->timeMs < right->timeMs;
            });

    std::int32_t startTimeMs = 0;
    for (const SandboxInputEvent *const event : sortedEvents) {
        if (event->action == SandboxInputAction::RaceRunning) {
            startTimeMs = event->timeMs;
            break;
        }
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    bool firstLine = true;
    for (const SandboxInputEvent *const eventPointer : sortedEvents) {
        const SandboxInputEvent &event = *eventPointer;
        if (event.timeMs < startTimeMs) {
            continue;
        }
        if (lastSimulationInputTimeMs &&
            static_cast<std::uint64_t>(event.timeMs) > *lastSimulationInputTimeMs) break;
        if (event.action == SandboxInputAction::FinishLine) {
            if (!lastSimulationInputTimeMs) break;
            continue;
        }
        std::string command;
        if ((event.action == SandboxInputAction::Steer ||
             event.action == SandboxInputAction::Gas) &&
            event.value.kind == PhysicsSandboxInputValueKind::Analog) {
            if (!forevervalidator::IsAnalogInputStateValid(
                        event.value.analog)) {
                throw std::invalid_argument(
                        "analog input state is outside [-65536, 65536]");
            }
            command = event.action == SandboxInputAction::Steer
                    ? "steer "
                    : "gas ";
            command += std::to_string(event.value.analog);
        } else if (event.value.kind ==
                   PhysicsSandboxInputValueKind::Switch) {
            const std::string_view action = SwitchCommand(event.action);
            if (action.empty()) {
                continue;
            }
            const bool pressed = event.value.switchState !=
                    PhysicsSandboxSwitchState::Released;
            if (event.action == SandboxInputAction::Respawn && !pressed) {
                continue;
            }
            command = pressed ? "press " : "rel ";
            command.append(action);
        } else {
            continue;
        }

        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;
        output << FormatTime(event.timeMs, startTimeMs) << ' ' << command;
    }
    return output.str();
}

}  // namespace forevertas
