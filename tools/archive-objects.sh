#!/usr/bin/env sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: archive-objects.sh AR ARCHIVE OBJECT_LIST" >&2
    exit 2
fi

ar=$1
archive=$2
object_list=$3
chunk_size=32
chunk_count=0
set --

rm -f "$archive"
while IFS= read -r object; do
    [ -n "$object" ] || continue
    set -- "$@" "$object"
    chunk_count=$((chunk_count + 1))
    if [ "$chunk_count" -eq "$chunk_size" ]; then
        "$ar" rcs "$archive" "$@"
        set --
        chunk_count=0
    fi
done < "$object_list"

if [ "$chunk_count" -ne 0 ]; then
    "$ar" rcs "$archive" "$@"
fi
