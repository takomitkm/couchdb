% Licensed under the Apache License, Version 2.0 (the "License"); you may not
% use this file except in compliance with the License. You may obtain a copy of
% the License at
%
%   http://www.apache.org/licenses/LICENSE-2.0
%
% Unless required by applicable law or agreed to in writing, software
% distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
% WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
% License for the specific language governing permissions and limitations under
% the License.

% Flush a directory so that changes to its entries reach stable storage.
%
% Creating, renaming or unlinking a file changes the directory it lives in, not
% the file itself, and syncing the file's descriptor does not make that change
% durable. Erlang/OTP cannot sync a directory -- file:open/2 on a directory
% succeeds but file:sync/1 returns {error, eisdir} -- so this goes through the
% couch_dirsync NIF.
%
% If the NIF is not available (for example a build without a C compiler) calls
% return {error, enotsup} and callers are expected to carry on: the database
% stays correct, only the durability of the directory entry is lost.

-module(couch_dirsync).

-export([sync_dir/1]).

-on_load(init/0).

-nifs([sync_dir_nif/1]).

% Sync the directory containing the given path. Accepts a chardata file name.
sync_dir(Path) ->
    sync_dir_nif(Path).

% Stub replaced by the NIF when it loads. A build without the NIF (or without a
% C compiler) keeps this one and every call reports enotsup.
sync_dir_nif(_) ->
    {error, enotsup}.

init() ->
    EbinDir = filename:dirname(code:which(?MODULE)),
    AppPath = filename:dirname(EbinDir),
    PrivDir = filename:join(AppPath, "priv"),
    % A missing or unloadable NIF must not stop this module from loading: the
    % stub above then stays in place and sync_dir/1 degrades to
    % {error, enotsup}.
    try
        case erlang:load_nif(filename:join(PrivDir, "couch_dirsync"), 0) of
            ok -> ok;
            {error, _} -> ok
        end
    catch
        _:_ -> ok
    end.
