#!/bin/sh
#
# apt-get with a bound and two more tries.
#
# A mirror that accepted the connection and then stopped answering once
# held six jobs of one run in the build environment until GitHub killed
# them at its own six hour ceiling, 360 and 361 minutes for the two that
# were measured.  The run produced no verdict at all.
#
# A step inside a composite action cannot carry timeout-minutes, so the
# bound has to live here.  Worst case this gives up after
# 3 * 180 + 2 * 5 = 550 seconds, which stays inside the 15 minutes every
# job now carries.  It lives in its own file so that there is one
# implementation and not a second copy in the lint workflow -- and so that
# the lint gate sees it, which it cannot do for a run block in a workflow.
#
# usage: ci/apt-get.sh update -qq
#        ci/apt-get.sh install -y --no-install-recommends foo bar

set -eu

[ "$#" -gt 0 ] || { echo "usage: $0 <apt-get arguments>" >&2; exit 2; }

attempt=1

while :; do
	# DEBIAN_FRONTEND through env(1) rather than sudo -E: whether sudo is
	# allowed to preserve the environment depends on the runner's sudoers,
	# whereas env(1) is just a command that sets it.
	if timeout 180 sudo env DEBIAN_FRONTEND=noninteractive \
			apt-get -o DPkg::Lock::Timeout=60 "$@"; then
		exit 0
	fi

	if [ "$attempt" -ge 3 ]; then
		echo "apt-get $1 failed three times, giving up" >&2
		exit 1
	fi

	echo "apt-get $1 failed, retrying ($attempt of 3)" >&2
	attempt=$((attempt + 1))
	sleep 5
done
