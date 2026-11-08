#!/bin/sh
#
# Put the keys t/ reads into Redis and nothing else.
#
# usage: ci/seed.sh
#
# The module only reads -- it has no SET -- so the suite cannot lay down its
# own fixtures the way the redis2 module's suite does from inside a location.
# This writes them once, here, from the shell, so every block that follows
# asks for a key whose value is known exactly.
#
# The module speaks a bare GET and no SELECT, so it reads the connection's
# current database, which is 0.  That is where these keys go.  A flush first,
# so the suite neither disturbs nor is disturbed by anything else -- but it
# is database 0 of whatever server this points at, so point it at a scratch
# Redis.

set -eu

PORT=${REDIS_PORT:-6379}
DB=${REDIS_DB:-0}

command -v redis-cli > /dev/null 2>&1 || {
	echo "ci/seed.sh: redis-cli is not installed" >&2
	exit 2
}

cli() {
	redis-cli -p "$PORT" -n "$DB" "$@"
}

cli flushdb > /dev/null

cli set foo bar > /dev/null
cli set greeting "hello world" > /dev/null
cli set empty "" > /dev/null

# a value that is binary: a CRLF in the middle of it, which is exactly what
# the length-prefixed protocol is there to carry and a line-based one could
# not.  -x reads the value from stdin, bytes and all.
printf 'a\r\nb' | cli -x set binary > /dev/null

# a value larger than one page, so the suite can set redis_buffer_size below
# it and make the value stream through the filter in pieces.
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "a" }' | cli -x set big > /dev/null

echo "ci/seed.sh: seeded foo, greeting, empty, binary, big in database $DB"
