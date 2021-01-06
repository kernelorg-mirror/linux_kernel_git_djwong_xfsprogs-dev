#!/bin/sh -f
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

status=0
DB_OPTS=""
DASH_O_DB_OPTS=""
REPAIR_OPTS=""
USAGE="Usage: xfs_admin [-efjlpuV] [-c 0|1] [-L label] [-U uuid] [-O v5_feature] device [logdev]"

while getopts "efjlpuc:L:O:U:V" c
do
	case $c in
	c)	REPAIR_OPTS=$REPAIR_OPTS" -c lazycount="$OPTARG;;
	e)	DB_OPTS=$DB_OPTS" -c 'version extflg'";;
	f)	DB_OPTS=$DB_OPTS" -f";;
	j)	DB_OPTS=$DB_OPTS" -c 'version log2'";;
	l)	DB_OPTS=$DB_OPTS" -r -c label";;
	L)	DB_OPTS=$DB_OPTS" -c 'label "$OPTARG"'";;
	p)	DB_OPTS=$DB_OPTS" -c 'version projid32bit'";;
	O)
		if [ -n "$DASH_O_DB_OPTS" ]; then
			echo "-O can only be specified once." 1>&2
			exit 1
		fi
		DASH_O_DB_OPTS=" -c 'version "$OPTARG"'"
		;;
	u)	DB_OPTS=$DB_OPTS" -r -c uuid";;
	U)	DB_OPTS=$DB_OPTS" -c 'uuid "$OPTARG"'";;
	V)	xfs_db -p xfs_admin -V
		status=$?
		exit $status
		;;
	\?)	echo $USAGE 1>&2
		exit 2
		;;
	esac
done
if [ -n "$DASH_O_DB_OPTS" ]; then
	if [ -n "$DB_OPTS" ]; then
		echo "-O can only be used by itself." 1>&2
		exit 1
	fi
	DB_OPTS="$DASH_O_DB_OPTS"
fi
set -- extra $@
shift $OPTIND
case $# in
	1|2)
		# Pick up the log device, if present
		if [ -n "$2" ]; then
			DB_OPTS=$DB_OPTS" -l '$2'"
			test -n "$REPAIR_OPTS" && \
				REPAIR_OPTS=$REPAIR_OPTS" -l '$2'"
		fi

		if [ -n "$DB_OPTS" ]
		then
			eval xfs_db -x -p xfs_admin $DB_OPTS "$1"
			status=$?
		fi
		if [ $status -eq 1 ]; then
			echo "Conversion failed due to filesystem errors; run xfs_repair."
		elif xfs_db -c 'version' "$1" | grep -q NEEDSREPAIR; then
			# Upgrade required us to run repair, so force
			# xfs_repair to run by adding a single space to
			# REPAIR_OPTS.
			echo "Running xfs_repair to complete the upgrade."
			REPAIR_OPTS="$REPAIR_OPTS "
		fi
		if [ -n "$REPAIR_OPTS" ]
		then
			# Hide normal repair output which is sent to stderr
			# assuming the filesystem is fine when a user is
			# running xfs_admin.
			# Ideally, we need to improve the output behaviour
			# of repair for this purpose (say a "quiet" mode).
			eval xfs_repair $REPAIR_OPTS "$1" 2> /dev/null
			status=`expr $? + $status`
			if [ $status -ne 0 ]
			then
				echo "Conversion failed, is the filesystem unmounted?"
			fi
		fi
		;;
	*)	echo $USAGE 1>&2
		exit 2
		;;
esac
exit $status
