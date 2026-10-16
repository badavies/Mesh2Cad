# cli (executable `mesh2cad`)

The command-line front end, for headless scripts and golden tests. It adds no capability of its
own: it creates a `CommandRegistry` and a `Document` and runs registered commands.

## Public API

No library or headers. The executable is built from `main.cpp`:

- `mesh2cad list`: lists commands and their parameter schemas (runs `commands.list`).
- `mesh2cad run <command> [params-json]`: runs one command on an empty document.
- `mesh2cad batch [file]`: runs JSON lines `{"command": ..., "params": {...}}` from a file or
  stdin on one shared document, stopping at the first failure.

Each command prints one line, `{"ok":true,"result":...}` or `{"ok":false,"error":{...}}`.
Exit status is 0 on success, 1 if a command failed and 2 for a usage error.

## Dependencies

May use: `core`, `document`, `commands`.
Must not depend on: `adapters`, `io`, `pipeline`, `app`, `rpc`, `mcp`, Qt, and kernel library
headers (CGAL, Open CASCADE, OpenSubdiv). Enforced by `scripts/check_layers.py`.

## Notes

- `scripts/gen_commands.py` reads `mesh2cad list` to write `docs/COMMANDS.md`, so keep that
  output format stable.
- New behaviour belongs in a command, not here.
