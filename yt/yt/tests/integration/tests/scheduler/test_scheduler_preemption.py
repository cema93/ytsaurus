from yt_env_setup import YTEnvSetup

from yt_commands import (
    authors, print_debug, wait, wait_breakpoint, release_breakpoint, with_breakpoint,
    events_on_fs, reset_events_on_fs,
    create, ls, get, set, remove, exists, create_pool, read_table, write_table,
    map, run_test_vanilla, run_sleeping_vanilla, get_job,
    sync_create_cells, update_controller_agent_config, update_op_parameters,
    create_test_tables, retry)

from yt_scheduler_helpers import (
    scheduler_orchid_pool_path, scheduler_orchid_default_pool_tree_path,
    scheduler_orchid_operation_path, scheduler_orchid_default_pool_tree_config_path)

from yt.packages.six.moves import xrange

from yt.test_helpers import are_almost_equal

from yt.common import YtError

import yt.environment.init_operation_archive as init_operation_archive

import pytest

import time
import datetime

##################################################################


def get_scheduling_options(user_slots):
    return {"scheduling_options_per_pool_tree": {"default": {"resource_limits": {"user_slots": user_slots}}}}


##################################################################


class TestSchedulerPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "fair_share_update_period": 100,
            "event_log": {
                "flush_period": 300,
                "retry_backoff_time": 300,
            },
            "enable_job_reporter": True,
            "enable_job_spec_reporter": True,
            "enable_job_stderr_reporter": True,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "event_log": {
                "flush_period": 300,
                "retry_backoff_time": 300
            },
            "enable_operation_progress_archivation": False,
        }
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_reporter": {
                "enabled": True,
                "reporting_period": 10,
                "min_repeat_delay": 10,
                "max_repeat_delay": 10,
            },
            "job_controller": {
                "resource_limits": {
                    "cpu": 1,
                    "user_slots": 2,
                },
            },
        },
    }

    def setup(self):
        sync_create_cells(1)
        init_operation_archive.create_tables_latest_version(
            self.Env.create_native_client(),
            override_tablet_cell_bundle="default",
        )

    def setup_method(self, method):
        super(TestSchedulerPreemption, self).setup_method(method)
        set("//sys/pool_trees/default/@config/preemption_satisfaction_threshold", 0.99)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.8)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout", 1000)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 0)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/job_graceful_interrupt_timeout", 10000)
        time.sleep(0.5)

    @authors("ignat")
    def test_preemption(self):
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        create("table", "//tmp/t_in")
        for i in xrange(3):
            write_table("<append=true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op1 = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
            spec={"pool": "fake_pool", "job_count": 3, "locality_timeout": 0},
        )

        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"
        wait(lambda: exists(pools_path + "/fake_pool"))
        wait(lambda: get(pools_path + "/fake_pool/fair_share_ratio") >= 0.999)
        wait(lambda: get(pools_path + "/fake_pool/usage_ratio") >= 0.999)

        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu")
        create_pool("test_pool", attributes={"min_share_resources": {"cpu": total_cpu_limit}})
        op2 = map(
            track=False,
            command="cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
            spec={"pool": "test_pool"},
        )
        op2.track()

        op1.abort()

    @authors("ignat")
    @pytest.mark.parametrize("interruptible", [False, True])
    def test_interrupt_job_on_preemption(self, interruptible):
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout", 100)
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        create("table", "//tmp/t_in")
        write_table(
            "//tmp/t_in",
            [{"key": "%08d" % i, "value": "(foo)", "data": "a" * (2 * 1024 * 1024)} for i in range(6)],
            table_writer={"block_size": 1024, "desired_chunk_size": 1024},
        )

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        spec = {
            "pool": "fake_pool",
            "locality_timeout": 0,
            "enable_job_splitting": False,
            "max_failed_job_count": 1,
        }
        if interruptible:
            data_size_per_job = get("//tmp/t_in/@uncompressed_data_size")
            spec["data_size_per_job"] = data_size_per_job / 3 + 1
        else:
            spec["job_count"] = 3

        mapper = " ; ".join([events_on_fs().breakpoint_cmd(), "sleep 7", "cat"])
        op1 = map(
            track=False,
            command=mapper,
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
            spec=spec,
        )

        time.sleep(3)

        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"
        assert get(pools_path + "/fake_pool/fair_share_ratio") >= 0.999
        assert get(pools_path + "/fake_pool/usage_ratio") >= 0.999

        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu")
        create_pool("test_pool", attributes={"min_share_resources": {"cpu": total_cpu_limit}})

        # Ensure that all three jobs have started.
        events_on_fs().wait_breakpoint(timeout=datetime.timedelta(1000), job_count=3)
        events_on_fs().release_breakpoint()

        op2 = map(
            track=False,
            command="cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
            spec={"pool": "test_pool", "max_failed_job_count": 1},
        )
        op2.track()
        op1.track()
        assert get(op1.get_path() + "/@progress/jobs/completed/total") == (4 if interruptible else 3)

    @authors("dakovalkov")
    def test_graceful_preemption(self):
        create_test_tables(row_count=1)

        command = """(trap "sleep 1; echo '{interrupt=42}'; exit 0" SIGINT; BREAKPOINT)"""

        op = map(
            track=False,
            command=with_breakpoint(command),
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "mapper": {
                    "interruption_signal": "SIGINT",
                },
                "preemption_mode": "graceful",
            },
        )

        wait_breakpoint()

        update_op_parameters(op.id, parameters=get_scheduling_options(user_slots=0))
        wait(lambda: op.get_job_count("running") == 0)
        op.track()
        assert op.get_job_count("completed", from_orchid=False) == 1
        assert op.get_job_count("total", from_orchid=False) == 1
        assert read_table("//tmp/t_out") == [{"interrupt": 42}]

    @authors("dakovalkov")
    def test_graceful_preemption_timeout(self):
        op = run_test_vanilla(
            command=with_breakpoint("BREAKPOINT ; sleep 100"),
            spec={"preemption_mode": "graceful"}
        )

        wait_breakpoint()
        release_breakpoint()
        update_op_parameters(op.id, parameters=get_scheduling_options(user_slots=0))
        wait(lambda: op.get_job_count("aborted") == 1)
        assert op.get_job_count("total") == 1
        assert op.get_job_count("aborted") == 1

    @authors("ignat")
    def test_min_share_ratio(self):
        create_pool("test_min_share_ratio_pool", attributes={"min_share_resources": {"cpu": 3}})

        get_operation_min_share_ratio = lambda op: op.get_runtime_progress("scheduling_info_per_pool_tree/default/min_share_ratio", 0.0)

        min_share_settings = [{"cpu": 3}, {"cpu": 1, "user_slots": 6}]

        for min_share_spec in min_share_settings:
            reset_events_on_fs()
            op = run_test_vanilla(
                with_breakpoint("BREAKPOINT"),
                spec={
                    "pool": "test_min_share_ratio_pool",
                    "min_share_resources": min_share_spec,
                },
                job_count=3,
            )
            wait_breakpoint()

            wait(lambda: get_operation_min_share_ratio(op) == 1.0)

            release_breakpoint()
            op.track()

    @authors("ignat")
    def test_recursive_preemption_settings(self):
        def get_pool_tolerance(pool):
            return get(scheduler_orchid_pool_path(pool) + "/effective_fair_share_starvation_tolerance")

        def get_operation_tolerance(op):
            return op.get_runtime_progress(
                "scheduling_info_per_pool_tree/default/effective_fair_share_starvation_tolerance", 0.0)

        create_pool("p1", attributes={"fair_share_starvation_tolerance": 0.6})
        create_pool("p2", parent_name="p1")
        create_pool("p3", parent_name="p1", attributes={"fair_share_starvation_tolerance": 0.5})
        create_pool("p4", parent_name="p1", attributes={"fair_share_starvation_tolerance": 0.9})
        create_pool("p5", attributes={"fair_share_starvation_tolerance": 0.8})
        create_pool("p6", parent_name="p5")

        wait(lambda: get_pool_tolerance("p1") == 0.6)
        wait(lambda: get_pool_tolerance("p2") == 0.6)
        wait(lambda: get_pool_tolerance("p3") == 0.5)
        wait(lambda: get_pool_tolerance("p4") == 0.9)
        wait(lambda: get_pool_tolerance("p5") == 0.8)
        wait(lambda: get_pool_tolerance("p6") == 0.8)

        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})
        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")
        create("table", "//tmp/t_out3")
        create("table", "//tmp/t_out4")

        op1 = map(
            track=False,
            command="sleep 1000; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out1",
            spec={"pool": "p2"},
        )

        op2 = map(
            track=False,
            command="sleep 1000; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out2",
            spec={"pool": "p6"},
        )

        wait(lambda: get_operation_tolerance(op1) == 0.6)
        wait(lambda: get_operation_tolerance(op2) == 0.8)

    @authors("asaitgalin", "ignat")
    def test_preemption_of_jobs_excessing_resource_limits(self):
        create("table", "//tmp/t_in")
        for i in xrange(3):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out")

        op = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={"data_size_per_job": 1},
        )

        wait(lambda: len(op.get_running_jobs()) == 3)

        update_op_parameters(
            op.id,
            parameters={"scheduling_options_per_pool_tree": {"default": {"resource_limits": {"user_slots": 1}}}},
        )

        wait(lambda: len(op.get_running_jobs()) == 1)

        update_op_parameters(
            op.id,
            parameters={"scheduling_options_per_pool_tree": {"default": {"resource_limits": {"user_slots": 0}}}},
        )

        wait(lambda: len(op.get_running_jobs()) == 0)

    @authors("mrkastep")
    def test_preemptor_event_log(self):
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu")
        create_pool("pool1", attributes={"min_share_resources": {"cpu": total_cpu_limit}})

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out0")
        create("table", "//tmp/t_out1")

        for i in xrange(3):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        op0 = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 1000; cat", breakpoint_name="b0"),
            in_="//tmp/t_in",
            out="//tmp/t_out0",
            spec={"pool": "pool0", "job_count": 3},
        )

        wait_breakpoint(breakpoint_name="b0", job_count=3)
        release_breakpoint(breakpoint_name="b0")

        op1 = map(
            track=False,
            command=with_breakpoint("cat; BREAKPOINT", breakpoint_name="b1"),
            in_="//tmp/t_in",
            out="//tmp/t_out1",
            spec={"pool": "pool1", "job_count": 1},
        )

        preemptor_job_id = wait_breakpoint(breakpoint_name="b1")[0]
        release_breakpoint(breakpoint_name="b1")

        def check_events():
            for row in read_table("//sys/scheduler/event_log"):
                event_type = row["event_type"]
                if event_type == "job_aborted" and row["operation_id"] == op0.id:
                    assert row["preempted_for"]["operation_id"] == op1.id
                    assert row["preempted_for"]["job_id"] == preemptor_job_id
                    return True
            return False

        wait(lambda: check_events())

        op0.abort()

    @authors("ignat")
    def test_waiting_job_timeout(self):
        # Pool tree misconfiguration
        set("//sys/pool_trees/default/@config/waiting_job_timeout", 10000)
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 5000)

        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu")
        create_pool("pool1", attributes={"min_share_resources": {"cpu": total_cpu_limit}})
        create_pool("pool2")

        command = """(trap "sleep 15; exit 0" SIGINT; BREAKPOINT)"""

        run_test_vanilla(
            with_breakpoint(command),
            spec={
                "pool": "pool2",
            },
            task_patch={
                "interruption_signal": "SIGINT",
            },
            job_count=3,
        )
        job_ids = wait_breakpoint(job_count=3)
        assert len(job_ids) == 3

        op1 = run_test_vanilla(
            "sleep 1",
            spec={
                "pool": "pool1",
            },
            job_count=1,
        )

        op1.track()

        assert op1.get_job_count("completed", from_orchid=False) == 1
        assert op1.get_job_count("aborted", from_orchid=False) == 0

    @authors("ignat")
    def test_inconsistent_waiting_job_timeout(self):
        # Pool tree misconfiguration
        set("//sys/pool_trees/default/@config/waiting_job_timeout", 5000)
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 15000)

        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu")
        create_pool("pool1", attributes={"min_share_resources": {"cpu": total_cpu_limit}})
        create_pool("pool2")

        command = """(trap "sleep 10; exit 0" SIGINT; BREAKPOINT)"""

        run_test_vanilla(
            with_breakpoint(command),
            spec={
                "pool": "pool2",
            },
            task_patch={
                "interruption_signal": "SIGINT",
            },
            job_count=3,
        )
        job_ids = wait_breakpoint(job_count=3)
        assert len(job_ids) == 3

        op1 = run_test_vanilla(
            "cat",
            spec={
                "pool": "pool1",
            },
            job_count=1,
        )

        wait(lambda: op1.get_job_count("aborted") == 1)

    @authors("eshcherbin")
    @pytest.mark.xfail(run=False, reason="Fails until YT-14804 is resolved.")
    def test_usage_overcommit_due_to_interruption(self):
        # Pool tree misconfiguration
        set("//sys/pool_trees/default/@config/waiting_job_timeout", 600000)
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 600000)

        set("//sys/scheduler/config/running_jobs_update_period", 100)

        total_cpu_limit = int(get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu"))
        create_pool("pool1", attributes={"min_share_resources": {"cpu": total_cpu_limit}})
        create_pool("pool2")

        command = """(trap "sleep 115; exit 0" SIGINT; BREAKPOINT)"""

        run_test_vanilla(
            with_breakpoint(command),
            spec={
                "pool": "pool2",
            },
            task_patch={
                "interruption_signal": "SIGINT",
            },
            job_count=total_cpu_limit,
        )
        job_ids = wait_breakpoint(job_count=total_cpu_limit)
        assert len(job_ids) == total_cpu_limit

        run_test_vanilla(
            "sleep 1",
            spec={
                "pool": "pool1",
            },
            job_count=1,
        )

        for i in range(100):
            time.sleep(0.1)
            assert get(scheduler_orchid_pool_path("<Root>") + "/resource_usage/cpu") <= total_cpu_limit

    @authors("eshcherbin")
    def test_pass_preemption_reason_to_node(self):
        update_controller_agent_config("enable_operation_progress_archivation", True)

        create_pool("research")
        create_pool("prod", attributes={"strong_guarantee_resources": {"cpu": 3}})

        op1 = run_test_vanilla(with_breakpoint("BREAKPOINT"), spec={"pool": "research"})
        job_id = wait_breakpoint(job_count=1)[0]

        op2 = run_sleeping_vanilla(spec={"pool": "prod"}, job_count=3)

        wait(lambda: op1.get_job_count(state="aborted") == 1)

        # NB(eshcherbin): Previous check doesn't guarantee that job's state in the archive is "aborted".
        def get_aborted_job(op_id, job_id):
            j = get_job(op_id, job_id, verbose_error=False)
            if j["state"] != "aborted":
                raise YtError()
            return j

        job = retry(lambda: get_aborted_job(op1.id, job_id))
        print_debug(job["error"])
        assert job["state"] == "aborted"
        assert job["abort_reason"] == "preemption"
        assert job["error"]["attributes"]["abort_reason"] == "preemption"
        preemption_reason = job["error"]["attributes"]["preemption_reason"]
        assert preemption_reason.startswith("Preempted to start job") and \
            "of operation {}".format(op2.id) in preemption_reason

    @authors("eshcherbin")
    def test_conditional_preemption(self):
        set("//sys/scheduler/config/min_spare_job_resources_on_node", {"cpu": 0.5, "user_slots": 1})

        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 1)
        set("//sys/pool_trees/default/@config/enable_conditional_preemption", False)
        wait(lambda: not get(scheduler_orchid_default_pool_tree_config_path() + "/enable_conditional_preemption"))

        create_pool("blocking_pool", attributes={"strong_guarantee_resources": {"cpu": 1}})
        create_pool("guaranteed_pool", attributes={"strong_guarantee_resources": {"cpu": 2}})

        for i in range(3):
            run_sleeping_vanilla(
                spec={"pool": "blocking_pool"},
                task_patch={"cpu_limit": 0.5},
            )
        wait(lambda: get(scheduler_orchid_pool_path("blocking_pool") + "/resource_usage/cpu") == 1.5)

        donor_op = run_sleeping_vanilla(
            job_count=4,
            spec={"pool": "guaranteed_pool"},
            task_patch={"cpu_limit": 0.5},
        )
        wait(lambda: get(scheduler_orchid_operation_path(donor_op.id) + "/resource_usage/cpu", default=0) == 1.5)
        wait(lambda: get(scheduler_orchid_operation_path(donor_op.id) + "/starvation_status", default=None) != "non_starving")
        wait(lambda: get(scheduler_orchid_pool_path("guaranteed_pool") + "/starvation_status") != "non_starving")

        time.sleep(1.5)
        assert get(scheduler_orchid_operation_path(donor_op.id) + "/starvation_status") != "non_starving"
        assert get(scheduler_orchid_pool_path("guaranteed_pool") + "/starvation_status") != "non_starving"

        starving_op = run_sleeping_vanilla(
            job_count=2,
            spec={"pool": "guaranteed_pool"},
            task_patch={"cpu_limit": 0.5},
        )

        wait(lambda: get(scheduler_orchid_operation_path(donor_op.id) + "/starvation_status", default=None) == "non_starving")
        wait(lambda: get(scheduler_orchid_operation_path(starving_op.id) + "/starvation_status", default=None) != "non_starving")
        wait(lambda: get(scheduler_orchid_pool_path("guaranteed_pool") + "/starvation_status") != "non_starving")

        time.sleep(3.0)
        assert get(scheduler_orchid_operation_path(donor_op.id) + "/starvation_status") == "non_starving"
        assert get(scheduler_orchid_operation_path(starving_op.id) + "/starvation_status", default=None) != "non_starving"
        assert get(scheduler_orchid_pool_path("guaranteed_pool") + "/starvation_status") != "non_starving"

        set("//sys/pool_trees/default/@config/enable_conditional_preemption", True)
        wait(lambda: get(scheduler_orchid_operation_path(starving_op.id) + "/resource_usage/cpu") == 0.5)
        wait(lambda: get(scheduler_orchid_operation_path(donor_op.id) + "/resource_usage/cpu") == 1.0)
        wait(lambda: get(scheduler_orchid_pool_path("blocking_pool") + "/resource_usage/cpu") == 1.5)

##################################################################


class TestSchedulingBugOfOperationWithGracefulPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {"exec_agent": {"job_controller": {"resource_limits": {"cpu": 2, "user_slots": 2}}}}

    @authors("renadeen")
    def test_scheduling_bug_of_operation_with_graceful_preemption(self):
        # Scenario:
        # 1. run operation with graceful preemption and with two jobs;
        # 2. first job is successfully scheduled;
        # 3. operation status becomes Normal since usage/fair_share is greater than fair_share_starvation_tolerance;
        # 4. next job will never be scheduled cause scheduler doesn't schedule jobs for operations with graceful preemption and Normal status.

        create_pool("pool", attributes={"fair_share_starvation_tolerance": 0.4})
        # TODO(renadeen): need this placeholder operation to work around some bugs in scheduler (YT-13840).
        run_test_vanilla(with_breakpoint("BREAKPOINT", breakpoint_name="placeholder"))

        op = run_test_vanilla(
            command=with_breakpoint("BREAKPOINT", breakpoint_name="graceful"),
            job_count=2,
            spec={
                "preemption_mode": "graceful",
                "pool": "pool",
            }
        )

        wait_breakpoint(breakpoint_name="graceful", job_count=1)
        release_breakpoint(breakpoint_name="placeholder")

        wait_breakpoint(breakpoint_name="graceful", job_count=2)
        wait(lambda: op.get_job_count("running") == 2)
        release_breakpoint(breakpoint_name="graceful")
        op.track()

##################################################################


class TestResourceLimitsOverdraftPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "allowed_node_resources_overcommit_duration": 1000,
        }
    }

    DELTA_NODE_CONFIG = {"exec_agent": {"job_controller": {"resource_limits": {"cpu": 2, "user_slots": 2}}}}

    def setup(self):
        set("//sys/pool_trees/default/@config/job_graceful_interrupt_timeout", 10000)
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 600000)

    def teardown(self):
        remove("//sys/scheduler/config", force=True)

    @authors("ignat")
    def test_scheduler_preempt_overdraft_resources(self):
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 1000)

        nodes = ls("//sys/cluster_nodes")
        assert len(nodes) > 0

        set(
            "//sys/cluster_nodes/{}/@resource_limits_overrides".format(nodes[0]),
            {"cpu": 0},
        )
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/cpu".format(nodes[0])) == 0.0)

        create("table", "//tmp/t_in")
        for i in xrange(1):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op1 = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 1000; cat"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
        )
        wait_breakpoint()

        op2 = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 1000; cat"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
        )
        wait_breakpoint()

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_usage/cpu".format(nodes[1])) == 2.0)

        # TODO(ignat): add check that jobs are not preemptable.

        set(
            "//sys/cluster_nodes/{}/@resource_limits_overrides".format(nodes[0]),
            {"cpu": 2},
        )
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/cpu".format(nodes[0])) == 2.0)

        set(
            "//sys/cluster_nodes/{}/@resource_limits_overrides".format(nodes[1]),
            {"cpu": 0},
        )
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/cpu".format(nodes[1])) == 0.0)

        wait(lambda: op1.get_job_count("aborted") == 1)
        wait(lambda: op2.get_job_count("aborted") == 1)

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)

    @authors("ignat")
    def test_scheduler_force_abort(self):
        nodes = ls("//sys/cluster_nodes")
        assert len(nodes) >= 2

        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(nodes[0]), True)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(nodes[0])) == 0
        )

        create("table", "//tmp/t_in")
        for i in xrange(1):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op1 = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
        )
        op2 = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
        )

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_usage/user_slots".format(nodes[1])) == 2
        )

        # TODO(ignat): add check that jobs are not preemptable.

        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(nodes[0]), False)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(nodes[0])) == 2
        )

        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(nodes[1]), True)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(nodes[1])) == 0
        )

        wait(lambda: op1.get_job_count("aborted") == 1)
        wait(lambda: op2.get_job_count("aborted") == 1)

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)

##################################################################


class TestSchedulerAggressivePreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 4
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"fair_share_update_period": 100}}

    def setup_method(self, method):
        super(TestSchedulerAggressivePreemption, self).setup_method(method)
        set("//sys/pool_trees/default/@config/aggressive_preemption_satisfaction_threshold", 0.2)
        set("//sys/pool_trees/default/@config/preemption_satisfaction_threshold", 1.0)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.9)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 0)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout", 100)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout_limit", 100)
        set("//sys/pool_trees/default/@config/fair_share_aggressive_starvation_timeout", 200)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 5)
        time.sleep(0.5)

    @classmethod
    def modify_node_config(cls, config):
        for resource in ["cpu", "user_slots"]:
            config["exec_agent"]["job_controller"]["resource_limits"][resource] = 2

    @authors("ignat")
    def test_aggressive_preemption(self):
        create_pool("special_pool")
        set("//sys/pools/special_pool/@enable_aggressive_starvation", True)

        def get_fair_share_ratio(op):
            return op.get_runtime_progress("scheduling_info_per_pool_tree/default/fair_share_ratio", 0.0)

        def get_usage_ratio(op):
            return op.get_runtime_progress("scheduling_info_per_pool_tree/default/usage_ratio", 0.0)

        ops = []
        for index in xrange(2):
            create("table", "//tmp/t_out" + str(index))
            op = run_sleeping_vanilla(
                job_count=4,
                spec={"pool": "fake_pool" + str(index), "locality_timeout": 0},
            )
            ops.append(op)
        time.sleep(3)

        for op in ops:
            wait(lambda: are_almost_equal(get_fair_share_ratio(op), 1.0 / 2.0))
            wait(lambda: are_almost_equal(get_usage_ratio(op), 1.0 / 2.0))
            wait(lambda: len(op.get_running_jobs()) == 4)

        op = run_sleeping_vanilla(
            job_count=1,
            spec={"pool": "special_pool", "locality_timeout": 0},
            task_patch={"cpu_limit": 2},
        )
        time.sleep(3)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op), 1.0 / 4.0))
        wait(lambda: are_almost_equal(get_usage_ratio(op), 1.0 / 4.0))
        wait(lambda: len(op.get_running_jobs()) == 1)

    @authors("eshcherbin")
    def test_no_aggressive_preemption_for_non_aggressively_starving_operation(self):
        create_pool("fake_pool", attributes={"weight": 10.0})
        create_pool("regular_pool", attributes={"weight": 1.0})
        create_pool("special_pool", attributes={"weight": 2.0})

        def get_starvation_status(op):
            return op.get_runtime_progress("scheduling_info_per_pool_tree/default/starvation_status")

        def get_fair_share_ratio(op):
            return op.get_runtime_progress("scheduling_info_per_pool_tree/default/fair_share_ratio", 0.0)

        def get_usage_ratio(op):
            return op.get_runtime_progress("scheduling_info_per_pool_tree/default/usage_ratio", 0.0)

        bad_op = run_sleeping_vanilla(
            job_count=8,
            spec={
                "pool": "fake_pool",
                "locality_timeout": 0,
                "update_preemptable_jobs_list_logging_period": 1,
            },
        )
        bad_op.wait_presence_in_scheduler()

        wait(lambda: are_almost_equal(get_fair_share_ratio(bad_op), 1.0))
        wait(lambda: are_almost_equal(get_usage_ratio(bad_op), 1.0))
        wait(lambda: len(bad_op.get_running_jobs()) == 8)

        op1 = run_sleeping_vanilla(job_count=2, spec={"pool": "special_pool", "locality_timeout": 0})
        op1.wait_presence_in_scheduler()

        wait(lambda: are_almost_equal(get_fair_share_ratio(op1), 2.0 / 12.0))
        wait(lambda: are_almost_equal(get_usage_ratio(op1), 1.0 / 8.0))
        wait(lambda: len(op1.get_running_jobs()) == 1)

        time.sleep(3)
        assert are_almost_equal(get_usage_ratio(op1), 1.0 / 8.0)
        assert len(op1.get_running_jobs()) == 1

        op2 = run_sleeping_vanilla(job_count=1, spec={"pool": "regular_pool", "locality_timeout": 0})
        op2.wait_presence_in_scheduler()

        wait(lambda: get_starvation_status(op2) == "starving")
        wait(lambda: are_almost_equal(get_fair_share_ratio(op2), 1.0 / 13.0))
        wait(lambda: are_almost_equal(get_usage_ratio(op2), 0.0))
        wait(lambda: len(op2.get_running_jobs()) == 0)

        time.sleep(3)
        assert are_almost_equal(get_usage_ratio(op2), 0.0)
        assert len(op2.get_running_jobs()) == 0

        # (1/8) / (1/6) = 0.75 < 0.9 (which is fair_share_starvation_tolerance).
        assert get_starvation_status(op1) == "starving"

        set("//sys/pools/special_pool/@enable_aggressive_starvation", True)
        wait(lambda: are_almost_equal(get_usage_ratio(op1), 2.0 / 8.0))
        wait(lambda: len(op1.get_running_jobs()) == 2)
        assert get_starvation_status(op1) == "non_starving"


# TODO(ignat): merge with class above.
class TestSchedulerAggressivePreemption2(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"fair_share_update_period": 100}}

    def setup_method(self, method):
        super(TestSchedulerAggressivePreemption2, self).setup_method(method)
        set("//sys/pool_trees/default/@config/aggressive_preemption_satisfaction_threshold", 0.2)
        set("//sys/pool_trees/default/@config/preemption_satisfaction_threshold", 0.75)
        set("//sys/pool_trees/default/@config/preemption_check_starvation", False)
        set("//sys/pool_trees/default/@config/preemption_check_satisfaction", False)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout", 100)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 2)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        time.sleep(0.5)

    @classmethod
    def modify_node_config(cls, config):
        config["exec_agent"]["job_controller"]["resource_limits"]["cpu"] = 5
        config["exec_agent"]["job_controller"]["resource_limits"]["user_slots"] = 5

    @authors("eshcherbin")
    def test_allow_aggressive_starvation_preemption_operation(self):
        get_fair_share_ratio = lambda op_id: get(
            scheduler_orchid_operation_path(op_id) + "/fair_share_ratio", default=0.0
        )
        get_usage_ratio = lambda op_id: get(scheduler_orchid_operation_path(op_id) + "/usage_ratio", default=0.0)

        create_pool("honest_pool", attributes={"min_share_resources": {"cpu": 15}})
        create_pool(
            "honest_subpool_big",
            parent_name="honest_pool",
            attributes={
                "min_share_resources": {"cpu": 10},
                "allow_aggressive_preemption": False,
            },
        )
        create_pool(
            "honest_subpool_small",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 5}},
        )
        create_pool("dishonest_pool")
        create_pool(
            "special_pool",
            attributes={
                "min_share_resources": {"cpu": 10},
                "enable_aggressive_starvation": True,
            },
        )

        op_dishonest = run_sleeping_vanilla(spec={"pool": "dishonest_pool"}, task_patch={"cpu_limit": 5}, job_count=2)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_dishonest.id), 0.4))
        wait(lambda: are_almost_equal(get_usage_ratio(op_dishonest.id), 0.4))

        op_honest_small = run_sleeping_vanilla(spec={"pool": "honest_subpool_small"}, task_patch={"cpu_limit": 5})
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_small.id), 0.2))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_small.id), 0.2))

        op_honest_big = run_sleeping_vanilla(spec={"pool": "honest_subpool_big"}, job_count=20)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.6))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.4))

        op_special = run_sleeping_vanilla(spec={"pool": "special_pool"}, job_count=10)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op_special.id), 0.4))
        time.sleep(1.0)
        wait(lambda: are_almost_equal(get_usage_ratio(op_special.id), 0.08), iter=10)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.4))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.32))

    @pytest.mark.skip("This test either is incorrect or simply doesn't work until YT-13715 is resolved")
    @authors("eshcherbin")
    def test_allow_aggressive_starvation_preemption_ancestor(self):
        get_fair_share_ratio = lambda op_id: get(
            scheduler_orchid_operation_path(op_id) + "/fair_share_ratio", default=0.0
        )
        get_usage_ratio = lambda op_id: get(scheduler_orchid_operation_path(op_id) + "/usage_ratio", default=0.0)

        create_pool(
            "honest_pool",
            attributes={
                "min_share_resources": {"cpu": 15},
                "allow_aggressive_preemption": False,
            },
        )
        create_pool(
            "honest_subpool_big",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 10}},
        )
        create_pool(
            "honest_subpool_small",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 5}},
        )
        create_pool("dishonest_pool")
        create_pool(
            "special_pool",
            attributes={
                "min_share_resources": {"cpu": 10},
                "enable_aggressive_starvation": True,
            },
        )

        op_dishonest = run_sleeping_vanilla(spec={"pool": "dishonest_pool"}, task_patch={"cpu_limit": 5}, job_count=2)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_dishonest.id), 0.4))
        wait(lambda: are_almost_equal(get_usage_ratio(op_dishonest.id), 0.4))

        op_honest_small = run_sleeping_vanilla(spec={"pool": "honest_subpool_small"}, task_patch={"cpu_limit": 5})
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_small.id), 0.2))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_small.id), 0.2))

        op_honest_big = run_sleeping_vanilla(
            spec={
                "pool": "honest_subpool_big",
                "scheduling_options_per_pool_tree": {"default": {"enable_detailed_logs": True}},
            },
            job_count=20,
        )
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.6))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.4))

        op_special = run_sleeping_vanilla(spec={"pool": "special_pool"}, job_count=10)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op_special.id), 0.4))
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.4))
        time.sleep(1.0)
        wait(lambda: are_almost_equal(get_usage_ratio(op_special.id), 0.08), iter=10)
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.32))