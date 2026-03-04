#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
LOG_PATH="$rootdir/test/lvol/spdk.log"

# try to examine lvs on top of bdev failing all I/O
function test_examine_lvs_on_failing_bdev() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	error_name="EE_$malloc_name"
	pt_name="PT_$error_name"

	rpc_cmd bdev_error_create "$malloc_name"
	rpc_cmd "bdev_wait_for_examine"
	# error bdev fails to examine with -EILSEQ because it's created with no errors
	grep -q "Lvol store not found on $error_name: -84" $LOG_PATH

	# enable errors on the error bdev
	rpc_cmd bdev_error_inject_error "$error_name" 'all' 'failure' -n 1000
	# create passthru bdev so we can inspect the examine import error
	rpc_cmd bdev_passthru_create -b "$error_name" -p "$pt_name"
	rpc_cmd "bdev_wait_for_examine"
	# This one should fail with -EIO as we should not try to validate the blob signature
	grep -q "Lvol store not found on $pt_name: -5" $LOG_PATH

	# clean up
	rpc_cmd bdev_lvol_get_lvstores -l lvs && false
	rpc_cmd bdev_passthru_delete "$pt_name"
	rpc_cmd bdev_error_delete "$error_name"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	rpc_cmd bdev_get_bdevs -b "$malloc_name" && false
	check_leftover_devices
}

# try to create lvs on top of bdev failing all I/O
# upon failure, blobstore should clean up and bdev delete should not get stuck
function test_construct_lvs_on_failing_bdev() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	error_name="EE_$malloc_name"

	rpc_cmd bdev_error_create "$malloc_name"

	# enable errors on the error bdev
	rpc_cmd bdev_error_inject_error "$error_name" 'all' 'failure' -n 1000

	rpc_cmd bdev_lvol_create_lvstore "$error_name" lvs && false

	# clean up
	rpc_cmd bdev_lvol_get_lvstores -l lvs && false
	rpc_cmd bdev_error_delete "$error_name"
	rpc_cmd bdev_get_bdevs -b "$error_name" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	rpc_cmd bdev_get_bdevs -b "$malloc_name" && false
	check_leftover_devices
}

# try to delete lvs on top of bdev failing all I/O
# upon failure, blobstore and bdev delete should not get stuck
function test_delete_lvs_on_failing_bdev() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	error_name="EE_$malloc_name"

	rpc_cmd bdev_error_create "$malloc_name"
	rpc_cmd bdev_lvol_create_lvstore "$error_name" lvs

	# enable errors on the error bdev
	rpc_cmd bdev_error_inject_error "$error_name" 'all' 'failure' -n 1000

	rpc_cmd bdev_lvol_delete_lvstore -l lvs

	# clean up
	rpc_cmd bdev_lvol_get_lvstores -l lvs && false
	rpc_cmd bdev_error_delete "$error_name"
	rpc_cmd bdev_get_bdevs -b "$error_name" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	rpc_cmd bdev_get_bdevs -b "$malloc_name" && false
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt -Lvbdev_lvol &> $LOG_PATH &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm "$LOG_PATH"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_examine_lvs_on_failing_bdev" test_examine_lvs_on_failing_bdev
run_test "test_construct_lvs_on_failing_bdev" test_construct_lvs_on_failing_bdev
run_test "test_delete_lvs_on_failing_bdev" test_delete_lvs_on_failing_bdev

trap - SIGINT SIGTERM EXIT
if ps -p $spdk_pid; then
	killprocess $spdk_pid
fi
rm "$LOG_PATH" | :
