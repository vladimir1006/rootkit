#!/bin/sh

set -eu

program=${1:-./rk2600}
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' 0 HUP INT TERM

make_name()
{
    awk -v count="$1" 'BEGIN { for (i = 0; i < count; i++) printf "x" }'
}

# Valid names pass CLI validation, then fail at device open (exit 1)
for length in 1 254 255; do
    name=$(make_name "$length")
    status=0
    "$program" hide "$name" \
        >"$temporary_directory/stdout" 2>"$temporary_directory/stderr" || status=$?
    if [ "$status" -ne 1 ]; then
        printf 'FAIL: %s-byte name should pass CLI validation (exit 1), got %s\n' \
            "$length" "$status" >&2
        exit 1
    fi
done

# Names exceeding RK2600_NAME_MAX are rejected by CLI validation (exit 2)
for length in 256 257; do
    name=$(make_name "$length")
    status=0
    "$program" hide "$name" \
        >"$temporary_directory/stdout" 2>"$temporary_directory/stderr" || status=$?
    if [ "$status" -ne 2 ]; then
        printf 'FAIL: %s-byte name should be rejected (exit 2), got %s\n' \
            "$length" "$status" >&2
        exit 1
    fi
done

printf 'All generated name-boundary checks passed.\n'
