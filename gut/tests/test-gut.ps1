# Cross-platform test suite for gut (Windows, Linux, macOS).
#
# Requires: git and PowerShell 7+ (pwsh).  Run from anywhere:
#   pwsh gut/tests/test-gut.ps1 [-Gut <path-to-binary>] [-Base <scratch-dir>]
param(
    [string]$Gut,
    [string]$Base = (Join-Path ([IO.Path]::GetTempPath()) 'guttest')
)

$ErrorActionPreference = 'Stop'

if (-not $Gut) {
    $root = (Resolve-Path (Join-Path $PSScriptRoot '..' '..')).Path
    if ($env:OS -eq 'Windows_NT' -or $IsWindows) {
        $Gut = Join-Path $root 'gut' 'build' 'Release' 'gut.exe'
    } else {
        $Gut = Join-Path $root 'gut' 'build-unix' 'gut'
    }
}
if (-not (Test-Path $Gut)) { Write-Host "gut binary not found: $Gut"; exit 1 }

$script:failures = 0

function Check($name, $cond) {
    if ($cond) { Write-Host "PASS: $name" -ForegroundColor Green }
    else { Write-Host "FAIL: $name" -ForegroundColor Red; $script:failures++ }
}

function New-Repo($path) {
    Remove-Item -Recurse -Force $path -ErrorAction SilentlyContinue
    git init -q -b main $path | Out-Null
    git -C $path config user.name 'Com Mitter'
    git -C $path config user.email 'committer@example.com'
}

function Add-Commit($path, $file, $content, $msg, $authorDate, $committerDate) {
    Set-Content (Join-Path $path $file) $content
    git -C $path add $file | Out-Null
    $env:GIT_AUTHOR_NAME = 'Au Thor'
    $env:GIT_AUTHOR_EMAIL = 'author@example.com'
    $env:GIT_AUTHOR_DATE = $authorDate
    $env:GIT_COMMITTER_DATE = $committerDate
    git -C $path commit -q -m $msg | Out-Null
    Remove-Item Env:GIT_AUTHOR_NAME, Env:GIT_AUTHOR_EMAIL, Env:GIT_AUTHOR_DATE, Env:GIT_COMMITTER_DATE
}

function LogFmt($path, $fmt, $rev) {
    if ($rev) { git -C $path log -1 --format=$fmt $rev } else { git -C $path log -1 --format=$fmt }
}

$ids = '%an|%ae|%ai|%cn|%ce|%ci'

# ---------- amend mode ----------
$r = Join-Path $base 'amend'
New-Repo $r
Add-Commit $r 'f1' 'a' 'c1' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
Add-Commit $r 'f2' 'b' 'c2' '2002-03-04T05:06:07+00:00' '2026-02-02T00:00:00+00:00'
$headBefore = git -C $r rev-parse HEAD

Push-Location $r
& $Gut | Out-Null
Pop-Location
$l = LogFmt $r $ids
Check 'amend: committer := author (incl. timestamp)' `
    ($l -eq 'Au Thor|author@example.com|2002-03-04 05:06:07 +0000|Au Thor|author@example.com|2002-03-04 05:06:07 +0000')
Check 'amend: older commit untouched' `
    ((LogFmt $r '%cn|%ci' 'HEAD~1') -eq 'Com Mitter|2026-01-01 00:00:00 +0000')
Check 'amend: tree unchanged' `
    ((git -C $r rev-parse 'HEAD^{tree}') -eq (git -C $r rev-parse "$headBefore^{tree}"))

Push-Location $r
& $Gut --author="Ne W <new@example.com>" | Out-Null
Pop-Location
Check 'amend --author: identity overridden, author date kept, committer synced' `
    ((LogFmt $r $ids) -eq 'Ne W|new@example.com|2002-03-04 05:06:07 +0000|Ne W|new@example.com|2002-03-04 05:06:07 +0000')

Push-Location $r
& $Gut --date="@1234567890 +0100" | Out-Null
Pop-Location
Check 'amend --date raw epoch+tz: both dates overridden' `
    ((LogFmt $r $ids) -eq 'Ne W|new@example.com|2009-02-14 00:31:30 +0100|Ne W|new@example.com|2009-02-14 00:31:30 +0100')

Push-Location $r
& $Gut --date "2010-05-06 07:08:09 +0200" | Out-Null
Pop-Location
Check 'amend --date ISO: both dates overridden' `
    ((LogFmt $r $ids) -eq 'Ne W|new@example.com|2010-05-06 07:08:09 +0200|Ne W|new@example.com|2010-05-06 07:08:09 +0200')

# staged changes must not be folded into the amend
Set-Content (Join-Path $r 'staged.txt') 'staged'
git -C $r add staged.txt | Out-Null
$treeBefore = git -C $r rev-parse 'HEAD^{tree}'
Push-Location $r
& $Gut | Out-Null
Pop-Location
Check 'amend: staged changes not committed' `
    ((git -C $r rev-parse 'HEAD^{tree}') -eq $treeBefore)
git -C $r reset -q --hard HEAD

# ---------- rebase mode: full sync ----------
$r = Join-Path $base 'rebase-full'
New-Repo $r
Add-Commit $r 'f0' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r tag base
Add-Commit $r 'f1' 'a' 'c1' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
Add-Commit $r 'f2' 'b' 'c2' '2002-03-04T05:06:07+00:00' '2026-02-02T00:00:00+00:00'
Add-Commit $r 'f3' 'c' 'c3' '2003-04-05T06:07:08+00:00' '2026-03-03T00:00:00+00:00'
$treeBefore = git -C $r rev-parse 'HEAD^{tree}'

Push-Location $r
$out = & $Gut rebase base
Pop-Location
$lines = git -C $r log --format=$ids base..HEAD
Check 'rebase full: all committers synced to authors' `
    (($lines | Where-Object { $_ -notmatch '^Au Thor\|author@example\.com\|([^|]+)\|Au Thor\|author@example\.com\|\1$' }).Count -eq 0 -and $lines.Count -eq 3)
Check 'rebase full: tree unchanged' ((git -C $r rev-parse 'HEAD^{tree}') -eq $treeBefore)
Check 'rebase full: base commit untouched' ((LogFmt $r '%cn|%ci' 'base') -eq 'Com Mitter|2025-01-01 00:00:00 +0000')
Check 'rebase full: reports rewrites' (($out | Select-String '^rewrite ').Count -eq 3)
git -C $r fsck --strict 2>$null | Out-Null
Check 'rebase full: fsck clean' ($LASTEXITCODE -eq 0)
Check 'rebase full: worktree clean' ((git -C $r status --porcelain) -eq $null)

# ---------- rebase mode: --sync-date ----------
$r = Join-Path $base 'rebase-date'
New-Repo $r
Add-Commit $r 'f0' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r tag base
Add-Commit $r 'f1' 'a' 'c1' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
Add-Commit $r 'f2' 'b' 'c2' '2002-03-04T05:06:07+00:00' '2026-02-02T00:00:00+00:00'

Push-Location $r
& $Gut --rebase base --sync-date | Out-Null
Pop-Location
$lines = git -C $r log --format=$ids base..HEAD
Check 'rebase --sync-date: committer ident from config, date from author' `
    (($lines | Where-Object { $_ -notmatch '^Au Thor\|author@example\.com\|([^|]+)\|Com Mitter\|committer@example\.com\|\1$' }).Count -eq 0 -and $lines.Count -eq 2)

# ---------- rebase mode: --sync-name ----------
Push-Location $r
& $Gut rebase base --sync-name | Out-Null
Pop-Location
$lines = git -C $r log --format='%an|%ae|%cn|%ce|%ct' base..HEAD
$now = [DateTimeOffset]::Now.ToUnixTimeSeconds()
$ok = $true
foreach ($l in $lines) {
    $f = $l -split '\|'
    if ($f[2] -ne 'Au Thor' -or $f[3] -ne 'author@example.com') { $ok = $false }
    if ([math]::Abs($now - [int64]$f[4]) -gt 120) { $ok = $false }
}
Check 'rebase --sync-name: committer ident from author, date is now' ($ok -and $lines.Count -eq 2)

# ---------- rebase mode: guards ----------
Set-Content (Join-Path $r 'f1') 'dirty'
Push-Location $r
$err = & $Gut rebase base 2>&1
$code = $LASTEXITCODE
Pop-Location
Check 'rebase: dirty worktree refused' ($code -eq 1 -and "$err" -match 'unstaged or staged')
git -C $r checkout -q -- .

Push-Location $r
$out = & $Gut rebase HEAD
Pop-Location
Check 'rebase: up-to-date range is a no-op' ("$out" -match 'up to date')

Push-Location $r
$err = & $Gut rebase base --author="X <x@y>" 2>&1
$code = $LASTEXITCODE
Pop-Location
Check 'rebase: --author rejected' ($code -eq 1 -and "$err" -match 'cannot be combined')

Push-Location $r
$err = & $Gut --sync-date 2>&1
$code = $LASTEXITCODE
Pop-Location
Check 'amend: --sync-date rejected' ($code -eq 1 -and "$err" -match 'only valid in rebase')

# ---------- rebase mode: conflict leaves repo untouched ----------
$r = Join-Path $base 'rebase-conflict'
New-Repo $r
Add-Commit $r 'f' 'line1' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r tag base
Add-Commit $r 'f' 'ours' 'ours' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
git -C $r checkout -q -b other base
Add-Commit $r 'f' 'theirs' 'theirs' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
git -C $r checkout -q main
$headBefore = git -C $r rev-parse HEAD

Push-Location $r
$err = & $Gut rebase other 2>&1
$code = $LASTEXITCODE
Pop-Location
Check 'rebase: conflict reported, exit 1' ($code -eq 1 -and "$err" -match 'conflict')
Check 'rebase: HEAD unchanged after conflict' ((git -C $r rev-parse HEAD) -eq $headBefore)
Check 'rebase: no rebase state left behind' `
    (-not (Test-Path (Join-Path $r '.git' 'rebase-merge')) -and -not (Test-Path (Join-Path $r '.git' 'rebase-apply')))
Check 'rebase: worktree untouched after conflict' ((git -C $r status --porcelain) -eq $null)

# ---------- rebase mode: already-upstream commit is skipped ----------
$r = Join-Path $base 'rebase-eapplied'
New-Repo $r
Add-Commit $r 'f' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r tag base
Add-Commit $r 'f' 'change' 'change' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
git -C $r checkout -q -b upstream base
git -C $r cherry-pick main | Out-Null   # same patch, already upstream
git -C $r checkout -q main

Push-Location $r
$out = & $Gut rebase upstream
Pop-Location
Check 'rebase: already-applied commit skipped' ("$out" -match 'skip')
Check 'rebase: branch moved to upstream tip' ((git -C $r rev-parse HEAD) -eq (git -C $r rev-parse upstream))

# ---------- rebase mode: onto a diverged upstream (no conflict) ----------
$r = Join-Path $base 'rebase-diverged'
New-Repo $r
Add-Commit $r 'f0' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r tag base
Add-Commit $r 'f1' 'main work' 'main work' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
git -C $r checkout -q -b upstream base
Add-Commit $r 'f2' 'upstream work' 'upstream work' '2001-06-06T00:00:00+00:00' '2026-06-06T00:00:00+00:00'
git -C $r checkout -q main

Push-Location $r
$out = & $Gut rebase upstream
Pop-Location
Check 'rebase onto diverged upstream succeeds' ("$out" -match 'Successfully rebased')
Check 'rebase: new parent is upstream tip' ((git -C $r rev-parse 'HEAD~1') -eq (git -C $r rev-parse upstream))
Check 'rebase: both files present' ((Test-Path (Join-Path $r 'f1')) -and (Test-Path (Join-Path $r 'f2')))
Check 'rebase: committer synced' `
    ((LogFmt $r $ids) -eq 'Au Thor|author@example.com|2001-02-03 04:05:06 +0000|Au Thor|author@example.com|2001-02-03 04:05:06 +0000')

# ---------- rebase mode: --root ----------
$r = Join-Path $base 'rebase-root'
New-Repo $r
Add-Commit $r 'f0' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
Add-Commit $r 'f1' 'a' 'c1' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
Add-Commit $r 'f2' 'b' 'c2' '2002-03-04T05:06:07+00:00' '2026-02-02T00:00:00+00:00'
$treeBefore = git -C $r rev-parse 'HEAD^{tree}'

Push-Location $r
$out = & $Gut rebase --root
Pop-Location
$lines = git -C $r log --format=$ids
Check 'rebase --root: every commit incl. root rewritten+synced' `
    (($lines | Where-Object { $_ -notmatch '^Au Thor\|author@example\.com\|([^|]+)\|Au Thor\|author@example\.com\|\1$' }).Count -eq 0 -and $lines.Count -eq 3)
Check 'rebase --root: tree unchanged' ((git -C $r rev-parse 'HEAD^{tree}') -eq $treeBefore)

Push-Location $r
$out2 = & $Gut --rebase --root
$code = $LASTEXITCODE
Pop-Location
Check 'rebase --root: second run via --rebase --root is a no-op' ($code -eq 0 -and "$out2" -match 'up to date')

Push-Location $r
$err = & $Gut --root 2>&1
$code = $LASTEXITCODE
Pop-Location
Check '--root rejected outside rebase mode' ($code -eq 1 -and "$err" -match 'only valid in rebase')

# merge commits are preserved (unlike git rebase, which linearizes)
$r = Join-Path $base 'rebase-root-merge'
New-Repo $r
Add-Commit $r 'f0' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r checkout -q -b side
Add-Commit $r 'fs' 'side' 'side' '2001-05-05T00:00:00+00:00' '2026-05-05T00:00:00+00:00'
git -C $r checkout -q main
Add-Commit $r 'fm' 'main' 'main' '2001-06-06T00:00:00+00:00' '2026-06-06T00:00:00+00:00'
$env:GIT_AUTHOR_NAME = 'Au Thor'; $env:GIT_AUTHOR_EMAIL = 'author@example.com'
$env:GIT_AUTHOR_DATE = '2001-07-07T00:00:00+00:00'; $env:GIT_COMMITTER_DATE = '2026-07-07T00:00:00+00:00'
git -C $r merge -q --no-ff -m 'merge side' side | Out-Null
Remove-Item Env:GIT_AUTHOR_NAME, Env:GIT_AUTHOR_EMAIL, Env:GIT_AUTHOR_DATE, Env:GIT_COMMITTER_DATE
$treeBefore = git -C $r rev-parse 'HEAD^{tree}'

Push-Location $r
& $Gut rebase --root | Out-Null
Pop-Location
Check 'rebase --root: merge commit preserved with 2 parents' ((git -C $r rev-list --merges --count HEAD) -eq 1)
Check 'rebase --root: merge commit committer synced' `
    ((LogFmt $r '%cn|%ce|%ci' 'HEAD') -eq 'Au Thor|author@example.com|2001-07-07 00:00:00 +0000')
Check 'rebase --root: merge tree unchanged' ((git -C $r rev-parse 'HEAD^{tree}') -eq $treeBefore)
Check 'rebase --root: worktree clean after merge rewrite' ((git -C $r status --porcelain) -eq $null)

# --root with a target replays the entire history onto it
$r = Join-Path $base 'rebase-root-onto'
New-Repo $r
Add-Commit $r 'f0' 'base' 'base' '2000-01-01T00:00:00+00:00' '2025-01-01T00:00:00+00:00'
git -C $r checkout -q -b other
Add-Commit $r 'fo' 'other' 'other' '2001-05-05T00:00:00+00:00' '2026-05-05T00:00:00+00:00'
git -C $r checkout -q main

Push-Location $r
$out = & $Gut rebase --root other
Pop-Location
Check 'rebase --root <onto>: whole history replayed onto target' `
    ("$out" -match 'Successfully rebased' -and (git -C $r rev-parse 'HEAD~1') -eq (git -C $r rev-parse other))
Check 'rebase --root <onto>: committer synced' `
    ((LogFmt $r $ids) -eq 'Au Thor|author@example.com|2000-01-01 00:00:00 +0000|Au Thor|author@example.com|2000-01-01 00:00:00 +0000')

# ---------- unicode: console output + argv encoding ----------
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$r = Join-Path $base 'unicode'
New-Repo $r
Add-Commit $r 'f1' 'a' 'läuft 🚀' '2001-02-03T04:05:06+00:00' '2026-01-01T00:00:00+00:00'
Push-Location $r
$out = & $Gut --author="Björn Åuthør <b@x.example>"
Pop-Location
Check 'unicode: non-ASCII commit summary printed correctly' ("$out" -match 'läuft 🚀')
Check 'unicode: --author argument stored as UTF-8' ((LogFmt $r '%an|%cn') -eq 'Björn Åuthør|Björn Åuthør')

Write-Host ''
if ($script:failures -eq 0) { Write-Host 'ALL TESTS PASSED' -ForegroundColor Green }
else { Write-Host "$($script:failures) TEST(S) FAILED" -ForegroundColor Red; exit 1 }
