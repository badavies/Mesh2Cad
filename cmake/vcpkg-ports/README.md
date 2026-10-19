# vcpkg overlay ports

Ports here replace vcpkg's own for the vcpkg presets (`vcpkg.json`, `vcpkg-configuration`).

- `opencascade`: vcpkg's port at the baseline in `vcpkg.json` (commit 44ccfd8, MIT licence, copyright Microsoft Corporation), with `port-version` 1 and one more patch, `0006-align-incallocator-allocations.patch` (ADR-027). Delete the folder once the baseline ships an Open CASCADE with that fix.
