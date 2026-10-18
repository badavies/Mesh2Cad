// SPDX-License-Identifier: GPL-3.0-or-later
#include <exception>
#include <ranges>

#include "m2c/commands/Command.h"

namespace m2c {

Result<void> CommandRegistry::add(Command command) {
    if (command.name.empty()) {
        return fail(ErrorCode::InvalidArgument, "command name must not be empty");
    }
    if (!command.handler) {
        return fail(ErrorCode::InvalidArgument, "command '" + command.name + "' has no handler");
    }
    if (const std::string name = command.name;
        !commands_.try_emplace(name, std::move(command)).second) {
        return fail(ErrorCode::InvalidArgument, "command '" + name + "' is already registered");
    }
    return {};
}

const Command* CommandRegistry::find(const std::string_view name) const {
    const auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

std::vector<const Command*> CommandRegistry::list() const {
    std::vector<const Command*> result;
    result.reserve(commands_.size());
    for (const Command& command : std::views::values(commands_)) result.push_back(&command);
    return result;
}

Result<Json> CommandRegistry::execute(CommandContext& context,
                                      const std::string_view name,
                                      const Json& params) const {
    const Command* command = find(name);
    if (command == nullptr) {
        return fail(ErrorCode::NotFound, "unknown command '" + std::string(name) + "'");
    }
    if (!params.is_object() && !params.is_null()) {
        return fail(ErrorCode::InvalidArgument, "parameters must be a JSON object");
    }
    const Json effective = params.is_null() ? Json::object() : params;

    const bool step = command->mutatesDocument && command->opensUndoStep;
    if (step) context.document.beginStep(command->name, context.source);
    context.currentCommand = command->name;
    context.currentParams = effective;

    Result<Json> result = fail(ErrorCode::Internal, "command did not run");
    try {
        result = command->handler(context, effective);
    } catch (const Json::exception& e) {
        result = fail(ErrorCode::InvalidArgument,
                      "invalid parameters for '" + command->name + "': " + e.what());
    } catch (const std::exception& e) {
        result = fail(ErrorCode::Internal, "command '" + command->name + "' failed: " + e.what());
    }

    if (step) {
        if (result) {
            context.document.commitStep();
        } else {
            context.document.abortStep();
        }
    }
    context.currentCommand.clear();
    context.currentParams = Json();
    if (result && command->mutatesDocument && context.sessionLog) {
        context.sessionLog(
            Json{{"command", command->name}, {"params", effective}, {"source", context.source}});
    }
    return result;
}

} // namespace m2c
