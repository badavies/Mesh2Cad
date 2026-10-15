# CI files waiting to be moved into `.github/`

These could not be written into `.github/` automatically (the folder is write-protected for the assistant). Move them yourself, then delete this folder:

```
mkdir .github\workflows
move docs\ci\ci.yml .github\workflows\ci.yml
move docs\ci\pull_request_template.md .github\pull_request_template.md
rmdir /s /q docs\ci
```

The workflow has not been run yet. Expect to adjust it after the first run on GitHub (vcpkg bootstrap on the Windows runner is the most likely place).
