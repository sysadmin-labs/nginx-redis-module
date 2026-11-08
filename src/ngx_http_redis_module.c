
/*
 * Copyright (C) 2026 Jochen Neumeister <joneum@FreeBSD.org>
 *
 * An HTTP content handler that fetches one value from a Redis server with a
 * single GET and serves it as the response body.  The key is taken from the
 * $redis_key variable, the way the memcached module bundled with nginx takes
 * its key from $memcached_key, and this module is built along the same lines:
 * a thin client on top of nginx's own upstream machinery, not a Redis library.
 *
 * It speaks just enough of the Redis protocol (RESP) to ask for a key and to
 * read the answer.  The request is one GET as a multi-bulk, the answer is a
 * bulk string: its $<len> header gives the body length, a $-1 nil answer is a
 * 404, and anything a GET cannot produce is refused as an invalid response.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_http_upstream_conf_t   upstream;
    ngx_int_t                  index;
    ngx_uint_t                 gzip_flag;
} ngx_http_redis_loc_conf_t;


typedef struct {
    size_t                     rest;
    ngx_http_request_t        *request;
    ngx_str_t                  key;
} ngx_http_redis_ctx_t;


static ngx_int_t ngx_http_redis_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_redis_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_redis_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_redis_filter_init(void *data);
static ngx_int_t ngx_http_redis_filter(void *data, ssize_t bytes);
static void ngx_http_redis_abort_request(ngx_http_request_t *r);
static void ngx_http_redis_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);

static void *ngx_http_redis_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_redis_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

static char *ngx_http_redis_pass(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_conf_bitmask_t  ngx_http_redis_next_upstream_masks[] = {
    { ngx_string("error"), NGX_HTTP_UPSTREAM_FT_ERROR },
    { ngx_string("timeout"), NGX_HTTP_UPSTREAM_FT_TIMEOUT },
    { ngx_string("invalid_response"), NGX_HTTP_UPSTREAM_FT_INVALID_HEADER },
    { ngx_string("not_found"), NGX_HTTP_UPSTREAM_FT_HTTP_404 },
    { ngx_string("off"), NGX_HTTP_UPSTREAM_FT_OFF },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_http_redis_commands[] = {

    { ngx_string("redis_pass"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE1,
      ngx_http_redis_pass,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("redis_bind"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE12,
      ngx_http_upstream_bind_set_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.local),
      NULL },

    { ngx_string("redis_socket_keepalive"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.socket_keepalive),
      NULL },

    { ngx_string("redis_connect_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.connect_timeout),
      NULL },

    { ngx_string("redis_send_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.send_timeout),
      NULL },

    { ngx_string("redis_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.buffer_size),
      NULL },

    { ngx_string("redis_read_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.read_timeout),
      NULL },

    { ngx_string("redis_next_upstream"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_conf_set_bitmask_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.next_upstream),
      &ngx_http_redis_next_upstream_masks },

    { ngx_string("redis_next_upstream_tries"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.next_upstream_tries),
      NULL },

    { ngx_string("redis_next_upstream_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, upstream.next_upstream_timeout),
      NULL },

    { ngx_string("redis_gzip_flag"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_redis_loc_conf_t, gzip_flag),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_redis_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_redis_create_loc_conf,        /* create location configuration */
    ngx_http_redis_merge_loc_conf          /* merge location configuration */
};


ngx_module_t  ngx_http_redis_module = {
    NGX_MODULE_V1,
    &ngx_http_redis_module_ctx,            /* module context */
    ngx_http_redis_commands,               /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_str_t  ngx_http_redis_key = ngx_string("redis_key");


/*
 * The trailer after a bulk string's payload is a bare CRLF, where the
 * memcached protocol has "\r\nEND\r\n".  The filter below is otherwise the
 * same shape: it copies content_length_n bytes of body and then checks that
 * exactly these two bytes follow.
 */
#define NGX_HTTP_REDIS_END   (sizeof(ngx_http_redis_end) - 1)
static u_char  ngx_http_redis_end[] = CRLF;


static ngx_int_t
ngx_http_redis_handler(ngx_http_request_t *r)
{
    ngx_int_t                   rc;
    ngx_http_upstream_t        *u;
    ngx_http_redis_ctx_t       *ctx;
    ngx_http_redis_loc_conf_t  *rlcf;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    if (ngx_http_set_content_type(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u = r->upstream;

    ngx_str_set(&u->schema, "redis://");
    u->output.tag = (ngx_buf_tag_t) &ngx_http_redis_module;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_redis_module);

    u->conf = &rlcf->upstream;

    u->create_request = ngx_http_redis_create_request;
    u->reinit_request = ngx_http_redis_reinit_request;
    u->process_header = ngx_http_redis_process_header;
    u->abort_request = ngx_http_redis_abort_request;
    u->finalize_request = ngx_http_redis_finalize_request;

    ctx = ngx_palloc(r->pool, sizeof(ngx_http_redis_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->request = r;

    ngx_http_set_ctx(r, ctx, ngx_http_redis_module);

    u->input_filter_init = ngx_http_redis_filter_init;
    u->input_filter = ngx_http_redis_filter;
    u->input_filter_ctx = ctx;

    r->main->count++;

    ngx_http_upstream_init(r);

    return NGX_DONE;
}


static ngx_int_t
ngx_http_redis_create_request(ngx_http_request_t *r)
{
    size_t                      len;
    ngx_buf_t                  *b;
    ngx_chain_t                *cl;
    ngx_http_redis_ctx_t       *ctx;
    ngx_http_variable_value_t  *vv;
    ngx_http_redis_loc_conf_t  *rlcf;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_redis_module);

    vv = ngx_http_get_indexed_variable(r, rlcf->index);

    if (vv == NULL || vv->not_found || vv->len == 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the \"$redis_key\" variable is not set");
        return NGX_ERROR;
    }

    /*
     * GET as a Redis multi-bulk request:
     *
     *     *2\r\n $3\r\n GET\r\n $<keylen>\r\n <key>\r\n
     *
     * A bulk string is length-prefixed, so the key goes out as its own bytes
     * with no escaping: a CRLF, a NUL or a space inside it is just data, the
     * way it is inside the value coming back.
     */

    len = sizeof("*2" CRLF "$3" CRLF "GET" CRLF "$") - 1
          + NGX_SIZE_T_LEN + sizeof(CRLF) - 1
          + vv->len + sizeof(CRLF) - 1;

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    r->upstream->request_bufs = cl;

    b->last = ngx_cpymem(b->last, "*2" CRLF "$3" CRLF "GET" CRLF "$",
                         sizeof("*2" CRLF "$3" CRLF "GET" CRLF "$") - 1);
    b->last = ngx_sprintf(b->last, "%uz" CRLF, vv->len);

    ctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);

    ctx->key.data = b->last;
    b->last = ngx_copy(b->last, vv->data, vv->len);
    ctx->key.len = b->last - ctx->key.data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http redis request: \"%V\"", &ctx->key);

    *b->last++ = CR; *b->last++ = LF;

    return NGX_OK;
}


static ngx_int_t
ngx_http_redis_reinit_request(ngx_http_request_t *r)
{
    return NGX_OK;
}


static ngx_int_t
ngx_http_redis_process_header(ngx_http_request_t *r)
{
    u_char                     *p, *len_start, *lf;
    ngx_str_t                   line;
    ngx_table_elt_t            *h;
    ngx_http_upstream_t        *u;
    ngx_http_redis_ctx_t       *ctx;
    ngx_http_redis_loc_conf_t  *rlcf;

    u = r->upstream;

    for (p = u->buffer.pos; p < u->buffer.last; p++) {
        if (*p == LF) {
            goto found;
        }
    }

    return NGX_AGAIN;

found:

    lf = p;

    line.data = u->buffer.pos;
    line.len = lf - u->buffer.pos;

    if (line.len == 0 || *(lf - 1) != CR) {
        goto no_valid;
    }

    line.len--;   /* drop the CR, so the log shows the reply without it */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "redis: \"%V\"", &line);

    ctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);
    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_redis_module);

    /* line.data[0] is the RESP type byte, the rest up to the CR is its text */

    len_start = line.data + 1;

    switch (line.data[0]) {

    case '$':

        if (line.len == sizeof("$-1") - 1
            && len_start[0] == '-' && len_start[1] == '1')
        {
            /* a nil bulk reply: the key is not there */

            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "key: \"%V\" was not found by redis", &ctx->key);

            u->headers_in.content_length_n = 0;
            u->headers_in.status_n = 404;
            u->state->status = 404;
            u->buffer.pos = lf + 1;
            u->keepalive = 1;

            return NGX_OK;
        }

        u->headers_in.content_length_n = ngx_atoof(len_start,
                                                   line.len - 1);
        if (u->headers_in.content_length_n == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "redis sent invalid length in response \"%V\" "
                          "for key \"%V\"", &line, &ctx->key);
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        if (rlcf->gzip_flag) {
            h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) {
                return NGX_ERROR;
            }

            h->hash = 1;
            h->next = NULL;
            ngx_str_set(&h->key, "Content-Encoding");
            ngx_str_set(&h->value, "gzip");
            r->headers_out.content_encoding = h;
        }

        u->headers_in.status_n = 200;
        u->state->status = 200;
        u->buffer.pos = lf + 1;

        return NGX_OK;

    case '-':

        /*
         * An error reply.  A GET earns one when the key holds something a
         * GET cannot read, so it is the upstream's answer and not nginx's
         * to paper over: pass it on as an invalid response.
         */

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "redis sent error reply \"%V\" for key \"%V\"",
                      &line, &ctx->key);

        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

no_valid:

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "redis sent invalid response: \"%V\"", &line);

    return NGX_HTTP_UPSTREAM_INVALID_HEADER;
}


static ngx_int_t
ngx_http_redis_filter_init(void *data)
{
    ngx_http_redis_ctx_t  *ctx = data;

    ngx_http_upstream_t  *u;

    u = ctx->request->upstream;

    if (u->headers_in.status_n != 404) {
        u->length = u->headers_in.content_length_n + NGX_HTTP_REDIS_END;
        ctx->rest = NGX_HTTP_REDIS_END;

    } else {
        u->length = 0;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_redis_filter(void *data, ssize_t bytes)
{
    ngx_http_redis_ctx_t  *ctx = data;

    u_char               *last;
    ngx_buf_t            *b;
    ngx_chain_t          *cl, **ll;
    ngx_http_upstream_t  *u;

    u = ctx->request->upstream;
    b = &u->buffer;

    if (u->length == (ssize_t) ctx->rest) {

        if (bytes > u->length
            || ngx_strncmp(b->last,
                   ngx_http_redis_end + NGX_HTTP_REDIS_END - ctx->rest,
                   bytes)
               != 0)
        {
            ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                          "redis sent invalid trailer");

            u->length = 0;
            ctx->rest = 0;

            return NGX_OK;
        }

        u->length -= bytes;
        ctx->rest -= bytes;

        if (u->length == 0) {
            u->keepalive = 1;
        }

        return NGX_OK;
    }

    for (cl = u->out_bufs, ll = &u->out_bufs; cl; cl = cl->next) {
        ll = &cl->next;
    }

    cl = ngx_chain_get_free_buf(ctx->request->pool, &u->free_bufs);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf->flush = 1;
    cl->buf->memory = 1;

    *ll = cl;

    last = b->last;
    cl->buf->pos = last;
    b->last += bytes;
    cl->buf->last = b->last;
    cl->buf->tag = u->output.tag;

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "redis filter bytes:%z size:%z length:%O rest:%z",
                   bytes, b->last - b->pos, u->length, ctx->rest);

    if (bytes <= (ssize_t) (u->length - NGX_HTTP_REDIS_END)) {
        u->length -= bytes;
        return NGX_OK;
    }

    last += (size_t) (u->length - NGX_HTTP_REDIS_END);

    if (bytes > u->length
        || ngx_strncmp(last, ngx_http_redis_end, b->last - last) != 0)
    {
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                      "redis sent invalid trailer");

        b->last = last;
        cl->buf->last = last;
        u->length = 0;
        ctx->rest = 0;

        return NGX_OK;
    }

    ctx->rest -= b->last - last;
    b->last = last;
    cl->buf->last = last;
    u->length = ctx->rest;

    if (u->length == 0) {
        u->keepalive = 1;
    }

    return NGX_OK;
}


static void
ngx_http_redis_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort http redis request");
    return;
}


static void
ngx_http_redis_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http redis request");
    return;
}


static void *
ngx_http_redis_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_redis_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_redis_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->upstream.bufs.num = 0;
     *     conf->upstream.next_upstream = 0;
     *     conf->upstream.temp_path = NULL;
     */

    conf->upstream.local = NGX_CONF_UNSET_PTR;
    conf->upstream.socket_keepalive = NGX_CONF_UNSET;
    conf->upstream.next_upstream_tries = NGX_CONF_UNSET_UINT;
    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.next_upstream_timeout = NGX_CONF_UNSET_MSEC;

    conf->upstream.buffer_size = NGX_CONF_UNSET_SIZE;

    /* the hardcoded values */
    conf->upstream.cyclic_temp_file = 0;
    conf->upstream.buffering = 0;
    conf->upstream.ignore_client_abort = 0;
    conf->upstream.send_lowat = 0;
    conf->upstream.bufs.num = 0;
    conf->upstream.busy_buffers_size = 0;
    conf->upstream.max_temp_file_size = 0;
    conf->upstream.temp_file_write_size = 0;
    conf->upstream.intercept_errors = 1;
    conf->upstream.intercept_404 = 1;
    conf->upstream.pass_request_headers = 0;
    conf->upstream.pass_request_body = 0;
    conf->upstream.force_ranges = 1;

    conf->index = NGX_CONF_UNSET;
    conf->gzip_flag = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_redis_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_redis_loc_conf_t *prev = parent;
    ngx_http_redis_loc_conf_t *conf = child;

    ngx_conf_merge_ptr_value(conf->upstream.local,
                              prev->upstream.local, NULL);

    ngx_conf_merge_value(conf->upstream.socket_keepalive,
                              prev->upstream.socket_keepalive, 0);

    ngx_conf_merge_uint_value(conf->upstream.next_upstream_tries,
                              prev->upstream.next_upstream_tries, 0);

    ngx_conf_merge_msec_value(conf->upstream.connect_timeout,
                              prev->upstream.connect_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.send_timeout,
                              prev->upstream.send_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.read_timeout,
                              prev->upstream.read_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.next_upstream_timeout,
                              prev->upstream.next_upstream_timeout, 0);

    ngx_conf_merge_size_value(conf->upstream.buffer_size,
                              prev->upstream.buffer_size,
                              (size_t) ngx_pagesize);

    ngx_conf_merge_bitmask_value(conf->upstream.next_upstream,
                              prev->upstream.next_upstream,
                              (NGX_CONF_BITMASK_SET
                               |NGX_HTTP_UPSTREAM_FT_ERROR
                               |NGX_HTTP_UPSTREAM_FT_TIMEOUT));

    if (conf->upstream.next_upstream & NGX_HTTP_UPSTREAM_FT_OFF) {
        conf->upstream.next_upstream = NGX_CONF_BITMASK_SET
                                       |NGX_HTTP_UPSTREAM_FT_OFF;
    }

    if (conf->upstream.upstream == NULL) {
        conf->upstream.upstream = prev->upstream.upstream;
    }

    if (conf->index == NGX_CONF_UNSET) {
        conf->index = prev->index;
    }

    ngx_conf_merge_uint_value(conf->gzip_flag, prev->gzip_flag, 0);

    return NGX_CONF_OK;
}


static char *
ngx_http_redis_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_redis_loc_conf_t *rlcf = conf;

    ngx_str_t                 *value;
    ngx_url_t                  u;
    ngx_http_core_loc_conf_t  *clcf;

    if (rlcf->upstream.upstream) {
        return "is duplicate";
    }

    value = cf->args->elts;

    ngx_memzero(&u, sizeof(ngx_url_t));

    u.url = value[1];
    u.no_resolve = 1;

    rlcf->upstream.upstream = ngx_http_upstream_add(cf, &u, 0);
    if (rlcf->upstream.upstream == NULL) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_redis_handler;

    if (clcf->name.len && clcf->name.data[clcf->name.len - 1] == '/') {
        clcf->auto_redirect = 1;
    }

    rlcf->index = ngx_http_get_variable_index(cf, &ngx_http_redis_key);

    if (rlcf->index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
