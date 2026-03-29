---
model: haiku
effort: low
context: fork
---

Summarize the current state of the repo. Run these commands in parallel:
- `git status` (never use -uall flag)
- `git diff HEAD --stat` to see lines added/removed per file
- `git diff HEAD` to see the full diff

Then provide a concise summary: which files changed, what kind of changes they are, and the lines added/removed totals from the stat output.
