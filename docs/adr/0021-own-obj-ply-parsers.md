# 0021. Own OBJ and PLY parsers

- Status: Accepted
- Date: 2026-10-15

## Context
M0 needs OBJ and PLY import. `THIRD_PARTY.md` listed tinyobjloader and happly as candidates "or own parsers; decide in the issue". Scans only need positions and triangles (polygons are fanned), and the STL reader already has the welding and validation code.

## Decision
`src/io` has its own readers: OBJ (`v`, `f` with any `v/vt/vn` form, negative indices, polygons fanned) and PLY (ASCII, binary little- and big-endian; `vertex` x/y/z of any numeric type and `face` index lists; other elements and properties skipped). Writers produce OBJ with round-trip `to_chars` output and binary little-endian PLY with double coordinates. Readers keep the file's vertex order and only merge exact duplicates, so a write/read round trip is identical.

## Alternatives considered
- tinyobjloader and happly: well tested, MIT, but each pulls in a material, texture or property model we drop immediately, and two more dependencies need vcpkg and fallback entries.

## Consequences
- About 600 lines to maintain; covered by `tests/unit/ObjPlyTests.cpp`. Fuzzing (planned in section 10) must include these readers.
- Colours, normals and texture coordinates are ignored on import; revisit if a stage needs them.
