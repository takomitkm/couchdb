# Flush directories so that file creation, renames and deletes survive a power loss

## Why

CouchDB fsyncs the database file, and it is careful about the order of those
syncs (data first, then the footer). What it never does is fsync the
**directory** the file lives in, and that is a separate thing:

> A directory sync is required. If you fail to fsync() the directory after
> creating a new file, then that file might not be there following a power loss
> and reboot. Or if you unlink() a file and do not fsync() the directory, that
> file might reappear following a power loss and reboot.
> — D. Richard Hipp, <https://sqlite.org/forum/forumpost/903a8f2277bf773330b0cd30d624776d12f7c9e4787f3c772e1fae8bdbc1f466>

Applying that to CouchDB, the two cases are:

1. **A newly created database can vanish.** `PUT /db` creates the file, the
   header is fsynced, documents are written and acknowledged. None of that
   makes the *entry* for the file durable. After a power loss the file may
   simply not exist, and with it everything that was acknowledged into it.
   There is no recovery for this: every other crash path in CouchDB works
   because *something* is left on disk to recover from, and here nothing is.
   The same applies to the files a database creates next to itself
   (`.compact`, `.compact.data`, `.compact.meta`).

2. **A deleted database can come back.** `DELETE /db` renames the file into
   `{database_dir}/.delete/` and then unlinks it. Neither the rename nor the
   unlink is ordered against the disk, so after a crash the database may be
   back — with `enable_database_recovery` it is back as an explicit
   `<name>.<timestamp>.deleted.couch`, without it the file is back under its
   original name.

Compaction has the same shape, and the code already knows it: `finish_compaction_int/2`
carries the comment

> Rename our *.compact.data file to *.compact so that if we die between
> deleting the old file and renaming *.compact we can recover correctly.

which is a recovery dance built entirely out of renames that are themselves
not durable. This PR does not change that dance; it makes each step of it
ordered on disk so the steps cannot be lost out of order.

## What this changes

Erlang/OTP cannot sync a directory. `file:open/2` on a directory succeeds, but
`file:sync/1` returns `{error, eisdir}` (checked on OTP 27, see the eunit test),
so this has to go through a NIF. The NIF is small and does the only two things
the platforms offer:

* POSIX: `open(dir, O_RDONLY)` → `fsync(fd)` → `close(fd)`.
* Windows: `CreateFileW(dir, GENERIC_WRITE, FILE_SHARE_READ|WRITE|DELETE,
  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)` → `FlushFileBuffers(h)` →
  `CloseHandle(h)`. A directory handle has to be opened with
  `FILE_FLAG_BACKUP_SEMANTICS`, and `FlushFileBuffers()` requires
  `GENERIC_WRITE`, so the handle is opened read/write even though nothing is
  written through it. This does not require administrator rights.

`couch_dirsync` is a new, separate NIF rather than a function added to
`couch_cfile`, because `couch_cfile`'s Erlang module only loads its NIF on Unix
(`couch_cfile:init/0`) — a Windows build would never see the function.

Call sites, all through two helpers in `couch_file`:

| Helper | Used by |
| --- | --- |
| `couch_file:sync_dir/1` | file creation (`init/1`, create branch), directory removal (`delete_dir/2`) |
| `couch_file:rename/2` | compaction swap (`couch_bt_engine`), compaction-file recovery (`couch_bt_engine`), database delete (`delete_file/4`), delete-with-recovery (`rename_file/1`), view compaction swap (`couch_mrview_compactor`), legacy view index rename (`couch_mrview_util`) |

`rename/2` syncs the source directory and, when the target is in a different
directory, the target directory as well.

Failures are logged and swallowed. A directory that cannot be flushed, or a
build without the NIF, leaves the database **correct** — it only loses the
durability of the directory entry, which is exactly what CouchDB had before.
This is deliberately not `ok =`-style fatal: the alternative (aborting an open
because a directory could not be flushed) would be a bigger behaviour change
than the bug being fixed.

## Behaviour change

* `DELETE /db` (and any other path through `delete_file/4`) now waits for two
  directory flushes (~0.3–1 ms each on a local SSD, more on spinning disks)
  before returning. Previously the rename was not waited on for durability at
  all; the actual unlink is still asynchronous and unchanged.
* Opening a file for creation now costs one directory flush. This is on the
  `create` path only, not on every write.

## Platforms: capability, filesystem behaviour, contract

Naming these three separately matters, because the old "you cannot fsync a
directory on Windows" folklore mixes them up:

* **Capability**: opening a directory with `GENERIC_WRITE` +
  `FILE_FLAG_BACKUP_SEMANTICS` and calling `FlushFileBuffers()` on the handle
  works, without administrator rights. Verified here on real Windows.
* **Filesystem behaviour**: MS-FSA §2.1.5.7 note `<80>` says flushing a
  directory handle "ensures that the directory structure is persisted to
  stable storage". That is a specification for filesystem implementations, not
  an application API contract.
* **Contract**: Microsoft has not published a Win32 contract stating that
  flushing a directory handle persists the namespace entry. `FlushFileBuffers`
  is documented as needing `GENERIC_WRITE` and says nothing about directories,
  and `CreateFile` documents `FILE_FLAG_BACKUP_SEMANTICS` for opening a
  directory without saying what a flush of that handle means. So this is
  best-effort by contract but not by capability, and that is why the failure
  path above is a log line rather than an error.

This is the same call the POSIX side already makes implicitly: `fsync(2)` says
that "calling fsync() does not necessarily ensure that the entry in the
directory containing the file has also reached disk. For that an explicit
fsync() on a directory is needed", and some filesystems still cannot do it
(SQLite ignores `ENOTSUP`-class failures on directories for that reason).

## Evidence

`strace` on the create path — a new database file, then the directory is
fsynced:

```
openat(AT_FDCWD, ".../createprobe/created.couch", O_RDWR|O_CREAT|O_APPEND, 0666) = 19
openat(AT_FDCWD, ".../createprobe", O_RDONLY) = 20
fsync(20) = 0
```

`strace` on the delete path — rename into `.delete/`, then both directories
are fsynced, then the unlink:

```
rename(".../deleted.couch", ".../.delete/3b81cfa526a7c11207cf567ef0712519") = 0
fsync(19) = 0
fsync(19) = 0
unlink(".../.delete/3b81cfa526a7c11207cf567ef0712519") = 0
```

Run on real Windows (NTFS, non-administrator, portable OTP 27.3.4, NIF built
from this patch). Each line is the actual return value:

```
existing dir      -> ok
missing dir       -> {error,enoent}
dir after create  -> ok
forward-slash dir -> ok
binary path       -> ok
iolist path       -> ok
non-path arg      -> {error,badarg}
```

Two things that came out of testing rather than review: on Windows the error
code has to be read *before* freeing the converted path, because freeing can
clobber the thread-local last-error value (the first version reported
`{error,win32,0}` for a missing directory); and the Win32 build must use the
Windows `erl_nif.h` from the OTP installation, because there `enif_*` go
through `WinDynNifCallbacks` rather than being plain imports.

`make eunit` for the new module: 5 tests, all passing. `dev/format_check.py`
(erlfmt 1.8.0, the version pinned by `configure`) passes for the whole tree.

## Not covered here

* View index files are still allowed to be lost (they are rebuilt): the
  rename is made durable because it is the same helper, but nothing tries to
  fsync index contents.
* `smoosh`'s persisted queue (`smoosh_persist`) writes a temp file and renames
  it; it is scheduler state that is regenerated, so it is left alone.
* `.delete/` contents are still cleaned up asynchronously at startup
  (`couch_server`); a file that survives there costs disk space, not
  correctness.
* The directory itself, when it is created by `filelib:ensure_dir/1`, is not
  flushed into its parent. Database directories are normally created by the
  installer/`mkdir`, not per-database, so this is out of scope.

## Prior art

The same gap exists, and in some cases has been patched or proposed, in other
storage engines: leveldb (proposal: `google/leveldb#1388`), RocksDB (proposal:
`meta/rocksdb#15682`), Pebble (proposal: `cockroachdb/pebble#6258`), restic
(proposal: `restic/restic#22177`), and SQLite, whose Windows VFS still discards
the `syncDir` argument on delete (`os_win.c`, `/* Not used on win32 */`) while
the Unix VFS does the sync. Non-database precedents that flush directories on
Windows: deltabox-lite, ejm-rightkit-fs, whim, D2rHub.