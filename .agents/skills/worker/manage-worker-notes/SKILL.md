---
name: manage-worker-notes
description: "How the background worker reads and maintains repository quirks, build tips, and lessons learned in .worker.notes.md."
---

# Manage Worker Notes Skill (Worker AI)

This skill allows the background Worker AI to read and maintain a local cheat sheet (`.worker.notes.md`) to remember project-specific build quirks, tool paths, and compiler flags across stateless runs.

---

## When to Consult `.worker.notes.md`
- **At the start of every task**: Check if `.worker.notes.md` exists at the repo root. Read its tips (e.g. "Always use `ninja -C build`" or "Virtualenv is at `.venv/bin/activate`").

---

## When to Update `.worker.notes.md`
- When you discover a non-obvious repository quirk (e.g. "Running `./scripts/01_build.sh` requires `export BUILD_TYPE=Release`").
- When an unexpected linker dependency or header order is required.
- Append a concise 1-2 line tip under the relevant section. Keep the file compact (< 50 lines).
