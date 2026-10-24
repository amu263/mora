#pragma once
#include "core.hpp"
namespace mora {
    struct CommandSpec {
        std::wstring name, usage, description;
    };
    struct Invocation {
        std::wstring verb, args;
    };
    const std::vector<CommandSpec>& commandRegistry();
    Invocation parseCommand(std::wstring_view text);
    std::vector<CommandSpec> commandSuggestions(std::wstring_view input);
} // namespace mora
