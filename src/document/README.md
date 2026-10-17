# document (`Mesh2Cad::document`)

The objects the user is working on, with provenance and linear undo and redo. It is a pure state
container: it never calls processing code. The command engine opens one undo step per mutating
command, so every command is one step whatever it changes.

## Public API

- `m2c/document/Document.h`:
  - `Document`: the object table and history.
    - Steps: `beginStep`, `commitStep`, `abortStep`.
    - Changes: `addMesh`, `add`, `replaceMesh`, `replaceSelection`, `replaceMeasurement`, `remove`, `rename`, `setVisible`, `clear`, `load`.
    - Queries: `find`, `findMesh`, `objects`, `revision`, `nextId`.
    - History: `undo`, `redo`, `canUndo`, `canRedo`, `undoHistory`, `redoHistory`.
  - `DocumentObject` with `ObjectData`, a variant of `MeshData`, `SelectionData`, `MeasurementData`, `QuadLayoutData` and `SolidData`.
  - `Provenance` (command name, parameters as JSON text, input ids), `HistoryStep`, `MeshObject` and `ObjectId`.

## Dependencies

May use: `core` and the C++ standard library.
Must not depend on: `adapters`, `io`, `pipeline`, `commands`, `app`, `cli`, `rpc`, `mcp`, any
JSON library, and kernel library headers (CGAL, Open CASCADE, OpenSubdiv). Enforced by
`scripts/check_layers.py`.

## Notes

- Not thread-safe. Front ends reach it only through the command engine.
- A snapshot copies the object map, not the meshes: objects hold `std::shared_ptr<const T>`.
- Steps nest; only the outermost one counts. A change made outside a step is its own step.
- Ids are never reused, even after undo.
- Removing a mesh removes the selections and measurements that refer to it. `replaceMesh` drops
  its selections unless the new mesh shares the old face buffer.
