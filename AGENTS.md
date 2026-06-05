# AGENTS.md

Guidance for working in this repository:

- Prefer simple code over clever code.
- Keep portability in mind; this project should build with ordinary POSIX/C tools and a bare Makefile.
- Write unsurprising code: clear names, direct control flow, and few abstractions.
- Avoid dependencies unless they remove more complexity than they add.
- Keep changes small and focused on the requested behavior.
- Favor readable error handling over terse tricks.
- When adding features, preserve the command-line tool's minimal shape.
