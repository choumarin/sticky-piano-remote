# Agent instructions (Sticky Piano Remote)

## Git policy — mandatory, recommendations have priority over efficiency

- **Never run `git push`.** Pushing is done by the human, manually. No
  force-with-lease, no force, no exception. This is also enforced by
  `.githooks/pre-push` (blocks pushes unless the human exports
  `GIT_ALLOW_PUSH=1`); after cloning, run
  `git config core.hooksPath .githooks` to install it.
- Never rewrite history that has been pushed (no rebase/amend/reset of
  commits that exist on the remote). Only unpushed, local commits may be
  rewritten, and only when the user asks.
- Committing locally is allowed — write clean, single-topic commit
  messages; do not commit temp files, generated previews or build output
  unless they are actual deliverables.
