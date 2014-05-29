#!/bin/bash

sysfs_dir="/sys/fs/cgroup/cacheqos/"
NR_RMIDS=`dmesg | grep max_rmid | sed -e 's/.*max_rmid=//'`

test1_setup() {
    for i in $(seq 0 1); do

        # RMID 0 is reserved for *all* tasks
        for j in $(seq 1 $NR_RMIDS); do
            mkdir t$i-$j
        done
    done

}

test1_cleanup() {
    for i in $(seq 0 1); do
        for j in $(seq 1 $NR_RMIDS); do
	    for t in `cat t$i-$j/tasks`; do
                kill -s SIGCONT $t
                wait $t
            done
        done
    done

    # Wait for every child process to catch SIGCONT and exit
    wait
    rmdir t0-* t1-*
}

#
# test1:
# Ensure that we can create more tasks than we have RMIDs if not all the
# tasks are running at once.
#
# Essentially, fork 2 * NR_RMID tasks and put the first NR_RMID tasks
# to sleep. NR_RMID+1..2 * NR_RMID should run.
#
test1() {
    test1_setup

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
        echo 1 > t1-$j/cacheqos.monitor_cache
    done

    test1_cleanup
}

cd $sysfs_dir

test1
