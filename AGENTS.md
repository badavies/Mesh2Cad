# Instructions for AI coding tools

Read these first, in this order:

1. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — what the project is, the layering rules (section 4), the repository layout (section 9) and the roadmap (section 12).
2. [`docs/AI_GUIDELINES.md`](docs/AI_GUIDELINES.md) — the rules for AI-assisted changes. They are binding.

Short version:

- Work only on the roadmap issue you were given, and only in the modules it names. Anything else you notice becomes a new issue.
- Keep a change under about 400 lines of non-test code.
- Do not add dependencies, abstractions, patterns or features from later milestones.
- Every new capability is a registered command (`src/commands`) with a schema and a test. Never add a UI-only or CLI-only code path.
- Kernel types (CGAL, Open CASCADE, OpenSubdiv) stay inside `src/adapters/`. `python3 scripts/check_layers.py` must pass.
- Never change an expected value, tolerance or golden metric to make a test pass. If a test looks wrong, say so and stop.
- Build and test before you finish: `cmake --preset linux-gcc-debug` (or `windows-msvc-debug`), `cmake --build --preset <same>`, `ctest --preset <same>`.
- Format with the repository `.clang-format` (CLion applies it automatically).
