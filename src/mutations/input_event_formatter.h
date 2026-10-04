#ifndef FOREVERTAS_MUTATIONS_INPUT_EVENT_FORMATTER_H
#define FOREVERTAS_MUTATIONS_INPUT_EVENT_FORMATTER_H

#include "mutations/input_event_utils.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forevertas {

struct ParsedInputCommand {
    std::int64_t userTimeMs = 0;
    SandboxInputAction action = SandboxInputAction::Unmapped;
    forevervalidator::experimental::PhysicsSandboxInputValue value{};
    std::size_t sourceLine = 0u;
};

struct InputScriptParseResult {
    std::vector<ParsedInputCommand> commands;
    std::optional<std::string> error;

    explicit operator bool() const noexcept {
        return !error.has_value();
    }
};

struct InputScriptBaselineResult {
    std::vector<SandboxInputEvent> events;
    std::optional<std::string> error;

    explicit operator bool() const noexcept {
        return !error.has_value();
    }
};

InputScriptParseResult ParseInputScript(std::string_view script);

InputScriptBaselineResult BuildInputScriptBaseline(
        const std::vector<SandboxInputEvent> &fixedInputs,
        const std::vector<ParsedInputCommand> &commands,
        std::uint32_t tickDurationMs);

std::string FormatInputScript(
        const std::vector<SandboxInputEvent> &events,
        std::optional<std::uint64_t> lastSimulationInputTimeMs = std::nullopt);

}  // namespace forevertas

#endif
