from helpers import get_object_attribute_cache_config, get_scheduling_options, get_schema_from_description

from yt_commands import (authors, raises_yt_error, create, create_user, make_ace, exists, abort_job, write_table, get,
                         get_table_columnar_statistics, set_banned_flag, ls, abort_transaction, remove, read_table,
                         sync_create_cells, sync_mount_table, insert_rows, update_op_parameters, print_debug, merge,
                         set, remove_user)

from base import ClickHouseTestBase, Clique, QueryFailedError, UserJobFailed, InstanceUnavailableCode

from yt.common import YtError, wait, parts_to_uuid

import yt.packages.requests as requests

import yt.yson as yson

import pytest
import time
import threading
import random


class TestClickHouseCommon(ClickHouseTestBase):
    def setup(self):
        self._setup()

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "cpu": 2,
                }
            }
        }
    }

    @authors("evgenstf")
    def test_show_tables(self):
        tables = ["/t11", "/t12", "/n1/t3", "/n1/t4"]

        def create_subtrees(root):
            create("map_node", root)
            create("map_node", root + "/n1")
            for table in tables:
                create("table", root + table, attributes={"schema": [{"name": "a", "type": "string"}]})

        roots = ["//tmp/root1", "//tmp/root2"]
        for root in roots:
            create_subtrees(root)

        with Clique(1, config_patch={"yt": {"show_tables": {"roots": roots}}}) as clique:
            shown_tables = {table["name"] for table in clique.make_query("show tables")}
            for root in roots:
                for table in tables:
                    assert root + table in shown_tables
                    shown_tables.remove(root + table)
            assert len(shown_tables) == 0

            shown_tables_like_t1 = {table["name"] for table in clique.make_query("show tables like '%t1%'")}
            for root in roots:
                assert root + "/t11" in shown_tables_like_t1
                shown_tables_like_t1.remove(root + "/t11")

                assert root + "/t12" in shown_tables_like_t1
                shown_tables_like_t1.remove(root + "/t12")

        with raises_yt_error(UserJobFailed):
            with Clique(
                    1,
                    config_patch={
                        "yt": {"show_tables": {"roots": ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"]}}
                    },
            ):
                pass

    @authors("evgenstf")
    def test_subquery_columnar_data_weight(self):
        create(
            "table",
            "//tmp/t",
            attributes={"schema": [{"name": "a", "type": "string"}, {"name": "b", "type": "string"}]},
        )
        write_table("//tmp/t", [{"a": "2012-12-12 20:00:00", "b": "2012-12-12 20:00:00"}])
        column_weight = get_table_columnar_statistics('["//tmp/t{a,b}"]')[0]["column_data_weights"]["a"]

        with Clique(1, config_patch={"yt": {
            "subquery": {"max_data_weight_per_subquery": column_weight - 1}
        }}) as clique:
            with raises_yt_error(QueryFailedError):
                clique.make_query('select a from "//tmp/t"')
            with raises_yt_error(QueryFailedError):
                clique.make_query('select b from "//tmp/t"')

        with Clique(1, config_patch={"yt": {
            "subquery": {"max_data_weight_per_subquery": column_weight + 1}
        }}) as clique:
            assert clique.make_query('select a from "//tmp/t"') == [{"a": "2012-12-12 20:00:00"}]
            with raises_yt_error(QueryFailedError):
                clique.make_query('select a, b from "//tmp/t"')

        with Clique(1, config_patch={
            "yt": {
                "subquery": {
                    "use_columnar_statistics": False,
                    "max_data_weight_per_subquery": column_weight + 1
                },
            },
        }) as clique:
            with raises_yt_error(QueryFailedError):
                assert clique.make_query('select a from "//tmp/t"') == [{"a": "2012-12-12 20:00:00"}]
            with raises_yt_error(QueryFailedError):
                clique.make_query('select a, b from "//tmp/t"')

    @authors("evgenstf")
    def test_extract_array_raw(self):
        with Clique(1) as clique:
            assert clique.make_query('select YSONExtractArrayRaw(\'["a";"0";""]\')') == [
                {'YSONExtractArrayRaw(\'["a";"0";""]\')': ['"a"', '"0"', '""']}
            ]
            assert clique.make_query('select JSONExtractArrayRaw(\'["a","0",""]\')') == [
                {'JSONExtractArrayRaw(\'["a","0",""]\')': ['"a"', '"0"', '""']}
            ]

    @authors("evgenstf")
    def test_timezone(self):
        create("table", "//tmp/test_table", attributes={"schema": [{"name": "date_time", "type": "datetime"}]})
        write_table("//tmp/test_table", [{"date_time": 100}])

        with Clique(1) as clique:
            assert clique.make_query("select timezone()") == [{"timezone()": "Europe/Moscow"}]
            assert clique.make_query('select date_time from "//tmp/test_table"') == [
                {"date_time": "1970-01-01 03:01:40"}
            ]

        with Clique(1, config_patch={"clickhouse": {"timezone": "America/Los_Angeles"}}) as clique:
            assert clique.make_query("select timezone()") == [{"timezone()": "America/Los_Angeles"}]
            assert clique.make_query('select date_time from "//tmp/test_table"') == [
                {"date_time": "1969-12-31 16:01:40"}
            ]

    @authors("evgenstf")
    def test_not_table_in_query(self):
        with Clique(1) as clique:
            table_schema = [{"name": "value", "type": "int64"}]
            create("table", "//tmp/test_table", attributes={"schema": table_schema})
            write_table("//tmp/test_table", [{"value": 1}])

            # TODO(evgenstf): CHYT-112 - use error code instead of the substring
            with raises_yt_error("point to attributes"):
                clique.make_query('select * from "//tmp/test_table/@schema"')

    @authors("evgenstf")
    def test_distinct_one_instance_several_threads(self):
        with Clique(1, config_patch={"clickhouse": {"settings": {"max_threads": 2}}}, cpu_limit=2) as clique:
            table_schema = [{"name": "value", "type": "int64"}]
            create("table", "//tmp/test_table", attributes={"schema": table_schema})

            write_table("//tmp/test_table", [{"value": 1}])
            write_table("<append=%true>//tmp/test_table", [{"value": 1}])

            assert get("//tmp/test_table/@chunk_count") == 2
            assert clique.make_query('select distinct value from "//tmp/test_table" where value = 1') == [{"value": 1}]

    @authors("evgenstf")
    def test_acl(self):
        with Clique(1) as clique:
            create_user("user_with_denied_column")
            create_user("user_with_allowed_one_column")
            create_user("user_with_allowed_all_columns")

            def create_and_fill_table(path):
                create(
                    "table",
                    path,
                    attributes={"schema": [{"name": "a", "type": "string"}, {"name": "b", "type": "string"}]},
                    recursive=True,
                )
                write_table(path, [{"a": "value1", "b": "value2"}])

            create_and_fill_table("//tmp/t1")
            set(
                "//tmp/t1/@acl",
                [
                    make_ace("allow", "user_with_denied_column", "read"),
                    make_ace("deny", "user_with_denied_column", "read", columns="a"),
                ],
            )

            with raises_yt_error(QueryFailedError):
                clique.make_query('select * from "//tmp/t1"', user="user_with_denied_column")

            with raises_yt_error(QueryFailedError):
                clique.make_query('select a from "//tmp/t1"', user="user_with_denied_column")

            assert clique.make_query('select b from "//tmp/t1"', user="user_with_denied_column") == [{"b": "value2"}]

            create_and_fill_table("//tmp/t2")
            set(
                "//tmp/t2/@acl",
                [
                    make_ace("allow", "user_with_allowed_one_column", "read", columns="b"),
                    make_ace("allow", "user_with_allowed_all_columns", "read", columns="a"),
                    make_ace("allow", "user_with_allowed_all_columns", "read", columns="b"),
                ],
            )

            with raises_yt_error(QueryFailedError):
                clique.make_query('select * from "//tmp/t2"', user="user_with_allowed_one_column")
            with raises_yt_error(QueryFailedError):
                clique.make_query('select a from "//tmp/t2"', user="user_with_allowed_one_column")
            assert clique.make_query('select b from "//tmp/t2"', user="user_with_allowed_one_column") == [
                {"b": "value2"}
            ]
            assert clique.make_query('select * from "//tmp/t2"', user="user_with_allowed_all_columns") == [
                {"a": "value1", "b": "value2"}
            ]
            assert clique.make_query('select b from "//tmp/t2"', user="user_with_allowed_one_column") == [
                {"b": "value2"}
            ]
            assert clique.make_query('select * from "//tmp/t2"', user="user_with_allowed_all_columns") == [
                {"a": "value1", "b": "value2"}
            ]
            assert clique.make_query('select b from "//tmp/t2"', user="user_with_allowed_one_column") == [
                {"b": "value2"}
            ]
            assert clique.make_query('select * from "//tmp/t2"', user="user_with_allowed_all_columns") == [
                {"a": "value1", "b": "value2"}
            ]

            time.sleep(1.5)

            assert clique.make_query('select b from "//tmp/t2"', user="user_with_allowed_one_column") == [
                {"b": "value2"}
            ]
            assert clique.make_query('select * from "//tmp/t2"', user="user_with_allowed_all_columns") == [
                {"a": "value1", "b": "value2"}
            ]

            time.sleep(0.5)

            assert (
                clique.get_orchid(
                    clique.get_active_instances()[0], "/profiling/clickhouse/yt/object_attribute_cache/hit"
                )[-1]["value"]
                > 0
            )
            assert (
                clique.get_orchid(clique.get_active_instances()[0], "/profiling/clickhouse/yt/permission_cache/hit")[
                    -1
                ]["value"]
                > 0
            )

    @authors("evgenstf")
    def test_orchid_error_handle(self):
        if not exists("//sys/clickhouse/orchids"):
            create("map_node", "//sys/clickhouse/orchids")

        create_user("test_user")
        set(
            "//sys/clickhouse/@acl",
            [
                make_ace("allow", "test_user", ["write", "create", "remove", "modify_children"]),
            ],
        )
        set("//sys/accounts/sys/@acl", [make_ace("allow", "test_user", "use")])

        set(
            "//sys/clickhouse/orchids/@acl",
            [
                make_ace("deny", "test_user", "create"),
            ],
        )

        with pytest.raises(YtError):
            with Clique(1, config_patch={"yt": {"user": "test_user"}}):
                pass

    @authors("evgenstf")
    def test_orchid_nodes(self):
        node_to_ban = None
        try:
            with Clique(3) as clique:
                for i in range(3):
                    assert "monitoring" in clique.get_orchid(clique.get_active_instances()[i], "/")

                job_to_abort = str(clique.get_active_instances()[0])
                node_to_ban = clique.op.get_node(job_to_abort)

                abort_job(job_to_abort)
                set_banned_flag(True, [node_to_ban])

                def instances_relocated():
                    active_instances = [str(instance) for instance in clique.get_active_instances()]
                    if len(active_instances) != 3:
                        return False
                    return job_to_abort not in active_instances

                wait(instances_relocated)

                for i in range(3):
                    assert "monitoring" in clique.get_orchid(clique.get_active_instances()[i], "/")
        finally:
            if node_to_ban is not None:
                set_banned_flag(False, [node_to_ban])

    @authors("evgenstf")
    def test_drop_nonexistent_table(self):
        patch = get_object_attribute_cache_config(500, 500, None)
        with Clique(1, config_patch=patch) as clique:
            assert not exists("//tmp/t")
            assert clique.make_query('exists "//tmp/t"') == [{"result": 0}]
            with raises_yt_error(QueryFailedError):
                assert clique.make_query('drop table "//tmp/t"')

    @authors("evgenstf")
    def test_drop_table(self):
        patch = get_object_attribute_cache_config(500, 500, None)
        with Clique(1, config_patch=patch) as clique:
            create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "string"}]})
            write_table("//tmp/t", [{"a": "2012-12-12 20:00:00"}])
            assert clique.make_query('select * from "//tmp/t"') == [{"a": "2012-12-12 20:00:00"}]
            clique.make_query('drop table "//tmp/t"')
            time.sleep(1)
            assert not exists("//tmp/t")
            assert clique.make_query('exists "//tmp/t"') == [{"result": 0}]

    @authors("evgenstf")
    def test_subquery_data_weight_limit_exceeded(self):
        with Clique(1, config_patch={"yt": {"subquery": {"max_data_weight_per_subquery": 0}}}) as clique:
            create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "string"}]})
            write_table("//tmp/t", [{"a": "2012-12-12 20:00:00"}])
            with raises_yt_error(QueryFailedError):
                clique.make_query('select CAST(a as datetime) from "//tmp/t"')

    @authors("evgenstf")
    def test_discovery_nodes_self_cleaning(self):
        patch = {
            "yt": {
                "discovery": {
                    # Allow node cleaning 1s after creation (default is 5m).
                    "lock_node_timeout": 1000,
                }
            }
        }
        with Clique(5, config_patch=patch) as clique:
            clique_path = "//sys/clickhouse/cliques/{0}".format(clique.op.id)

            nodes_before_resizing = ls(clique_path, verbose=False)
            assert len(nodes_before_resizing) == 5

            jobs = list(clique.op.get_running_jobs())
            assert len(jobs) == 5

            clique.resize(3, jobs[:2])
            wait(lambda: len(ls(clique_path, verbose=False)) == 3, iter=10)

    @authors("evgenstf")
    def test_discovery_transaction_restore(self):
        with Clique(1) as clique:
            instances_before_transaction_abort = clique.get_active_instances()
            assert len(instances_before_transaction_abort) == 1

            locks = instances_before_transaction_abort[0].attributes["locks"]
            assert len(locks) == 1

            transaction_id = locks[0]["transaction_id"]

            abort_transaction(transaction_id)
            time.sleep(5)

            wait(lambda: clique.get_active_instance_count() == 1, iter=10)

    @authors("max42")
    @pytest.mark.parametrize("instance_count", [1, 5])
    def test_avg(self, instance_count):
        with Clique(instance_count) as clique:
            create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "int64"}]})
            for i in range(5):
                write_table("<append=%true>//tmp/t", [{"a": 2 * i}, {"a": 2 * i + 1}])

            assert abs(clique.make_query('select avg(a) from "//tmp/t"')[0]["avg(a)"] - 4.5) < 1e-6
            with raises_yt_error(QueryFailedError):
                clique.make_query('select avg(b) from "//tmp/t"')

            assert abs(clique.make_query('select avg(a) from "//tmp/t[#2:#9]"')[0]["avg(a)"] - 5.0) < 1e-6

    # YT-9497
    @authors("max42")
    def test_aggregation_with_multiple_string_columns(self):
        with Clique(1) as clique:
            create(
                "table",
                "//tmp/t",
                attributes={
                    "schema": [
                        {"name": "key1", "type": "string"},
                        {"name": "key2", "type": "string"},
                        {"name": "value", "type": "int64"},
                    ]
                },
            )
            for i in range(5):
                write_table(
                    "<append=%true>//tmp/t",
                    [{"key1": "dream", "key2": "theater", "value": i * 5 + j} for j in range(5)],
                )
            total = 24 * 25 // 2

            result = clique.make_query('select key1, key2, sum(value) from "//tmp/t" group by key1, key2')
            assert result == [{"key1": "dream", "key2": "theater", "sum(value)": total}]

    @authors("max42")
    @pytest.mark.parametrize("instance_count", [1, 2])
    def test_cast(self, instance_count):
        with Clique(instance_count) as clique:
            create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "string"}]})
            write_table("//tmp/t", [{"a": "2012-12-12 20:00:00"}])

            result = clique.make_query('select CAST(a as datetime) from "//tmp/t"')
            assert result == [{"CAST(a, 'datetime')": "2012-12-12 20:00:00"}]

    @authors("max42")
    def test_settings(self):
        with Clique(1) as clique:
            # I took some random option from the documentation and changed it in config.yson.
            # Let's see if it changed in internal table with settings.
            result = clique.make_query("select * from system.settings where name = 'max_temporary_non_const_columns'")
            assert result[0]["value"] == "1234"
            assert result[0]["changed"] == 1

    @authors("max42", "dakovalkov", "evgenstf")
    @pytest.mark.parametrize("remove_method", ["yt", "chyt"])
    def test_schema_caching(self, remove_method):
        patch = get_object_attribute_cache_config(2000, 2000, None)

        with Clique(1, config_patch=patch) as clique:
            create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "int64"}]})
            write_table("//tmp/t", [{"a": 1}])
            old_description = clique.make_query('describe "//tmp/t"')
            assert old_description[0]["name"] == "a"
            if remove_method == "yt":
                remove("//tmp/t")
            else:
                clique.make_query('drop table "//tmp/t"')
            cached_description = clique.make_query('describe "//tmp/t"')
            assert cached_description == old_description
            create("table", "//tmp/t", attributes={"schema": [{"name": "b", "type": "int64"}]})
            write_table("//tmp/t", [{"b": 1}])
            time.sleep(5)
            new_description = clique.make_query('describe "//tmp/t"')
            assert new_description[0]["name"] == "b"

    @authors("dakovalkov")
    def test_cache_auto_update(self):
        # Will never expire.
        patch = get_object_attribute_cache_config(100500, 100500, 100)

        with Clique(1, config_patch=patch) as clique:
            create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "int64"}]})
            write_table("//tmp/t", [{"a": 1}])
            old_description = clique.make_query('describe "//tmp/t"')
            assert old_description[0]["name"] == "a"

            remove("//tmp/t")
            time.sleep(0.5)
            with raises_yt_error(QueryFailedError):
                clique.make_query('describe "//tmp/t"')

            create("table", "//tmp/t", attributes={"schema": [{"name": "b", "type": "int64"}]})
            write_table("//tmp/t", [{"b": 1}])
            time.sleep(0.5)

            new_description = clique.make_query('describe "//tmp/t"')
            assert new_description[0]["name"] == "b"

    @authors("evgenstf")
    def test_concat_directory_with_mixed_objects(self):
        with Clique(1) as clique:
            create("map_node", "//tmp/test_dir")

            # static table
            create("table", "//tmp/test_dir/table_1", attributes={"schema": [{"name": "i", "type": "int64"}]})
            write_table("//tmp/test_dir/table_1", [{"i": 1}])

            # link to static table
            create("map_node", "//tmp/dir_with_static_table")
            create(
                "table", "//tmp/dir_with_static_table/table_2", attributes={"schema": [{"name": "i", "type": "int64"}]}
            )
            write_table("//tmp/dir_with_static_table/table_2", [{"i": 2}])
            create(
                "link",
                "//tmp/test_dir/link_to_table_2",
                attributes={"target_path": "//tmp/dir_with_static_table/table_2"},
            )

            # dynamic table
            sync_create_cells(1)
            create(
                "table",
                "//tmp/test_dir/table_3",
                attributes={"dynamic": True, "schema": [{"name": "i", "type": "int64"}]},
            )
            sync_mount_table("//tmp/test_dir/table_3")
            insert_rows("//tmp/test_dir/table_3", [{"i": 3}])

            # link to dynamic table
            create("map_node", "//tmp/dir_with_dynamic_table")
            create(
                "table",
                "//tmp/dir_with_dynamic_table/table_4",
                attributes={"dynamic": True, "schema": [{"name": "i", "type": "int64"}]},
            )
            sync_mount_table("//tmp/dir_with_dynamic_table/table_4", sync=True)
            insert_rows("//tmp/dir_with_dynamic_table/table_4", [{"i": 4}])
            create(
                "link",
                "//tmp/test_dir/link_to_table_4",
                attributes={"target_path": "//tmp/dir_with_dynamic_table/table_4"},
            )

            # map_node
            create("map_node", "//tmp/test_dir/map_node")

            # link to map_node
            create("map_node", "//tmp/dir_with_map_node")
            create("map_node", "//tmp/dir_with_map_node/map_node")
            create(
                "link",
                "//tmp/test_dir/link_to_map_node",
                attributes={"target_path": "//tmp/dir_with_map_node/map_node"},
            )

            # link to link to static table
            create(
                "table", "//tmp/dir_with_static_table/table_5", attributes={"schema": [{"name": "i", "type": "int64"}]}
            )
            write_table("//tmp/dir_with_static_table/table_5", [{"i": 5}])
            create("map_node", "//tmp/dir_with_link_to_static_table")
            create(
                "link",
                "//tmp/dir_with_link_to_static_table/link_to_table_5",
                attributes={"target_path": "//tmp/dir_with_static_table/table_5"},
            )
            create(
                "link",
                "//tmp/test_dir/link_to_link_to_table_5",
                attributes={"target_path": "//tmp/dir_with_link_to_static_table/link_to_table_5"},
            )

            assert clique.make_query("select * from concatYtTablesRange('//tmp/test_dir') order by i") == [
                {"i": 1},
                {"i": 2},
                {"i": 5},
            ]

    @authors("evgenstf")
    def test_concat_tables_filter_range(self):
        with Clique(1) as clique:
            create("map_node", "//tmp/test_dir")
            for table_index in range(1, 7):
                create(
                    "table",
                    "//tmp/test_dir/table_" + str(table_index),
                    attributes={"schema": [{"name": "i", "type": "int64"}]},
                    )
                write_table("//tmp/test_dir/table_" + str(table_index), [{"i": table_index}])
            assert clique.make_query(
                "select * from concatYtTablesRange('//tmp/test_dir', 'table_2', 'table_5') order by i"
            ) == [{"i": 2}, {"i": 3}, {"i": 4}, {"i": 5}]

    @authors("evgenstf")
    def test_concat_tables_filter_regexp(self):
        with Clique(1) as clique:
            create("map_node", "//tmp/test_dir")
            create("table", "//tmp/test_dir/t1", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table_2", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table_3", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table_4", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table_5", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/t6", attributes={"schema": [{"name": "i", "type": "int64"}]})
            write_table("//tmp/test_dir/t1", [{"i": 1}])
            write_table("//tmp/test_dir/table_2", [{"i": 2}])
            write_table("//tmp/test_dir/table_3", [{"i": 3}])
            write_table("//tmp/test_dir/table_4", [{"i": 4}])
            write_table("//tmp/test_dir/table_5", [{"i": 5}])
            write_table("//tmp/test_dir/t6", [{"i": 6}])
            assert clique.make_query("select * from concatYtTablesRegexp('//tmp/test_dir', 'table_*') order by i") == [
                {"i": 2},
                {"i": 3},
                {"i": 4},
                {"i": 5},
            ]

    @authors("evgenstf")
    def test_concat_tables_filter_like(self):
        with Clique(1) as clique:
            create("map_node", "//tmp/test_dir")
            create("table", "//tmp/test_dir/t1", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table_3", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table.3", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table.4", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/table_4", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/test_dir/t6", attributes={"schema": [{"name": "i", "type": "int64"}]})
            write_table("//tmp/test_dir/t1", [{"i": 1}])
            write_table("//tmp/test_dir/table_3", [{"i": 2}])
            write_table("//tmp/test_dir/table.3", [{"i": 3}])
            write_table("//tmp/test_dir/table.4", [{"i": 4}])
            write_table("//tmp/test_dir/table_4", [{"i": 5}])
            write_table("//tmp/test_dir/t6", [{"i": 6}])
            assert clique.make_query("select * from concatYtTablesLike('//tmp/test_dir', 'table.*') order by i") == [
                {"i": 3},
                {"i": 4},
            ]

    @authors("max42")
    def test_concat_tables_inside_link(self):
        with Clique(1) as clique:
            create("map_node", "//tmp/dir")
            create("link", "//tmp/link", attributes={"target_path": "//tmp/dir"})
            create("table", "//tmp/link/t1", attributes={"schema": [{"name": "i", "type": "int64"}]})
            create("table", "//tmp/link/t2", attributes={"schema": [{"name": "i", "type": "int64"}]})
            write_table("//tmp/link/t1", [{"i": 0}])
            write_table("//tmp/link/t2", [{"i": 1}])
            assert len(clique.make_query("select * from concatYtTablesRange('//tmp/link')")) == 2

    @authors("dakovalkov")
    def test_system_clique(self):
        with Clique(3) as clique:
            time.sleep(1)
            instances = clique.get_active_instances()
            assert len(instances) == 3
            responses = []

            def get_clique_list(instance):
                clique_list = clique.make_direct_query(instance, "select * from system.clique")
                for item in clique_list:
                    assert item["self"] == (1 if str(instance) == item["job_id"] else 0)
                    del item["self"]
                return sorted(clique_list)

            for instance in instances:
                responses.append(get_clique_list(instance))
            assert len(responses[0]) == 3
            for node in responses[0]:
                assert "host" in node and "rpc_port" in node and "monitoring_port" in node
                assert "tcp_port" in node and "http_port" in node and "job_id" in node
            assert responses[0] == responses[1]
            assert responses[1] == responses[2]

            jobs = list(clique.op.get_running_jobs())
            assert len(jobs) == 3

            clique.resize(2, [jobs[0]])
            clique.resize(3)

            time.sleep(1)

            instances = clique.get_active_instances()
            assert len(instances) == 3
            responses2 = []
            for instance in instances:
                responses2.append(get_clique_list(instance))
            assert len(responses2[0]) == 3
            assert responses2[0] == responses2[1]
            assert responses2[1] == responses2[2]
            assert responses != responses2

    @authors("dakovalkov")
    def test_ban_nodes(self):
        patch = {
            "yt": {
                "discovery": {
                    # Set big value to prevent unlocking node.
                    "transaction_timeout": 1000000,
                }
            }
        }
        with Clique(2, config_patch=patch) as clique:
            time.sleep(1)
            old_instances = clique.get_active_instances()
            assert len(old_instances) == 2

            for instance in old_instances:
                assert len(clique.make_direct_query(instance, "select * from system.clique")) == 2

            jobs = list(clique.op.get_running_jobs())
            assert len(jobs) == 2
            abort_job(jobs[0])

            wait(lambda: clique.get_active_instance_count() == 3)

            time.sleep(1)

            instances = clique.get_active_instances()
            # One instance is dead, but the lock should be alive.
            assert len(instances) == 3

            for instance in instances:
                if instance in old_instances:
                    # Avoid sending request to the dead instance.
                    continue

                wait(lambda: len(clique.make_direct_query(instance, "select * from system.clique")) == 2)

    @authors("dakovalkov")
    def test_single_interrupt(self):
        patch = {
            "interruption_graceful_timeout": 1000,
        }
        with Clique(1, max_failed_job_count=2, config_patch=patch) as clique:
            update_op_parameters(clique.op.id, parameters=get_scheduling_options(user_slots=0))
            instances = clique.get_active_instances()
            assert len(instances) == 1
            self._signal_instance(instances[0].attributes["pid"], "INT")
            time.sleep(0.5)
            assert len(clique.get_active_instances()) == 0
            assert clique.make_direct_query(instances[0], "select 1", full_response=True).status_code == 301
            time.sleep(0.8)
            with raises_yt_error(InstanceUnavailableCode):
                clique.make_direct_query(instances[0], "select 1")

            clique.resize(1)

            new_instances = clique.get_active_instances()
            assert len(new_instances) == 1
            assert new_instances != instances

    @authors("dakovalkov")
    def test_double_interrupt(self):
        patch = {
            "interruption_graceful_timeout": 10000,
        }
        with Clique(1, max_failed_job_count=2, config_patch=patch) as clique:
            update_op_parameters(clique.op.id, parameters=get_scheduling_options(user_slots=0))
            instances = clique.get_active_instances()
            assert len(instances) == 1
            pid = instances[0].attributes["pid"]
            self._signal_instance(pid, "INT")
            time.sleep(0.2)
            assert len(clique.get_active_instances()) == 0
            assert clique.make_direct_query(instances[0], "select 1", full_response=True).status_code == 301
            self._signal_instance(pid, "INT")
            time.sleep(0.2)
            with raises_yt_error(InstanceUnavailableCode):
                clique.make_direct_query(instances[0], "select 1")

            clique.resize(1)

            new_instances = clique.get_active_instances()
            assert len(new_instances) == 1
            assert new_instances != instances

    @authors("dakovalkov")
    def test_long_query_interrupt(self):
        patch = {
            "interruption_graceful_timeout": 1000,
        }
        with Clique(1, max_failed_job_count=2, config_patch=patch) as clique:
            update_op_parameters(clique.op.id, parameters=get_scheduling_options(user_slots=0))
            instances = clique.get_active_instances()
            assert len(instances) == 1

            def signal_job_later():
                time.sleep(0.3)
                self._signal_instance(instances[0].attributes["pid"], "INT")

            signal_thread = threading.Thread(target=signal_job_later)
            signal_thread.start()

            assert clique.make_direct_query(instances[0], "select sleep(3)") == [{"sleep(3)": 0}]
            time.sleep(0.3)
            with raises_yt_error(InstanceUnavailableCode):
                clique.make_direct_query(instances[0], "select 1")

            clique.resize(1)

            new_instances = clique.get_active_instances()
            assert len(new_instances) == 1
            assert new_instances != instances

    @authors("dakovalkov")
    def test_convert_yson(self):
        create(
            "table",
            "//tmp/table",
            attributes={"schema": [{"name": "i", "type": "any"}, {"name": "fmt", "type": "string"}]},
        )
        value1 = 1
        value2 = [1, 2]
        value3 = {"key": "value"}
        write_table(
            "//tmp/table",
            [
                {"i": value1, "fmt": "binary"},
                {"i": value2, "fmt": "pretty"},
                {"i": value3, "fmt": "text"},
                {"i": None, "fmt": "text"},
            ],
        )
        with Clique(1) as clique:
            value = {"key": [1, 2]}
            func = "ConvertYson('" + yson.dumps(value, yson_format="text") + "', 'pretty')"
            assert clique.make_query("select " + func) == [{func: yson.dumps(value, yson_format="pretty")}]
            func = "ConvertYson(NULL, 'text')"
            assert clique.make_query("select " + func) == [{func: None}]
            func = "ConvertYson(i, 'text')"
            assert clique.make_query("select " + func + ' from "//tmp/table"') == [
                {func: yson.dumps(value1, yson_format="text")},
                {func: yson.dumps(value2, yson_format="text")},
                {func: yson.dumps(value3, yson_format="text")},
                {func: None},
            ]
            func = "ConvertYson(i, fmt)"
            assert clique.make_query("select " + func + ' from "//tmp/table"') == [
                {func: yson.dumps(value1, yson_format="binary")},
                {func: yson.dumps(value2, yson_format="pretty")},
                {func: yson.dumps(value3, yson_format="text")},
                {func: None},
            ]
            with raises_yt_error(QueryFailedError):
                clique.make_query("select ConvertYson('{key=[1;2]}', NULL)")
            with raises_yt_error(QueryFailedError):
                clique.make_query("select ConvertYson('{key=[1;2]}', 'xxx')")
            with raises_yt_error(QueryFailedError):
                clique.make_query("select ConvertYson('{{{{', 'binary')")
            with raises_yt_error(QueryFailedError):
                clique.make_query("select ConvertYson(1, 'text')")

    @authors("dakovalkov")
    def test_reject_request(self):
        with Clique(1) as clique:
            instance = clique.get_active_instances()[0]

            host = instance.attributes["host"]
            port = instance.attributes["http_port"]
            query_id = parts_to_uuid(random.randint(0, 2 ** 64 - 1), random.randint(0, 2 ** 64 - 1))

            result = requests.post(
                "http://{}:{}/query?query_id={}".format(host, port, query_id),
                data="select 1",
                headers={"X-ClickHouse-User": "root", "X-Yt-Request-Id": query_id, "X-Clique-Id": "wrong-id"},
            )
            print_debug(result.content)
            assert result.status_code == 301

            result = requests.post(
                "http://{}:{}/query?query_id={}".format(host, port, query_id),
                data="select 1",
                headers={"X-ClickHouse-User": "root", "X-Yt-Request-Id": query_id, "X-Clique-Id": clique.op.id},
            )
            print_debug(result.content)
            assert result.status_code == 200

            self._signal_instance(instance.attributes["pid"], "INT")

            result = requests.post(
                "http://{}:{}/query?query_id={}".format(host, port, query_id),
                data="select 1",
                headers={"X-ClickHouse-User": "root", "X-Yt-Request-Id": query_id, "X-Clique-Id": clique.op.id},
            )
            print_debug(result.content)
            assert result.status_code == 301

    @authors("dakovalkov")
    def test_exists_table(self):
        create("table", "//tmp/t1", attributes={"schema": [{"name": "a", "type": "int64"}]})
        with Clique(1) as clique:
            assert clique.make_query('exists table "//tmp/t1"') == [{"result": 1}]
            # Table doesn't exist.
            assert clique.make_query('exists table "//tmp/t123456"') == [{"result": 0}]
            # Not a table.
            with raises_yt_error(QueryFailedError):
                clique.make_query('exists table "//sys"')

    @authors("dakovalkov")
    def test_date_types(self):
        create(
            "table",
            "//tmp/t1",
            attributes={
                "schema": [
                    {"name": "datetime", "type": "datetime"},
                    {"name": "date", "type": "date"},
                    {"name": "timestamp", "type": "timestamp"},
                    {"name": "interval_", "type": "interval"},
                ]
            },
        )
        write_table(
            "//tmp/t1",
            [
                {
                    "datetime": 1,
                    "date": 2,
                    "timestamp": 3,
                    "interval_": 4,
                },
            ],
        )
        with Clique(1) as clique:
            assert get_schema_from_description(clique.make_query('describe "//tmp/t1"')) == [
                {"name": "datetime", "type": "Nullable(DateTime)"},
                {"name": "date", "type": "Nullable(Date)"},
                # TODO(dakovalkov): https://github.com/yandex/ClickHouse/pull/7170.
                # {"name": "timestamp", "type": "Nullable(DateTime64)"},
                {"name": "timestamp", "type": "Nullable(UInt64)"},
                {"name": "interval_", "type": "Nullable(Int64)"},
            ]
            assert clique.make_query(
                "select toTimeZone(datetime, 'UTC') as datetime, date, timestamp, interval_ from \"//tmp/t1\""
            ) == [
                {
                    "datetime": "1970-01-01 00:00:01",
                    "date": "1970-01-03",
                    "timestamp": 3,
                    "interval_": 4,
                }
            ]
            clique.make_query('create table "//tmp/t2" engine YtTable() as select * from "//tmp/t1"')
            assert get_schema_from_description(get("//tmp/t2/@schema")) == [
                {"name": "datetime", "type": "datetime"},
                {"name": "date", "type": "date"},
                # TODO(dakovalkov): https://github.com/yandex/ClickHouse/pull/7170.
                # {"name": "timestamp", "type": "timestamp"},
                {"name": "timestamp", "type": "uint64"},
                {"name": "interval_", "type": "int64"},
            ]
            assert read_table("//tmp/t1") == read_table("//tmp/t2")

    @authors("dakovalkov")
    def test_yson_extract(self):
        with Clique(1) as clique:
            assert clique.make_query("select YSONHas('{a=5;b=6}', 'a') as a") == [{"a": 1}]
            assert clique.make_query("select YSONHas('{a=5;b=6}', 'c') as a") == [{"a": 0}]
            assert clique.make_query("select YSONHas('{a=5;b=[5; 4; 3]}', 'b', 1) as a") == [{"a": 1}]

            assert clique.make_query("select YSONLength('{a=5;b=6}') as a") == [{"a": 2}]
            assert clique.make_query("select YSONLength('{a=5;b=[5; 4; 3]}', 'b') as a") == [{"a": 3}]

            assert clique.make_query("select YSONKey('{a=5;b={c=4}}', 'b', 'c') as a") == [{"a": "c"}]

            assert clique.make_query("select YSONType('{a=5}') as a") == [{"a": "Object"}]
            assert clique.make_query("select YSONType('[1; 3; 4]') as a") == [{"a": "Array"}]
            assert clique.make_query("select YSONType('{a=5;b=4}', 'b') as a") == [{"a": "Int64"}]

            assert clique.make_query("select YSONExtractInt('{a=5;b=[5; 4; 3]}', 'b', 1) as a") == [{"a": 5}]

            assert clique.make_query("select YSONExtractUInt('{a=5;b=[5; 4; 3]}', 'b', 1) as a") == [{"a": 5}]

            assert clique.make_query("select YSONExtractFloat('[1; 2; 4.4]', 3) as a") == [{"a": 4.4}]

            assert clique.make_query("select YSONExtractBool('[%true; %false]', 1) as a") == [{"a": 1}]
            assert clique.make_query("select YSONExtractBool('[%true; %false]', 2) as a") == [{"a": 0}]

            assert clique.make_query("select YSONExtractString('[true; false]', 1) as a") == [{"a": "true"}]
            assert clique.make_query("select YSONExtractString('{a=true; b=false}', 'b') as a") == [{"a": "false"}]

            assert clique.make_query("select YSONExtract('{a=5;b=[5; 4; 3]}', 'b', 'Array(Int64)') as a") == [
                {"a": [5, 4, 3]}
            ]

            assert sorted(
                clique.make_query("select YSONExtractKeysAndValues('[{a=5};{a=5;b=6;c=10}]', 2, 'Int8') as a")[0]["a"]
            ) == [["a", 5], ["b", 6], ["c", 10]]

            assert yson.loads(clique.make_query("select YSONExtractRaw('[{a=5};{a=5;b=6;c=10}]', 2) as a")[0]["a"]) == {
                "a": 5,
                "b": 6,
                "c": 10,
            }

    @authors("dakovalkov")
    def test_yson_extract_invalid(self):
        with Clique(1) as clique:
            assert clique.make_query("select YSONLength('{a=5;b=6}', 'invalid_key') as a") == [{"a": 0}]
            assert clique.make_query("select YSONKey('{a=5;b={c=4}}', 'b', 'c', 'invalid_key') as a") == [{"a": ""}]
            assert clique.make_query("select YSONType('{a=5}', 'invalid_key') as a") == [{"a": "Null"}]
            assert clique.make_query("select YSONExtractInt('{a=5;b=[5; 4; 3]}', 'b', 100500) as a") == [{"a": 0}]
            assert clique.make_query("select YSONExtractUInt('{a=5;b=[5; 4; 3]}', 'b', -100500) as a") == [{"a": 0}]
            assert clique.make_query("select YSONExtractFloat('[1; 2; 4.4]', 42) as a") == [{"a": 0.0}]
            assert clique.make_query("select YSONExtractBool('[%true; %false]', 10) as a") == [{"a": 0}]
            assert clique.make_query("select YSONExtractString('[true; false]', 10) as a") == [{"a": ""}]
            assert clique.make_query("select YSONExtractString('{a=true; b=false}', 'invalid_key') as a") == [{"a": ""}]
            assert clique.make_query("select YSONExtract('{a=5;b=[5; 4; 3]}', 'invalid_key', 'Array(Int64)') as a") == [
                {"a": []}
            ]
            assert (
                clique.make_query("select YSONExtractKeysAndValues('[{a=5};{a=5;b=6;c=10}]', 2, 10, 'Int8') as a")[0][
                    "a"
                ]
                == []
            )
            assert clique.make_query("select YSONExtractRaw('[{a=5};{a=5;b=6;c=10}]', 2, 1) as a") == [{"a": ""}]

            assert clique.make_query("select YSONExtractString('{Invalid_YSON') as a") == [{"a": ""}]

    @authors("max42")
    def test_old_chunk_schema(self):
        # CHYT-256.
        create("table", "//tmp/t1", attributes={"schema": [{"name": "a", "type": "int64"}]})
        create(
            "table", "//tmp/t2", attributes={"schema": [{"name": "a", "type": "int64"}, {"name": "b", "type": "int64"}]}
        )
        write_table("//tmp/t1", [{"a": 1}])
        merge(in_=["//tmp/t1"], out="//tmp/t2", mode="ordered")

        with Clique(1) as clique:
            assert clique.make_query('select b from "//tmp/t2"') == [{"b": None}]

    @authors("max42")
    def test_nothing(self):
        with Clique(5):
            pass

    @authors("max42")
    def test_any_empty_result(self):
        # CHYT-338, CHYT-246.
        create(
            "table",
            "//tmp/t",
            attributes={
                "schema": [
                    {"name": "key", "type": "int64", "sort_order": "ascending"},
                    {"name": "value", "type": "string"},
                ]
            },
        )
        write_table("//tmp/t", [{"key": 1, "value": "a"}])

        with Clique(1) as clique:
            assert clique.make_query("select any(value) from `//tmp/t` where key = 2") == [{"any(value)": None}]

    @authors("max42")
    def test_constants(self):
        # CHYT-400.
        create(
            "table",
            "//tmp/t",
            attributes={"schema": [{"name": "key", "type": "int64"}, {"name": "value", "type": "string"}]},
        )
        write_table("//tmp/t", [{"key": 1, "value": "a"}])

        with Clique(1) as clique:
            assert clique.make_query("select 1 from `//tmp/t`") == [{"1": 1}]

    @authors("max42")
    def test_group_by(self):
        # CHYT-401.
        create(
            "table",
            "//tmp/t",
            attributes={"schema": [{"name": "key", "type": "int64"}, {"name": "value", "type": "int64"}]},
        )
        write_table(
            "//tmp/t", [{"key": 1, "value": 3}, {"key": 2, "value": 1}, {"key": 1, "value": 2}, {"key": 2, "value": 5}]
        )

        with Clique(1) as clique:
            assert clique.make_query("select key, min(value), max(value) from `//tmp/t` group by key order by key") == [
                {"key": 1, "min(value)": 2, "max(value)": 3},
                {"key": 2, "min(value)": 1, "max(value)": 5},
            ]

    @authors("dakovalkov")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_virtual_columns(self, optimize_for):
        for i in range(4):
            table_path = "//tmp/t{}".format(i)
            create(
                "table",
                table_path,
                attributes={
                    "schema": [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                    ],
                    "optimize_for": optimize_for,
                },
            )
            write_table(
                table_path, [{"key": i}]
            )

        with Clique(1) as clique:
            # Virtual columns are not visible via 'select *' and 'describe'.
            assert clique.make_query("select * from `//tmp/t0`") == [{"key": 0}]
            query = "select * from concatYtTables('//tmp/t0', '//tmp/t1', '//tmp/t2', '//tmp/t3')"
            assert sorted(clique.make_query(query)) == [{"key": index} for index in range(4)]
            assert len(clique.make_query("describe `//tmp/t0`")) == 1
            assert len(clique.make_query("describe concatYtTables('//tmp/t0', '//tmp/t1')")) == 1

            def get_table_virtual_values(index):
                return {
                    "$table_index": index,
                    "$table_path": "//tmp/t{}".format(index),
                    "$table_name": "t{}".format(index),
                }

            def get_table_content(index):
                result = get_table_virtual_values(index)
                result["key"] = index
                return result

            query = "select *, $table_index, $table_path, $table_name from `//tmp/t0`"
            assert clique.make_query(query) == [get_table_content(0)]
            query = '''
                select *, $table_index, $table_path, $table_name
                from concatYtTables('//tmp/t0', '//tmp/t1', '//tmp/t2', '//tmp/t3')
            '''
            assert sorted(clique.make_query(query)) == [get_table_content(index) for index in range(4)]

            # Select only virtual values.
            query = '''
                select $table_index, $table_path, $table_name
                from concatYtTables('//tmp/t0', '//tmp/t1', '//tmp/t2', '//tmp/t3')
            '''
            assert sorted(clique.make_query(query)) == [get_table_virtual_values(index) for index in range(4)]

            # Join on virtual column.
            # XXX(dakovalkov): this does not work (https://github.com/ClickHouse/ClickHouse/issues/17860).
            # query = '''
            #     select * from concatYtTables("//tmp/t0", "//tmp/t1") as a
            #     global join "//tmp/t2" as b
            #     using $table_index
            # '''
            # assert sorted(clique.make_query(query)) == [
            #     {"key": 0, "b.key": 2},
            # ]

            query = '''
                select * from concatYtTables("//tmp/t0", "//tmp/t1") as a
                global join (select *, $table_index from concatYtTables("//tmp/t2", "//tmp/t3")) as b
                using $table_index
            '''
            assert sorted(clique.make_query(query)) == [
                {"key": 0, "b.key": 2},
                {"key": 1, "b.key": 3},
            ]

            # TODO(dakovalkov): This should not work since virtual columns are not key columns,
            # but the error is strange.
            # query = '''
            #     select * from concatYtTables("//tmp/t0", "//tmp/t1") as a
            #     join concatYtTables("//tmp/t2", "//tmp/t3") as b
            #     using $table_index
            # '''
            # assert sorted(clique.make_query(query)) == [
            #     {"key": 0, "b.key": 2},
            #     {"key": 1, "b.key": 3},
            # ]

    @authors("dakovalkov")
    @pytest.mark.skipif(True, reason="Virtual columns are not supported in dynamic tables (CHYT-506)")
    def test_virtual_columns_in_dynamic_tables(self):
        for i in range(2):
            table_path = "//tmp/dt{}".format(i)
            create(
                "table",
                table_path,
                attributes={
                    "dynamic": True,
                    "schema": [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                        {"name": "value", "type": "int64"},
                    ],
                    "enable_dynamic_store_read": True,
                },
            )
            sync_mount_table(table_path)
            insert_rows(table_path, [{"key": i, "value": i + 10}])

        with Clique(1, config_patch={"yt": {"enable_dynamic_tables": True}}) as clique:
            def get_table_virtual_values(index):
                return {
                    "$table_index": index,
                    "$table_path": "//tmp/dt{}".format(index),
                    "$table_name": "dt{}".format(index),
                }

            def get_table_content(index):
                result = get_table_virtual_values(index)
                result["key"] = index
                result["value"] = index + 10
                return result

            assert clique.make_query("select * from `//tmp/dt0`") == [{"key": 0, "value": 10}]

            query = "select *, $table_index, $table_path, $table_name from `//tmp/dt0`"
            assert clique.make_query(query) == [get_table_content(0)]

            query = '''
                select *, $table_index, $table_path, $table_name
                from concatYtTables("//tmp/dt0", "//tmp/dt1")
            '''
            assert sorted(clique.make_query(query)) == [get_table_content(index) for index in range(2)]

            query = '''
                select $table_index, $table_path, $table_name
                from concatYtTables("//tmp/dt0", "//tmp/dt1")
            '''
            assert sorted(clique.make_query(query)) == [get_table_virtual_values(index) for index in range(2)]

    @authors("dakovalkov")
    def test_virtual_column_index(self):
        rows_per_table = 10
        table_data = []

        for i in range(4):
            table_path = "//tmp/t{}".format(i)
            create(
                "table",
                table_path,
                attributes={
                    "schema": [
                        {"name": "key", "type": "int64"},
                        {"name": "subkey", "type": "int64"},
                    ],
                },
            )
            rows = [{"key": i, "subkey": j} for j in range(0, rows_per_table)]
            write_table(table_path, rows)
            table_data.append(rows)

        with Clique(1) as clique:
            # Simple.
            query = "select * from concatYtTablesRange('//tmp') where $table_index = 2"
            assert sorted(clique.make_query_and_validate_row_count(query, exact=(1 * rows_per_table))) == \
                   table_data[2]

            # Non-monotonic transformation.
            query = "select * from concatYtTablesRange('//tmp') where $table_index % 2 = 0"
            assert sorted(clique.make_query_and_validate_row_count(query, exact=(2 * rows_per_table))) == \
                   table_data[0] + table_data[2]

            # Several expressions.
            query = """
            select * from concatYtTablesRange('//tmp')
            where $table_index = 0 or $table_name = 't1' or $table_path = '//tmp/t2'
            """
            assert sorted(clique.make_query_and_validate_row_count(query, exact=(3 * rows_per_table))) == \
                   table_data[0] + table_data[1] + table_data[2]

            # Non-monotonic transformation + $table_index check.
            query = """
            select *, $table_index from concatYtTablesRange('//tmp')
            where endsWith($table_path, '1')
            """
            assert sorted(clique.make_query_and_validate_row_count(query, exact=(1 * rows_per_table))) == \
                   [{"key": 1, "$table_index": 1, "subkey": i} for i in range(0, rows_per_table)]


class TestClickHouseNoCache(ClickHouseTestBase):
    def setup(self):
        self._setup()
        remove_user("yt-clickhouse-cache")

    @authors("dakovalkov")
    def test_no_clickhouse_cache(self):
        patch = {
            "yt": {
                "permission_cache": {
                    "refresh_time": 250,
                },
                "table_attribute_cache": {
                    "refresh_time": 250,
                },
            }
        }
        create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "int64"}]})
        write_table("//tmp/t", [{"a": 123}])
        with Clique(1, config_patch=patch) as clique:
            for i in range(4):
                if i != 0:
                    time.sleep(0.5)
                assert clique.make_query('select * from "//tmp/t"') == [{"a": 123}]


class TestCustomSettings(ClickHouseTestBase):
    def setup(self):
        self._setup()

    @authors("max42")
    def test_simple(self):
        create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "int64"}]})
        write_table("//tmp/t", [{"a": 1}])
        with Clique(1) as clique:
            for throw_exception_in_distributor in (None, False, True):
                for throw_exception_in_subquery in (None, False, True):
                    settings = {}
                    if throw_exception_in_distributor is not None:
                        settings["chyt.testing.throw_exception_in_distributor"] = int(
                            throw_exception_in_distributor
                        )
                    if throw_exception_in_subquery is not None:
                        settings["chyt.testing.throw_exception_in_subquery"] = int(throw_exception_in_subquery)
                    if throw_exception_in_subquery is not None:
                        assert clique.make_query(
                            "select CAST(getSetting('chyt.testing.throw_exception_in_subquery') as Int64) as v",
                            settings=settings,
                        ) == [{"v": int(throw_exception_in_subquery)}]
                    if throw_exception_in_distributor is not None:
                        assert clique.make_query(
                            "select CAST(getSetting('chyt.testing.throw_exception_in_distributor') as Int64) as v",
                            settings=settings,
                        ) == [{"v": int(throw_exception_in_distributor)}]
                    if not bool(throw_exception_in_distributor) and not bool(
                            throw_exception_in_subquery
                    ):
                        assert clique.make_query("select * from `//tmp/t`", settings=settings) == [{"a": 1}]
                    else:
                        if bool(throw_exception_in_distributor):
                            error_substr = "Testing exception in distributor"
                        else:
                            error_substr = "Testing exception in subquery"
                        with raises_yt_error(error_substr):
                            clique.make_query("select * from `//tmp/t`", settings=settings)

    @authors("max42")
    def test_defaults(self):
        create("table", "//tmp/t", attributes={"schema": [{"name": "a", "type": "int64"}]})
        write_table("//tmp/t", [{"a": 1}])
        for default_value in (None, False, True):

            default_settings = {}
            if default_value is not None:
                default_settings = {
                    "testing": {
                        "throw_exception_in_distributor": int(default_value),
                    },
                }

            with Clique(1, config_patch={"yt": {"settings": default_settings}}) as clique:
                for override_value in (None, False, True):
                    value = False
                    if default_value is not None:
                        value = default_value
                    if override_value is not None:
                        value = override_value
                    settings = (
                        {"chyt.testing.throw_exception_in_distributor": int(override_value)}
                        if override_value is not None
                        else {}
                    )
                    if value:
                        with raises_yt_error("Testing exception in distributor"):
                            clique.make_query("select * from `//tmp/t`", settings=settings)
                    else:
                        assert clique.make_query("select * from `//tmp/t`", settings=settings) == [{"a": 1}]

    @authors("max42")
    def test_boolean(self):
        create("table", "//tmp/t", attributes={"schema": [{"name": "b", "type": "boolean", "required": True}]})
        write_table("//tmp/t", [{"b": False}, {"b": True}])
        with Clique(1) as clique:
            assert clique.make_query("select b, 2 * b as two_b  from `//tmp/t`") == \
                   [{"b": 0, "two_b": 0}, {"b": 1, "two_b": 2}]
            assert clique.make_query("select toTypeName(b) as tb, toTypeName(2 * b) as t2b from `//tmp/t` limit 1") == \
                   [{"tb": "YtBoolean", "t2b": "UInt16"}]
            assert get_schema_from_description(clique.make_query("describe `//tmp/t`")) == \
                   [{"name": "b", "type": "YtBoolean"}]