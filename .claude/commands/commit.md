Commit and optionally push changes. Follow these steps exactly in order. Do not skip steps or combine them.

## Step 1: Check branch

Run `git branch --show-current` and display it. If the branch seems unexpected for the work being done, ask the user to confirm before proceeding.

## Step 2: Inspect changes

Run these in parallel:
- `git status` (never use -uall flag)
- `git diff HEAD` to see all unstaged and staged changes (or `git diff --cached` if the user has already staged files)
- `git log --oneline -5` to see the repo's commit message style

## Step 3: Draft commit message

Based ONLY on what the diff shows, draft a concise commit message. Do not rely on conversation memory — changes may have been made and reverted during the session. Match the style of recent commits shown in git log.

Do NOT include `Co-Authored-By` or any attribution trailers.

## Step 4: Stage files

Stage specific files by name. Never use `git add -A` or `git add .`. Do not stage files that look like secrets (.env, credentials, etc.) or build artifacts (.o files, binaries). Show the user which files you are staging.

## Step 5: Commit

Create the commit with the drafted message. Use a HEREDOC for the message to preserve formatting:
```
git commit -m "$(cat <<'EOF'
message here
EOF
)"
```

## Step 6: Push

After the commit succeeds, run `git branch --show-current` one more time to verify the branch. If the branch looks correct for the work, run `git push`. If the branch looks wrong or unexpected, ask the user before pushing.
