# vi:ft=perl
#
# The module reads one key with a GET and serves the value as the body.  It
# speaks no SELECT, so it reads database 0, which is where ci/seed.sh writes
# the keys these blocks ask for:
#
#     foo       -> bar
#     greeting  -> hello world
#     empty     -> (empty string)
#     binary    -> a\r\nb
#     big       -> 100000 'a'
#
# Run ci/seed.sh before this suite.

use Test::Nginx::Socket 'no_plan';

repeat_each(2);

run_tests();

__DATA__

=== TEST 1: a key that is there is the response body
--- config
    location = /t {
        set        $redis_key foo;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_body chomp
bar



=== TEST 2: a key that is not there is a 404
--- config
    location = /t {
        set        $redis_key definitely-not-here;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 404



=== TEST 3: an empty value is a 200 with an empty body
--- config
    location = /t {
        set        $redis_key empty;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_body chomp



=== TEST 4: a value with a key that carries spaces
--- config
    location = /t {
        set        $redis_key greeting;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_body chomp
hello world



=== TEST 5: a binary value, CRLF inside it included
--- config
    location = /t {
        set        $redis_key binary;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_body eval
"a\r\nb"



=== TEST 6: a value larger than the buffer streams through whole
--- config
    location = /t {
        set               $redis_key big;
        redis_buffer_size 1k;
        redis_pass        127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_body eval
"a" x 100000



=== TEST 7: $redis_key taken from the request
--- config
    location /r/ {
        set        $redis_key $arg_k;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /r/x?k=foo
--- error_code: 200
--- response_body chomp
bar



=== TEST 8: a HEAD reaches the same place, with no body
--- config
    location = /t {
        set        $redis_key foo;
        redis_pass 127.0.0.1:6379;
    }
--- request
HEAD /t
--- error_code: 200
--- response_body



=== TEST 9: redis_pass through an upstream block
--- http_config
    upstream redisbackend {
        server 127.0.0.1:6379;
    }
--- config
    location = /t {
        set        $redis_key foo;
        redis_pass redisbackend;
    }
--- request
GET /t
--- error_code: 200
--- response_body chomp
bar



=== TEST 10: a keepalive pool survives being used more than once
--- http_config
    upstream redisbackend {
        server 127.0.0.1:6379;
        keepalive 1;
    }
--- config
    location = /t {
        set        $redis_key foo;
        redis_pass redisbackend;
    }
--- pipelined_requests eval
["GET /t", "GET /t", "GET /t"]
--- response_body eval
["bar", "bar", "bar"]



=== TEST 11: redis_gzip_flag marks the value gzip-encoded
--- config
    location = /t {
        set             $redis_key foo;
        redis_gzip_flag 1;
        redis_pass      127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_headers
Content-Encoding: gzip
--- response_body chomp
bar



=== TEST 12: without the flag there is no Content-Encoding
--- config
    location = /t {
        set        $redis_key foo;
        redis_pass 127.0.0.1:6379;
    }
--- request
GET /t
--- error_code: 200
--- response_headers
Content-Encoding:
--- response_body chomp
bar



=== TEST 13: a POST is not allowed
--- config
    location = /t {
        set        $redis_key foo;
        redis_pass 127.0.0.1:6379;
    }
--- request
POST /t
a body
--- error_code: 405
