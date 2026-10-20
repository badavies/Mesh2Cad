# 0029. Command schemas name the object kinds an id accepts

- Status: Accepted
- Date: 2026-10-20

## Context
Tool forms in the desktop app are generated from command schemas (#69). An object id was a plain integer, so the form showed a number box. People had to type ids, and a wrong kind only failed when the command ran ("object 1 is a mesh, not a primitive"). The app guessed which objects to pre-fill from a hand-kept table of command names.

## Decision
Each parameter that names an object carries `"x-object"` in its schema: one kind (`"mesh"`, `"selection"`, `"primitive"`, `"solid"`, `"sketch"`, `"quad_layout"`, `"deviation"`, `"measurement"`), an array of kinds in order of preference, or `"any"`. The app builds a drop-down of the document's objects of those kinds and pre-fills from it. `docs/COMMANDS.md` shows the kind in the type column. A command test requires the annotation on every `id` and `*_id` parameter.

## Alternatives considered
- Infer the kind in the app from the parameter and command names: keeps schemas unchanged, but the table drifts as commands are added, and agents don't get the information.
- Put the kind in `description`: readable, but not something a form can use.

## Consequences
- Agents see the same kinds in `commands.list` and the MCP tool schemas. `x-` keywords are ignored by JSON Schema validators.
- A new object kind means updating the test's list of kinds.
- Handlers still check the kind themselves. The annotation describes the parameter; it is not enforced by the dispatcher.
