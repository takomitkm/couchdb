# CouchDB directory-flush patch — ledger

Date: 2026-11-15
Upstream: apache/couchdb, branch `main`, base commit `300d379c6821aec1fb91637d7f9fee0640499515`
("Fix types parameter for /_index_info", 2026-11-13)

## Files

New:
- `src/couch/priv/couch_dirsync/couch_dirsync.c` (+243)
- `src/couch/src/couch_dirsync.erl` (+56)
- `src/couch/test/eunit/couch_dirsync_tests.erl` (+91)

Modified:
- `src/couch/src/couch_file.erl` (+50/-3): `sync_dir/1`, `rename/2`, create branch,
  `delete_file/4`, `rename_file/1`, `delete_dir/2`
- `src/couch/src/couch_bt_engine.erl` (+3/-3): recovery rename + 2 compaction renames
- `src/couch_mrview/src/couch_mrview_compactor.erl` (+1/-1)
- `src/couch_mrview/src/couch_mrview_util.erl` (+1/-1)
- `src/couch/rebar.config.script` (+8/-1): port specs for the new NIF
- `src/couch/.gitignore` (+1): the port compiler's `.d` file

Totals: 9 files, +450/-10.

## Patch

- `dirsync.patch` (21767 bytes, 9 diffs)
- `git apply --check` against a pristine `git archive HEAD` tree: **RC=0**

## Verification

| Check | Result | Artifact |
| --- | --- | --- |
| OTP cannot fsync a directory | `file:sync/1` → `{error, eisdir}` despite `openat(dir)` succeeding | probe/dirsync_probe.erl |
| POSIX NIF behaviour | existing dir → ok, missing → `{error,enoent}`, file path → ok | probe/build_test_dirsync.sh |
| POSIX syscall proof (create path) | `O_CREAT` file → `openat(dir)` → `fsync(dirfd)` | probe/work/create.trace |
| POSIX syscall proof (delete path) | `rename` into `.delete/` → `fsync` ×2 → `unlink` | probe/work/del.trace |
| Real Windows (NTFS, non-admin, OTP 27.3.4) | 7/7 as expected incl. `enoent`, binary/iolist/forward-slash paths, `badarg` | probe/win_run.sh |
| Windows cross-compile | NIF builds with the Windows `erl_nif.h`; exports `nif_init` | probe/win_compile_check.sh |
| eunit | 5 tests, all passed | probe/eunit-final.txt |
| erlfmt 1.8.0 (repo `dev/format_check.py`) | RC=0 for the whole tree | probe/fmt3.log |
| rebar port compiler | produced `src/couch/priv/couch_dirsync.so` (23 KB) | probe/rebar_compile.log |

Local build harness notes (not part of the patch):
- `src/rebar/bootstrap` needed a local shim for stdlib `make:files/2`, removed in
  OTP 27; reverted before diffing.
- `couch_ejson_compare_tests.erl` does not compile without `WITH_PROPER` (pre-existing).
- WSL `/tmp` is cleared between invocations; all artifacts live under this directory.

## Bugs found by testing (already fixed)

1. Windows error path returned `{error,win32,0}` — `enif_free()` clobbers the
   thread-local last-error value; the code now reads `GetLastError()` first.
2. A Windows DLL built with the Linux `erl_nif.h` crashes the VM on load — the
   Windows header routes `enif_*` through `WinDynNifCallbacks`.

## Not sent

Nothing pushed, no PR opened. Next step needs a fork of apache/couchdb plus a
token with contents write; awaiting the user's go-ahead.
## Pushed to the fork (2026-11-15)

- Fork: https://github.com/takomitkm/couchdb (created by the user in the web UI;
  the API tokens available here cannot create repositories)
- `main` = `95d430687` "Replace README with an explanation of why this fork has no
  pull request" (parent `b54baba66`, README.rst blob matches local `22719bee7a9b`)
- `dirsync-proposal` = `c7169b20c` "Flush directories when creating, renaming and
  unlinking database files" (parent `b54baba66`; all 9 file blobs match local,
  upstream's newer commit inherited untouched)
- Method: Git Data API (blobs -> tree on top of the fork's main tree -> commit ->
  ref), because direct `git push` to github.com is reset on this network. Token =
  the `gh:github.com:takomitkm` classic credential (`repo, workflow, gist,
  read:org`); the HKCU fine-grained PAT has Contents: read only and the SSH keys
  on this machine are deploy keys.
- Both commits authored/committed by `takomitkm <takomitkm@outlook.com>`.
- No PR was opened (CouchDB AI contribution policy).
