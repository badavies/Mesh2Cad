// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "m2c/core/Error.h"
#include "m2c/document/Document.h"

namespace m2c {

using Json = nlohmann::json;

/// What a command may touch. Grows (job queue, progress) as later milestones need it.
struct CommandContext {
    Document& document;
    /// Who is issuing commands: "ui", "cli", "json", "mcp", ... Recorded on every undo step and
    /// in the session log, so a person can see what an AI agent changed.
    std::string source{"api"};
    /// Called after every successful mutating command with {"command", "params", "source"}: the
    /// session log. Replaying the logged lines reproduces the session (ARCHITECTURE.md section 8).
    std::function<void(const Json&)> sessionLog{};

    // Set by CommandRegistry::execute while a handler runs.
    std::string currentCommand{};
    Json currentParams{};

    /// Provenance record for an object the running command creates or changes.
    [[nodiscard]] Provenance provenance(std::vector<ObjectId> inputs = {}) const {
        return Provenance{currentCommand, currentParams.dump(), std::move(inputs)};
    }
};

/// One user-visible action. The CLI, the JSON channel, the MCP server and the UI all run commands
/// through the registry; no capability exists outside it (ARCHITECTURE.md section 8).
struct Command {
    std::string name;    ///< Dotted and lower-case, e.g. "mesh.import".
    std::string summary; ///< One sentence, shown in `commands.list` and in generated docs.
    /// JSON Schema (draft 2020-12) describing the parameters object, published by `commands.list`
    /// for the MCP server and generated docs. It is not enforced by the dispatcher yet: handlers
    /// validate their own parameters. Automatic validation needs json-schema-validator (an ADR).
    Json paramsSchema;
    bool mutatesDocument{false};
    std::function<Result<Json>(CommandContext&, const Json& params)> handler;
    /// Mutating commands run inside one document undo step, so whatever they change is undone
    /// together. Only commands that manage the history themselves (undo, redo, open) turn this off.
    bool opensUndoStep{true};
};

class CommandRegistry {
public:
    CommandRegistry() = default;
    // Built-in handlers hold a pointer to their registry, so it must not move or be copied.
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    /// Fails with InvalidArgument if the name is empty, has no handler, or is already registered.
    [[nodiscard]] Result<void> add(Command command);

    [[nodiscard]] const Command* find(std::string_view name) const;

    /// All commands sorted by name.
    [[nodiscard]] std::vector<const Command*> list() const;

    /// Looks up and runs a command. `params` must be a JSON object (null counts as `{}`).
    /// Exceptions from handlers (including JSON type errors) are turned into Errors. A failed
    /// mutating command leaves the document unchanged.
    [[nodiscard]] Result<Json>
    execute(CommandContext& context, std::string_view name, const Json& params) const;

private:
    std::map<std::string, Command, std::less<>> commands_;
};

/// Registers every built-in command (`mesh2cad list` or docs/COMMANDS.md shows them all).
/// The registry must outlive the registered commands (it always does: they live inside it).
[[nodiscard]] Result<void> registerBuiltinCommands(CommandRegistry& registry);

} // namespace m2c
