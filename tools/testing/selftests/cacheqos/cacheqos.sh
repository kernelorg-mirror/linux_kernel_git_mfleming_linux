#!/bin/bash

sysfs_dir="/sys/fs/cgroup/cacheqos/"
NR_RMIDS=`dmesg | grep max_rmid | sed -e 's/.*max_rmid=//'`
NR_RMIDS=$(($NR_RMIDS - 1))

#
# test1:
# Ensure that we can create more tasks than we have RMIDs if not all the
# tasks are running at once.
#
# Essentially, fork 2 * NR_RMID tasks and put the first NR_RMID tasks
# to sleep. NR_RMID+1..2 * NR_RMID should run.
#
test1() {
    for i in $(seq 0 1); do

        # RMID 0 is reserved for *all* tasks
        for j in $(seq 1 $NR_RMIDS); do
            mkdir t$i-$j
        done
    done

    # Create permanently stopped tasks
    for j in $(seq 1 $NR_RMIDS); do
        sleep 10&
        kill -s SIGSTOP $!
        echo $! > t0-$j/tasks
        echo 1 > t0-$j/cacheqos.monitor_cache
    done

    # Create periodically running tasks
    for j in $(seq 1 $NR_RMIDS); do
        ( trap "exit 0" SIGCONT ; while :; do sleep 1; done ) &
        echo $! > t1-$j/tasks

        echo 1 > t1-$j/cacheqos.monitor_cache 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "[rmid $j] Failed to enable monitoring" >&2
        fi
    done

    for i in $(seq 0 1); do
        for j in $(seq 1 $NR_RMIDS); do
	    for t in `cat t$i-$j/tasks`; do
                if [ ! -z "$t" ]; then
                    kill -s SIGCONT $t
                fi
            done
        done
    done

    # Wait for every child process to catch SIGCONT and exit
    wait
    rmdir t0-* t1-*
}

#
# test2:
# Test that our parent/child hierarchy behaves the way that cgroup
# expects.
#
test2() {
    mkdir -p child1
    echo 1 > child1/cacheqos.monitor_cache

    # On creation child2 inherits child1 rmid
    mkdir -p child1/child2

    # Allocate new rmid
    echo 1 > child1/cacheqos.monitor_cache

    rmdir child1/child2
    rmdir child1
}

#
# test3:
# Ensure that we can actually read non-zero occupancy data. This isn't
# guaranteed to work because it is *possible* for the cache to not
# contain any of our data. We can arrange things so that we're pretty
# confident however.
#
test3() {
    mkdir group

    ( trap "exit 0" SIGCONT ; while :; do : ; done) &

    echo $! > group/tasks
    echo 1 > group/cacheqos.monitor_cache

    for i in $(seq 0 9); do
        val=`cat group/cacheqos.occupancy`
        if [ $val -gt 0 ]; then
            break
        fi
    done

    kill -s SIGCONT $!
    wait $!


    if [ $val -eq 0 ]; then
        echo "Failed to read occupancy data" >&2
    fi

    rmdir group
}

#
# test4:
# Stress test our recycling algorithm.
#
# We try our damndest to ensure there's stale data in the cache when we
# come to reuse an rmid. We exercise the pathological case to test the
# limits of our recycling algorithm.
#
test4() {
    mkdir
}

cd $sysfs_dir

test1
test3
