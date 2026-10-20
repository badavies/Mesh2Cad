// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "m2c/commands/Command.h"

namespace m2c::cmd {

// Each file of src/commands contributes one group of commands.
void addDocumentCommands(const CommandRegistry& registry, std::vector<Command>& out);
void addMeshToolCommands(std::vector<Command>& out);
void addShellCommands(std::vector<Command>& out);
void addSelectionCommands(std::vector<Command>& out);
void addSegmentCommands(std::vector<Command>& out);
void addPrimitiveCommands(std::vector<Command>& out);
void addAlignMeasureCommands(std::vector<Command>& out);
void addProjectCommands(std::vector<Command>& out);
void addQuadCommands(std::vector<Command>& out);
void addSurfaceCommands(std::vector<Command>& out);
void addSketchCommands(std::vector<Command>& out);
void addDeviationCommands(std::vector<Command>& out);

} // namespace m2c::cmd
