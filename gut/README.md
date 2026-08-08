# gut

A lightweight Windows utility based on libgit2 that fixes commit metadata:
it makes the **committer** match the **author** — name, email and timestamp.

Builds to a single self-contained `gut.exe` (static libgit2, static CRT, no
DLL dependencies).

## Usage

```
gut [options]                          amend the last commit (HEAD)
gut rebase <upstream> [sync options]   fix all commits in <upstream>..HEAD
gut --rebase <upstream> [sync options] same as above
```

### Default: amend HEAD

`gut` amends the last commit so that the committer signature (name, email
**and** timestamp) is copied from the author signature. The tree is left
untouched — staged changes are *not* folded in. This is equivalent to
`gut.py`:

```python
pygit2.Repository(".").amend_commit(refname="HEAD", commit=..., committer=author)
```

### Overrides (amend mode)

Like `git commit --amend`, except the override applies to **both** author
and committer (so they stay in sync):

* `--author="Name <email>"` — override author name/email. Unlike git, the
  author **timestamp is kept** unless a date is also given.
* `--date=<date>` — override the timestamp. Accepted formats:
  `now`, `@<epoch> <+-HHMM>`, `<epoch>`,
  `YYYY-MM-DD[T ]HH:MM[:SS][Z| +-HHMM]`.

Quote values containing `<`, `>` or spaces on cmd.exe/PowerShell.

### Rebase mode

`gut rebase <upstream>` replays every commit in `<upstream>..HEAD` onto the
upstream tip — exactly the range and target handling of
`git rebase <upstream>` (any commit-ish works: branch, tag, SHA, `HEAD~3`).
Commits already applied upstream are skipped (patch-id), and the branch,
index and worktree are updated only after **every** commit was rewritten
successfully. The rebase runs fully in memory, so a conflict aborts with an
error and leaves the repository completely untouched — there is no rebase
state to clean up.

Like `git rebase`, gut refuses to run with unstaged or staged changes to
tracked files (untracked files are fine).

Per-commit overrides make no sense across a range, so `--author`/`--date`
are rejected here. Instead, choose *what* gets synced from author to
committer (naming adapted from git rebase's `--committer-date-is-author-date`):

* *(neither flag)* — full sync: committer := author (name, email, timestamp)
* `--sync-date` — sync only the timestamp; committer name/email come from
  the configured `user.name`/`user.email`
* `--sync-name` — sync only name/email; committer timestamp is set to now
  (like a normal rebase)

## Build

Requires Visual Studio (or Build Tools) 2022+; uses the CMake bundled with
it, nothing else needs to be installed:

```
gut\build.cmd
```

produces `gut\build\Release\gut.exe`. The script configures
`gut\CMakeLists.txt`, which pulls in libgit2 from the parent directory and
links it statically. To cross-compile, configure manually with
`cmake -S gut -B <builddir> -A Win32|x64|ARM64`.

## Releases

`.github/workflows/gut.yml` builds `gut-windows-x64.exe`,
`gut-windows-x86.exe` and `gut-windows-arm64.exe` on every manual dispatch,
and creates a GitHub release with all three binaries whenever a tag named
`gut-*` is pushed:

```
git tag gut-v1.0.0
git push origin gut-v1.0.0
```

## Notes & limitations

* Merge commits inside the rebased range are linearized (dropped as
  merges), same as a plain `git rebase` without `--rebase-merges`.
* Commit messages, GPG signatures and trees are preserved as-is; only the
  author/committer identity lines change. Signed commits lose their valid
  signature by definition (the content changes).
* `--date` implements a small set of explicit formats, not git's full
  approxidate parser.
* Unicode-safe: arguments are read as UTF-16 (`wmain`) and console output
  goes through `WriteConsoleW`, so non-ASCII names and messages (umlauts,
  emoji) are stored and displayed correctly regardless of the console code
  page; redirected output is plain UTF-8.
