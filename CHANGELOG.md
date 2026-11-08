# Changelog

Newest first.  Dates are release dates.

## v0.1.0 (2026-11-08)

First release.

### Added

- `ngx_http_redis_module`, an HTTP content handler that reads one key from
  Redis with a single `GET` and serves the value as the response body.  It
  is modelled on the memcached module bundled with nginx -- the upstream
  handler shape, the `$redis_key` variable in place of `$memcached_key`, the
  header/filter split -- and freshly written: no line of that module is
  copied.

  - The request is a Redis multi-bulk `GET`.  The key is sent as a bulk
    string, length-prefixed, so a CRLF, a NUL or a space inside it is data
    and needs no escaping.
  - A bulk-string reply's `$<len>` header gives the body length; the value
    streams through nginx's upstream filter, so a value larger than
    `redis_buffer_size` is passed on in pieces rather than held whole.
  - A nil reply (`$-1`) is a `404`, with `intercept_404` on, so it reaches
    `error_page` the way a missing memcached key does.
  - An error reply (`-...`), which a `GET` earns only when the key holds
    something a `GET` cannot read, is passed on as an invalid response
    rather than papered over.

- Directives `redis_pass`, `redis_bind`, `redis_connect_timeout`,
  `redis_send_timeout`, `redis_read_timeout`, `redis_buffer_size`,
  `redis_next_upstream`, `redis_next_upstream_tries`,
  `redis_next_upstream_timeout`, `redis_socket_keepalive` and
  `redis_gzip_flag`, and the `$redis_key` variable.

- The scaffolding the other modules in this organization carry: a version
  and digest pin for every build input in `.github/versions.env`, a build
  and test matrix across the nginx releases the FreeBSD ports ship, a
  FreeBSD virtual machine, A/UBSan, valgrind, CodeQL, shellcheck and
  actionlint, and eight reloads in a row with a probe that asks the module
  rather than the server.

- A test suite that needs nothing but nginx, this module and a Redis server.
  Every block selects database 13 and flushes it first, so what comes back
  is fully determined and the suite can neither disturb nor be disturbed by
  anything else in that server.
