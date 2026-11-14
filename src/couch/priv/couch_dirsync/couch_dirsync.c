// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

// Flush a directory so that changes to its entries reach stable storage.
//
// Creating, renaming or unlinking a file changes the directory it lives in, not
// the file itself. fsync() on the file's descriptor does not make that change
// durable: a file created and fsynced just before a power loss may simply not
// be there afterwards, and a file that was unlinked may reappear. Only a sync
// of the directory itself orders those operations against the disk.
//
// Erlang/OTP cannot do this: file:open/2 on a directory succeeds but
// file:sync/1 returns {error, eisdir} (checked on OTP 27). So this NIF exists.
//
// POSIX: open(2) the directory, fsync(2) the descriptor, close(2) it.
//
// Windows: CreateFileW() with FILE_FLAG_BACKUP_SEMANTICS to get a directory
// handle, then FlushFileBuffers(). A directory handle has to be opened with
// FILE_FLAG_BACKUP_SEMANTICS, and FlushFileBuffers() requires GENERIC_WRITE,
// so the handle is opened read/write even though nothing is written through it.
// A read-only directory handle makes FlushFileBuffers() fail with
// ERROR_ACCESS_DENIED. This does not require administrator rights.

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <limits.h>
#include <string.h>

#include "erl_nif.h"

static ERL_NIF_TERM ATOM_OK;
static ERL_NIF_TERM ATOM_ERROR;
static ERL_NIF_TERM ATOM_WIN32;
static ERL_NIF_TERM ATOM_BADARG;

static ERL_NIF_TERM err_tup(ErlNifEnv *env, const char *reason) {
    return enif_make_tuple2(env, ATOM_ERROR, enif_make_atom(env, reason));
}

#ifdef _WIN32

static ERL_NIF_TERM win32_err_tup(ErlNifEnv *env, DWORD err) {
    const char *reason;

    switch(err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            reason = "enoent";
            break;
        case ERROR_ACCESS_DENIED:
            reason = "eacces";
            break;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            reason = "ebusy";
            break;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
            reason = "enotsup";
            break;
        case ERROR_WRITE_PROTECT:
            reason = "erofs";
            break;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
            reason = "einval";
            break;
        default:
            return enif_make_tuple3(
                    env, ATOM_ERROR, ATOM_WIN32, enif_make_uint(env, (unsigned)err));
    }

    return err_tup(env, reason);
}

static ERL_NIF_TERM sync_dir_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary bin;
    WCHAR *wpath;
    int wlen;
    HANDLE hdl;
    DWORD err;

    if (argc != 1 || !enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
        return enif_make_tuple2(env, ATOM_ERROR, ATOM_BADARG);
    }

    if(bin.size == 0 || bin.size > (size_t)INT_MAX) {
        return err_tup(env, "einval");
    }

    wlen = MultiByteToWideChar(CP_UTF8, 0, (const char *)bin.data, (int)bin.size, NULL, 0);
    if(wlen == 0) {
        return win32_err_tup(env, GetLastError());
    }

    wpath = enif_alloc((wlen + 1) * sizeof(WCHAR));
    if(wpath == NULL) {
        return err_tup(env, "enomem");
    }

    if(MultiByteToWideChar(CP_UTF8, 0, (const char *)bin.data, (int)bin.size, wpath, wlen) != wlen) {
        err = GetLastError();
        enif_free(wpath);
        return win32_err_tup(env, err);
    }
    wpath[wlen] = L'\0';

    hdl = CreateFileW(
            wpath,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL);

    if(hdl == INVALID_HANDLE_VALUE) {
        // Read the error before enif_free(): freeing can clobber the
        // thread-local last-error value.
        err = GetLastError();
        enif_free(wpath);
        return win32_err_tup(env, err);
    }

    enif_free(wpath);

    if(!FlushFileBuffers(hdl)) {
        err = GetLastError();
        CloseHandle(hdl);
        return win32_err_tup(env, err);
    }

    if(!CloseHandle(hdl)) {
        return win32_err_tup(env, GetLastError());
    }

    return ATOM_OK;
}

#else

// Kept in sync with the errno names CouchDB and OTP report elsewhere. Anything
// else is reported as eio; callers only log the reason.
static const char *posix_err_reason(int err) {
    switch(err) {
        case ENOENT:  return "enoent";
        case EACCES:  return "eacces";
        case EPERM:   return "eperm";
        case EBUSY:   return "ebusy";
        case EINVAL:  return "einval";
        case EROFS:   return "erofs";
        case EBADF:   return "ebadf";
        case EMFILE:  return "emfile";
        case ENFILE:  return "enfile";
        case ENOMEM:  return "enomem";
#ifdef ENOTSUP
        case ENOTSUP: return "enotsup";
#endif
#ifdef EOPNOTSUPP
#  if !defined(ENOTSUP) || (EOPNOTSUPP != ENOTSUP)
        case EOPNOTSUPP: return "enotsup";
#  endif
#endif
        default:      return "eio";
    }
}

static ERL_NIF_TERM sync_dir_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary bin;
    char *path;
    int fd;
    int rc;
    int err;

    if (argc != 1 || !enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
        return enif_make_tuple2(env, ATOM_ERROR, ATOM_BADARG);
    }

    path = enif_alloc(bin.size + 1);
    if(path == NULL) {
        return err_tup(env, "enomem");
    }
    memcpy(path, bin.data, bin.size);
    path[bin.size] = '\0';

    fd = open(path, O_RDONLY);
    err = errno;
    enif_free(path);

    if(fd < 0) {
        return err_tup(env, posix_err_reason(err));
    }

    rc = fsync(fd);
    err = errno;
    close(fd);

    if(rc < 0) {
        return err_tup(env, posix_err_reason(err));
    }

    return ATOM_OK;
}

#endif

static int load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info)
{
    (void) env;
    (void) load_info;

    ATOM_OK     = enif_make_atom(env, "ok");
    ATOM_ERROR  = enif_make_atom(env, "error");
    ATOM_WIN32  = enif_make_atom(env, "win32");
    ATOM_BADARG = enif_make_atom(env, "badarg");

    *priv_data = NULL;

    return 0;
}

static ErlNifFunc nif_funcs[] = {
    {"sync_dir_nif", 1, sync_dir_nif, ERL_NIF_DIRTY_JOB_IO_BOUND}
};

ERL_NIF_INIT(couch_dirsync, nif_funcs, load, NULL, NULL, NULL)