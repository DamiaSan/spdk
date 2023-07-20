#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 SUSE LLC.
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

function test_shallow_copy_compare() {
	# Create lvs
	bs_malloc_name=$(rpc_cmd bdev_malloc_create 20 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$bs_malloc_name" lvs_test)

	# Create lvol with 4 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * 4))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	# Fill second and fourth cluster of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1 seek=1
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1 seek=3
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill first and third cluster of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1 seek=2
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Set lvol as read only to perform the copy
	rpc_cmd bdev_lvol_set_read_only "$lvol_uuid"

	# Create external bdev to make a shallow copy of lvol on
	ext_malloc_name=$(rpc_cmd bdev_malloc_create "$lvol_size" $MALLOC_BS)

	# Make a shallow copy of lvol over external bdev
	rpc_cmd bdev_lvol_shallow_copy "$lvol_uuid" "$ext_malloc_name"

	# Create nbd devices of lvol and external bdev for comparison
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$ext_malloc_name" /dev/nbd1

	# Compare lvol and external bdev in first and third cluster
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd0 /dev/nbd1
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd0 /dev/nbd1 "$((LVS_DEFAULT_CLUSTER_SIZE * 2))" "$((LVS_DEFAULT_CLUSTER_SIZE * 2))"

	# Check that second and fourth cluster of external bdev are zero filled
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd1 /dev/zero "$LVS_DEFAULT_CLUSTER_SIZE"
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd1 /dev/zero "$((LVS_DEFAULT_CLUSTER_SIZE * 3))"

	# Stop nbd devices
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Clean up
	rpc_cmd bdev_malloc_delete "$ext_malloc_name"
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$bs_malloc_name"
	check_leftover_devices
}

function test_lvol_set_local_parent() {
	local vol_size_mb=20
	local fill_size_mb=$((vol_size_mb / 2))
	local fill_size=$((fill_size_mb * 1024 * 1024))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create a bdev that will be the external snapshot
	# State:
	#    esnap1
	esnap_uuid=2abddd12-c08d-40ad-bccf-ab131586ee4c
	rpc_cmd bdev_malloc_create -b esnap1 -u "$esnap_uuid" "$vol_size_mb" $MALLOC_BS

	# Perform write operation to the external snapshot
	# Change first half of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$esnap_uuid" /dev/nbd1
	run_fio_test /dev/nbd1 $fill_size $fill_size "write" "0xbb"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	sleep 1

	# Create a temp volume: lvol2_temp
	# New state:
	#    esnap1
	#    lvol2_temp
	lvol2_temp_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol2_temp "$vol_size_mb")

	# Copy esnap1 over lvol2_temp
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$esnap_uuid" /dev/nbd1
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_temp_uuid" /dev/nbd2
	blocks_count=$((vol_size_mb * 1024 * 1024 / MALLOC_BS))
	dd if=/dev/nbd1 of=/dev/nbd2 bs=$MALLOC_BS count=$blocks_count
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1

	# Make a snapshot of lvol2_temp: snap2
	# New state:
	#    esnap1
	#    snap2  <-- lvol2_temp
	snap2_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol2_temp_uuid" snap2)

	# Create an esnap clone: lvol2
	# New state:
	#    esnap1 <-- lvol2
	#    snap2  <-- lvol2_temp
	lvol2_uuid=$(rpc_cmd bdev_lvol_clone_bdev "$esnap_uuid" lvs_test lvol2)

	# Perform write operation to lvol2
	# Change second half of its space: the write over third cluster perform COW reading from esnap1
	# Calculate md5sum of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $fill_size "write" "0xaa"
	md5_1=$(md5sum /dev/nbd2)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Change parent of lvol2
	# New state:
	#    esnap1
	#    snap2  <-- lvol2
	#          `<-- lvol2_temp
	rpc_cmd bdev_lvol_set_local_parent "$lvol2_uuid" "$snap2_uuid"

	# Delete lvol2_temp
	# New state:
	#    esnap1
	#    snap2  <-- lvol2
	rpc_cmd bdev_lvol_delete "$lvol2_temp_uuid"

	# Calculate again md5 of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	md5_2=$(md5sum /dev/nbd2)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that md5_1 and md5_2 are equal
	[[ $md5_1 == "$md5_2" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol2_uuid"
	rpc_cmd bdev_lvol_delete "$snap2_uuid"
	rpc_cmd bdev_malloc_delete esnap1
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_shallow_copy_compare" test_shallow_copy_compare
run_test "test_lvol_set_local_parent" test_lvol_set_local_parent

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
