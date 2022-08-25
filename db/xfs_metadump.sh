#!/bin/sh -f
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2007 Silicon Graphics, Inc.  All Rights Reserved.
#

OPTS=" "
DBOPTS=" "
USAGE="Usage: xfs_metadump [-aefFgoVwx] [-m max_extents] [-l logdev] [-R rtdev] source target"

while getopts "aefgl:m:owFVxR:" c
do
	case $c in
	a)	OPTS=$OPTS"-a ";;
	e)	OPTS=$OPTS"-e ";;
	g)	OPTS=$OPTS"-g ";;
	m)	OPTS=$OPTS"-m "$OPTARG" ";;
	o)	OPTS=$OPTS"-o ";;
	w)	OPTS=$OPTS"-w ";;
	f)	DBOPTS=$DBOPTS" -f";;
	l)	DBOPTS=$DBOPTS" -l "$OPTARG" ";;
	F)	DBOPTS=$DBOPTS" -F";;
	V)	xfs_db -p xfs_metadump -V
		status=$?
		exit $status
		;;
	x)	OPTS=$OPTS"-x ";;
	R)	DBOPTS=$DBOPTS"-R "$OPTARG" ";;
	\?)	echo $USAGE 1>&2
		exit 2
		;;
	esac
done
set -- extra $@
shift $OPTIND
case $# in
	2)	xfs_db$DBOPTS -i -p xfs_metadump -c "metadump$OPTS $2" $1
		status=$?
		;;
	*)	echo $USAGE 1>&2
		exit 2
		;;
esac
exit $status
