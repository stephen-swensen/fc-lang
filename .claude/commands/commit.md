---
disable-model-invocation: true
effort: low
context: fork
---

Commit changes. Follow these steps in order.

## Step 1: Check repo state

Run `git status`. If the repo is in a conflicted state (mid-merge, mid-rebase, mid-cherry-pick), stop and tell the user.

## Step 2: Stage files

Stage all changed and untracked files with `git add -A`. If the user asked to exclude specific files, unstage them with `git reset HEAD -- <file>`. Do not stage files that look like secrets (.env, credentials, etc.).

## Step 3: Diff

Run `git diff --cached` to see what will be committed.

## Step 4: Commit

Based on the diff, write a concise commit message that describes what changed and why. Scale the message to the size of the change — small changes get a short sentence, larger changes may get a few sentences. Never include `Co-Authored-By` or attribution trailers. Commit using a HEREDOC:
```
git commit -m "$(cat <<'EOF'
message here
EOF
)"
```

## Step 5: Summary

Print the branch, commit hash, and commit message.
