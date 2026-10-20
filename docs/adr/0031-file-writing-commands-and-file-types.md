# 0031. Commands that write files say so and name their file types

- Status: Accepted
- Date: 2026-10-20

## Context
A scan was turned into a STEP file by an agent driving the desktop app over MCP. The app's log only showed commands that change the document, so `solid.export` and `project.save` never appeared and the person could not tell whether the files had been written. The MCP server marked the same tools read-only (`readOnlyHint`), although they can replace a file on disk.

The tool forms guessed the file dialog from the command name: projects got `*.m2c` and everything else meshes. Export solid therefore offered only `.stl`, `.obj` and `.ply`, a typed name became `.stl`, and `solid.export` refused it. `view.render` and `deviation.report`, whose names contain neither "export" nor "save", opened an Open dialog for the file they write.

## Decision
- `Command` gains `writesFiles`: the command writes the file named by its `path` parameter (an export, a save, a picture, a report). It stays independent of `mutatesDocument`; a command can do either, both or neither. `commands.list` publishes it as `writes_files`.
- Every `path` parameter carries `"x-file-types"` in its schema: the extensions it takes, without the dot, the preferred one first (`["step", "stp"]`). The app builds its file dialog filter from the list, opens a Save dialog when the command writes files and an Open dialog otherwise. `docs/COMMANDS.md` lists the types.
- The app logs a command that writes files, with the path, like a mutating one. The MCP server marks such a tool neither read-only nor non-destructive and says in its description that it replaces any file already at the path.
- Command tests require `x-file-types` on every `path` parameter, a required `path` on every command that writes files, and the exact set of file-writing commands.

## Alternatives considered
- Keep inferring from command names in the app: no schema change, but it was already wrong for three of the seven commands with a path, and agents learn nothing from it.
- Put the extensions only in `description`: readable for people and agents, but not something a form can use.
- One `kind` enum (query, mutation, file write) instead of a second flag: tidier, but `project.open` reads a file and changes the document, and a later command may both change the document and write a file.

## Consequences
- A new command that takes a `path` must name its file types, and one that writes a file must set `writesFiles`; the tests catch both omissions.
- `x-` keywords are ignored by JSON Schema validators, as with `x-object` (ADR-029).
- The engine still checks extensions itself (`solid.export` refuses anything but `.step` and `.stp`); the annotation describes the parameter, it does not enforce it.
