#!/bin/sh
#
# Make sure a Redis server is listening for the test suite, and leave an
# existing one alone.
#
# usage: ci/redis.sh
#
# Every workflow that runs the suite needs this, and so does anyone running
# it by hand, which is why it is one script and not a copy per workflow.  A
# service container would be the other way to do it, but the image tag would
# then live in each workflow file: a job's "services" are started before any
# step of that job runs, so nothing can read .github/versions.env first, and
# a version that cannot come from there does not belong in a workflow.
#
# The server it starts keeps nothing: no snapshot, no append-only file, and
# a directory under the work tree that is thrown away with it.

set -eu

PORT=${REDIS_PORT:-6379}
WORK=${CI_WORK:-$(cd "$(dirname "$0")/.." && pwd)/ci-work}/redis

alive() {
	if command -v redis-cli > /dev/null 2>&1; then
		[ "$(redis-cli -p "$PORT" ping 2>/dev/null)" = PONG ]
	else
		# No redis-cli on a minimal install.  An inline PING is the oldest
		# part of the protocol and needs nothing but a socket.
		printf 'PING\r\n' | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null |
			grep -q PONG
	fi
}

if alive; then
	echo "ci/redis.sh: a Redis server already answers on port $PORT"
	exit 0
fi

command -v redis-server > /dev/null 2>&1 || {
	echo "ci/redis.sh: no Redis server answers on port $PORT and" >&2
	echo "ci/redis.sh: redis-server is not installed" >&2
	exit 2
}

rm -rf "$WORK"
mkdir -p "$WORK"

redis-server \
	--port "$PORT" \
	--bind 127.0.0.1 \
	--daemonize yes \
	--save '' \
	--appendonly no \
	--dir "$WORK" \
	--pidfile "$WORK/redis.pid" \
	--logfile "$WORK/redis.log"

i=0
while [ "$i" -lt 50 ]; do
	if alive; then
		echo "ci/redis.sh: Redis is up on port $PORT"
		exit 0
	fi
	i=$((i + 1))
	sleep 1
done

echo "ci/redis.sh: Redis did not come up within 50 seconds" >&2
[ -r "$WORK/redis.log" ] && tail -20 "$WORK/redis.log" >&2
exit 1
