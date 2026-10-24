#!/bin/sh

set -eu

program=${1:-./companion}
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM

fail()
{
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

expect_exit()
{
    expected_status=$1
    description=$2
    shift 2

    actual_status=0
    "$program" "$@" >"$temporary_directory/stdout" \
        2>"$temporary_directory/stderr" || actual_status=$?
    if [ "$actual_status" -ne "$expected_status" ]; then
        fail "$description (expected exit $expected_status, got $actual_status)"
    fi
}

expect_exit 0 'help succeeds' --help
grep -q 'Usage:' "$temporary_directory/stdout" || fail 'help prints usage'

expect_exit 2 'missing command is rejected'
expect_exit 2 'unknown command is rejected' unknown
expect_exit 2 'status rejects extra arguments' status extra
expect_exit 2 'hide requires a name' hide
expect_exit 2 'hide rejects an empty name' hide ''
expect_exit 2 'hide rejects a path' hide ../secret

missing_device="$temporary_directory/no-device"
expect_exit 1 'missing device is reported' --device "$missing_device" status
grep -q 'Cannot open' "$temporary_directory/stderr" || \
    fail 'missing-device error is printed'

printf 'All companion CLI tests passed.\n'
