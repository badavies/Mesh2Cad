# Automation and AI agents

Everything in the app is a command, and every command can also be run from the command line, from a script over JSON-RPC, or by an AI agent over MCP. The results are the same whichever way you run it. The [command reference](../COMMANDS.md) lists each command's parameters.

## Command line

```
mesh2cad list                                        # every command and its parameters
mesh2cad run mesh.info '{"id": 1}'                   # one command on an empty document
mesh2cad batch session.jsonl                         # a session of commands on one document
```

A session file has one command per line, like the examples in this guide. Each command prints one line of output: `{"ok": true, "result": ...}` or `{"ok": false, "error": {...}}`. The exit status is 0 when every command succeeds and 1 when one fails. The app keeps a log of every change in the same format, so a session recorded in the app can be replayed with `mesh2cad batch`. **Help > Connect an AI agent** shows where the log is.

## JSON-RPC

`mesh2cad-rpc` is a headless engine that reads JSON-RPC 2.0 requests on its input, one per line, and writes replies. Each command is a method of the same name:

```
{"jsonrpc": "2.0", "id": 1, "method": "mesh.import", "params": {"path": "bracket.stl"}}
{"jsonrpc": "2.0", "id": 2, "method": "mesh.info", "params": {"id": 1}}
```

While a long command runs, the engine sends `rpc.progress` notifications. `rpc.cancel` with the request's id stops it.

While the desktop app is open, it serves the same protocol on a local socket (a named pipe on Windows), so a script can work on the document you can see. Connections are local only, and each one must first send the per-session token from the session file, which only your user can read.

## MCP for AI agents

`mesh2cad-mcp` is an MCP server, so Claude and other agents can use every command as a tool. In tool names the dots become underscores: `mesh.repair` is `mesh_repair`. If the app is open, the agent works on the app's document: its changes show in the viewport and in **History** with the source `mcp`, and **Edit > Undo** reverts them. Without the app it works on a private document. `view.render` returns a picture of the document, so the agent can see what it has made.

For Claude Desktop, add this to `claude_desktop_config.json` and restart Claude Desktop (the file is in `%APPDATA%\Claude` on Windows and `~/.config/Claude` on Linux):

```json
{
  "mcpServers": {
    "mesh2cad": { "command": "C:\\Program Files\\Mesh2Cad\\bin\\mesh2cad-mcp.exe" }
  }
}
```

On Linux the command is `/usr/bin/mesh2cad-mcp`. For Claude Code, run `claude mcp add mesh2cad -- mesh2cad-mcp`. Tools that change the document or write a file are marked that way, so a client that asks before such steps will ask you first.
