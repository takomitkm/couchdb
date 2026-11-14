# How to reproduce the checks on this branch

Everything below was run on the branch's tree. Nothing here needs a CouchDB
release build.

## 1. The NIF on POSIX

    ERLROOT=$(erl -noshell -eval 'io:format("~s",[code:root_dir()]),halt().')
    gcc -Wall -Wextra -O2 -fPIC -shared \
        -o couch_dirsync.so src/couch/priv/couch_dirsync/couch_dirsync.c \
        -I"$ERLROOT/usr/include"

Load it through `couch_dirsync:sync_dir/1`: an existing directory returns `ok`,
a missing one returns `{error, enoent}`. See `verification/posix-run.txt`.

## 2. The syscalls the create and delete paths make

`verification/create.trace` and `verification/delete.trace` are `strace`
outputs (`-e trace=openat,fsync,unlink,rename`) of a driver that calls
`couch_file:open/2` with `[create]` and then `couch_file:delete/3` with `[sync]`
against the couch app compiled from this tree.

Create path, in order:

    openat(..., "created.couch", O_RDWR|O_CREAT|O_APPEND, 0666) = 19
    openat(..., "createprobe", O_RDONLY)                       = 20
    fsync(20)                                                  = 0

Delete path, in order:

    rename(".../deleted.couch", ".../.delete/<uuid>") = 0
    fsync(20)                                         = 0
    fsync(20)                                         = 0
    unlink(".../.delete/<uuid>")                      = 0

The two `fsync` calls after the rename are the source directory and the target
directory (`rename/2` syncs both when they differ).

## 3. The NIF on Windows

Built with the Windows `erl_nif.h` from the OTP installation (the Windows
header routes `enif_*` through `WinDynNifCallbacks`; the Linux header produces a
DLL that crashes the VM at load), loaded by a portable OTP 27.3.4 on NTFS as a
non-administrator. Output in `verification/windows-run.txt`:

    existing dir      -> ok
    missing dir       -> {error,enoent}
    dir after create  -> ok
    forward-slash dir -> ok
    binary path       -> ok
    iolist path       -> ok
    non-path arg      -> {error,badarg}

## 4. EUnit

`make eunit` for the new module, five tests, output in
`verification/eunit.txt`.

## 5. Formatting

`dev/format_check.py` with erlfmt 1.8.0 (the version `configure` pins) passes
for the whole tree.

## What was not run

The official CI. CouchDB's required check is ASF Jenkins
(`continuous-integration/jenkins/pr-merge`, see `.asf.yaml`), which only runs
for pull requests against `apache/couchdb`. The tree has no GitHub Actions
workflows, so a fork branch has nothing to run.