#!/usr/bin/env bash
set -e

if [ $# -lt 2 ]; then
	echo "usage: $0 <pid> <script>"
	echo ""
	echo "Environment variable BPF_OUTFILE can be set to save results to a file"
	echo "rather than print to stdout."
	exit 1
fi
SCRIPTS_DIR=$(readlink -f $(dirname $0))
BIN_PATH=$(readlink -f /proc/$1/exe)
BPF_SCRIPT=$($SCRIPTS_DIR/bpf/gen_enums.sh)
BPF_SCRIPT+=$(sed "s#__EXE__#${BIN_PATH}#g" "${@:2}" | sed "s#__PID__#${1}#g")
if [ -n "$ECHO_SCRIPT" ]; then
	echo "$BPF_SCRIPT"
fi

bpftrace -p $1 -e "$BPF_SCRIPT" ${BPF_OUTFILE:+-o "$BPF_OUTFILE"}
