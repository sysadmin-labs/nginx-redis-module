# nginx-redis-module

[![Build & Test][build-test-badge]][build-test-link]
[![FreeBSD][freebsd-badge]][freebsd-link]
[![A/UBSan][sanitizers-badge]][sanitizers-link]
[![Valgrind][valgrind-badge]][valgrind-link]
[![Reload][reload-badge]][reload-link]
[![CodeQL][codeql-badge]][codeql-link]
[![Lint][lint-badge]][lint-link]

[build-test-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/build-test.yml/badge.svg
[build-test-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/build-test.yml
[freebsd-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/freebsd.yml/badge.svg
[freebsd-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/freebsd.yml
[sanitizers-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/sanitizers.yml/badge.svg
[sanitizers-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/sanitizers.yml
[valgrind-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/valgrind.yml/badge.svg
[valgrind-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/valgrind.yml
[reload-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/reload.yml/badge.svg
[reload-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/reload.yml
[codeql-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/codeql.yml/badge.svg
[codeql-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/codeql.yml
[lint-badge]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/lint.yml/badge.svg
[lint-link]: https://github.com/sysadmin-labs/nginx-redis-module/actions/workflows/lint.yml

Read one value from Redis and serve it as the response body.

This is the Redis counterpart of the `memcached` module that ships with
nginx, and it is built the same way: a location names a Redis server with
`redis_pass`, the key comes from the `$redis_key` variable, and the module
does a single `GET`.  The reply's bulk-string header gives the body length,
a nil reply (`$-1`) is a `404`, and anything a `GET` cannot return is refused
as an invalid response.

It is a **content handler**, not a Redis client: it reads, it does not write,
it runs no other command, and it does not interpret the value -- the bytes
stored under the key are the bytes sent to the client.  For talking to Redis
in full, pipelines and all commands, that is a different module
([nginx-redis2-module](https://github.com/sysadmin-labs/nginx-redis2-module));
this one is the small, cache-shaped read.

## Example

```nginx
location / {
    set              $redis_key $uri;
    redis_pass       127.0.0.1:6379;
    redis_gzip_flag  0;

    error_page       404 502 504 = @fallback;
}

location @fallback {
    proxy_pass http://backend;
}
```

A request for `/page` asks Redis for the key `/page`.  If it is there, its
value is the response.  If it is not, the `404` sends the request on to the
backend -- the usual read-through cache in front of a slower origin.

## Directives

### redis_pass

**syntax:** `redis_pass <host>:<port>` or `redis_pass <upstream>`

**context:** location, if in location

The Redis server, directly or through an `upstream` block.  The address may
contain variables, in which case a `resolver` is needed.

### redis_bind

**syntax:** `redis_bind <address> [transparent]` or `redis_bind off`

**context:** http, server, location

The local address outgoing connections are bound to.

### redis_connect_timeout, redis_send_timeout, redis_read_timeout

**default:** `60s`

**context:** http, server, location

### redis_buffer_size

**syntax:** `redis_buffer_size <size>`

**default:** one page

**context:** http, server, location

The buffer the reply header and the first bytes of the value are read into.
A value larger than this is streamed on in pieces, so this is not a limit on
the value size.

### redis_next_upstream

**syntax:** `redis_next_upstream error | timeout | invalid_response | not_found | off ...`

**default:** `error timeout`

**context:** http, server, location

When to give the next server in the `upstream` block a turn.

### redis_next_upstream_tries, redis_next_upstream_timeout

**default:** `0` (no limit)

**context:** http, server, location

### redis_gzip_flag

**syntax:** `redis_gzip_flag <number>`

**default:** `0`

**context:** http, server, location

A plain `GET` returns only the value, with none of the per-key flags
memcached carries, so there is nothing in the reply to read a gzip bit out
of.  This directive is the blunt stand-in: set to a non-zero number, every
value this location serves is marked `Content-Encoding: gzip`.  Use it where
a location is known to hold gzip-compressed blobs; leave it `0` otherwise.

### $redis_key

The key to fetch.  Set it the way `memcached_key` is set for the memcached
module; without it the request is an error.

```nginx
location = /photo {
    set        $redis_key "photo:main";
    redis_pass 127.0.0.1:6379;
}
```

## Building

Into the binary:

```sh
./configure --add-module=/path/to/nginx-redis-module
```

or as a loadable module:

```sh
./configure --with-compat --add-dynamic-module=/path/to/nginx-redis-module
```

```nginx
load_module modules/ngx_http_redis_module.so;
```

On FreeBSD the module is packaged as `www/nginx-module-redis`, and
`www/nginx`, `www/nginx-devel` and `www/freenginx` build it from there when
their `HTTP_REDIS` option is on.

## Testing

The suite needs a Redis server on `127.0.0.1:6379`.  It works in database 13
and deletes its own keys, so it will not touch anything else -- but point it
at a scratch Redis anyway.

```sh
ci/build.sh 1.30.5 /tmp/nginx-redis
ci/redis.sh
TEST_NGINX_BINARY=/tmp/nginx-redis/sbin/nginx prove -r t/
```

`ci/redis.sh` starts a Redis that keeps nothing -- no snapshot, no
append-only file -- and leaves an already running one alone.

## License

BSD 2-Clause, see [LICENSE](LICENSE).  The module is freshly written and
modelled on nginx's own memcached module; no line of that module is copied
here.
