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

-module(couch_dirsync_tests).

-include_lib("couch/include/couch_eunit.hrl").

% Directory syncs have no observable effect inside a running VM, so these tests
% cover what can be checked here: the NIF reports success and failure like any
% other file operation, and the couch_file helpers that call it still leave the
% filesystem in the expected state.

setup() ->
    {ok, _} = application:ensure_all_started(config),
    {ok, _} = application:ensure_all_started(couch_log),
    {ok, _} = application:ensure_all_started(couch_stats),
    Dir = ?tempfile() ++ "-dir",
    ok = filelib:ensure_dir(filename:join(Dir, "x")),
    Dir.

teardown(Dir) ->
    filelib:fold_files(Dir, ".*", true, fun(F, _) -> file:delete(F) end, ok),
    file:del_dir(Dir),
    application:stop(couch_stats),
    application:stop(couch_log),
    application:stop(config).

sync_dir_test_() ->
    {
        foreach,
        fun setup/0,
        fun teardown/1,
        [
            ?TDEF_FE(t_syncs_existing_dir),
            ?TDEF_FE(t_missing_dir_is_enoent),
            ?TDEF_FE(t_rename_is_durable),
            ?TDEF_FE(t_rename_missing_is_enoent),
            ?TDEF_FE(t_create_and_delete_couch_file)
        ]
    }.

t_syncs_existing_dir(Dir) ->
    case couch_dirsync:sync_dir(Dir) of
        ok ->
            ok;
        {error, enotsup} ->
            % Built without the NIF (no C compiler). couch_file:sync_dir/1 is
            % documented to carry on in that case, so nothing to assert.
            ok
    end.

t_missing_dir_is_enoent(Dir) ->
    case couch_dirsync:sync_dir(filename:join(Dir, "no-such-dir")) of
        {error, enoent} -> ok;
        {error, enotsup} -> ok
    end.

t_rename_is_durable(Dir) ->
    Source = filename:join(Dir, "before.couch"),
    Target = filename:join(Dir, "after.couch"),
    ok = file:write_file(Source, <<"x">>),
    ?assertEqual(ok, couch_file:rename(Source, Target)),
    ?assertEqual({error, enoent}, file:read_file_info(Source)),
    ?assertMatch({ok, _}, file:read_file_info(Target)).

t_rename_missing_is_enoent(Dir) ->
    Source = filename:join(Dir, "no-such-file.couch"),
    Target = filename:join(Dir, "unused.couch"),
    ?assertEqual({error, enoent}, couch_file:rename(Source, Target)).

% The create and delete paths of couch_file both sync the directory as part of
% their work; that side of it is only observable outside the VM. What is
% checked here is that going through those paths still succeeds and still
% leaves the expected files behind.
t_create_and_delete_couch_file(Dir) ->
    Path = filename:join(Dir, "db.couch"),
    {ok, Fd} = couch_file:open(Path, [create]),
    ok = couch_file:close(Fd),
    ?assert(filelib:is_file(Path)),
    ok = couch_file:init_delete_dir(Dir),
    ?assertEqual(ok, couch_file:delete(Dir, Path, [sync])),
    ?assertEqual({error, enoent}, file:read_file_info(Path)).
