/*
 * Copyright (c) 2012 Roman Arutyunyan
 */

#include "ngx_rtmp_amf.h"
#include "ngx_rtmp.h"
#include <string.h>

static inline void*
ngx_rtmp_amf_reverse_copy(void *dst, void* src, size_t len)
{
    size_t  k;

    if (dst == NULL || src == NULL) {
        return NULL;
    }

    for(k = 0; k < len; ++k) {
        ((u_char*)dst)[k] = ((u_char*)src)[len - 1 - k];
    }

    return dst;
}

#define NGX_RTMP_AMF_DEBUG_SIZE 16

#ifdef NGX_DEBUG
static void
ngx_rtmp_amf_debug(const char* op, ngx_log_t *log, u_char *p, size_t n)
{
    u_char          hstr[3 * NGX_RTMP_AMF_DEBUG_SIZE + 1];
    u_char          str[NGX_RTMP_AMF_DEBUG_SIZE + 1];
    u_char         *hp, *sp;
    static u_char   hex[] = "0123456789ABCDEF";
    size_t          i;

    hp = hstr;
    sp = str;

    for(i = 0; i < n && i < NGX_RTMP_AMF_DEBUG_SIZE; ++i) {
        *hp++ = ' ';
        if (p) {
            *hp++ = hex[(*p & 0xf0) >> 4];
            *hp++ = hex[*p & 0x0f];
            *sp++ = (*p >= 0x20 && *p <= 0x7e) ?
                *p : (u_char)'?';
            ++p;
        } else {
            *hp++ = 'X';
            *hp++ = 'X';
            *sp++ = '?';
        }
    }
    *hp = *sp = '\0';

    ngx_log_debug4(NGX_LOG_DEBUG_RTMP, log, 0,
            "AMF %s (%d)%s '%s'", op, n, hstr, str);
}
#endif

static ngx_int_t
ngx_rtmp_amf_get(ngx_rtmp_amf_ctx_t *ctx, void *p, size_t n)
{
    ngx_buf_t      *b;
    size_t          size;
    ngx_chain_t    *l;
#ifdef NGX_DEBUG
    void           *op = p;
    size_t          on = n;
#endif

    if (!n)
        return NGX_OK;

    for(l = ctx->link; l; l = l->next) {

        b = l->buf;

        if (b->last >= n + b->pos) {
            if (p) {
                p = ngx_cpymem(p, b->pos, n);
            }
            b->pos += n;

            ctx->link = l;
            
#ifdef NGX_DEBUG
            ngx_rtmp_amf_debug("read", ctx->log, (u_char*)op, on);
#endif

            return NGX_OK;
        }

        size = b->last - b->pos;

        if (p) {
            p = ngx_cpymem(p, b->pos, size);
        }

        n -= size;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, ctx->log, 0,
            "AMF read eof (%d)", n);

    return NGX_DONE;
}


static ngx_int_t
ngx_rtmp_amf_put(ngx_rtmp_amf_ctx_t *ctx, void *p, size_t n)
{
    ngx_buf_t       *b;
    size_t          size;
    ngx_chain_t    *l, *ln;

#ifdef NGX_DEBUG
    ngx_rtmp_amf_debug("write", ctx->log, (u_char*)p, n);
#endif

    l = ctx->link;

    while(n) {
        b = l ? l->buf : NULL;

        if (b == NULL || b->last == b->end) {

            ln = ctx->alloc(ctx->cscf);
            if (ln == NULL) {
                return NGX_ERROR;
            }

            if (l == NULL) {
                l = ln;
                ctx->first = l;
            } else {
                l->next = ln;
                l = ln;
            }

            ctx->link = l;
            b = l->buf;
        }

        size = b->end - b->last;

        if (size >= n) {
            b->last = ngx_cpymem(b->last, p, n);
            return NGX_OK;
        }

        b->last = ngx_cpymem(b->last, p, size);
        p = (u_char*)p + size;
        n -= size;
    }

    return NGX_OK;
}


static ngx_int_t 
ngx_rtmp_amf_read_object(ngx_rtmp_amf_ctx_t *ctx, ngx_rtmp_amf_elt_t *elts, 
        size_t nelts)
{
    uint8_t                 type;
    uint16_t                len;
    size_t                  n, namelen, maxlen;
    ngx_int_t               rc;
    u_char                  buf[2];

    maxlen = 0;
    for(n = 0; n < nelts; ++n) {
        namelen = elts[n].name.len;
        if (namelen > maxlen)
            maxlen = namelen;
    }

    for(;;) {

        char    name[maxlen];

        /* read key */
        if (ngx_rtmp_amf_get(ctx, buf, 2) != NGX_OK)
            return NGX_ERROR;

        ngx_rtmp_amf_reverse_copy(&len, buf, 2);

        if (!len)
            break;

        if (len <= maxlen) {
            rc = ngx_rtmp_amf_get(ctx, name, len);

        } else {
            rc = ngx_rtmp_amf_get(ctx, name, maxlen);
            if (rc != NGX_OK)
                return NGX_ERROR;
            rc = ngx_rtmp_amf_get(ctx, 0, len - maxlen);
        }

        if (rc != NGX_OK)
            return NGX_ERROR;

        /* TODO: if we require array to be sorted on name
         * then we could be able to use binary search */
        for(n = 0; n < nelts 
                && (len != elts[n].name.len
                    || ngx_strncmp(name, elts[n].name.data, len));
                ++n);

        if (ngx_rtmp_amf_read(ctx, n < nelts ? &elts[n] : NULL, 1) != NGX_OK)
            return NGX_ERROR;
    }

    if (ngx_rtmp_amf_get(ctx, &type, 1) != NGX_OK
        || type != NGX_RTMP_AMF_END)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t 
ngx_rtmp_amf_read_array(ngx_rtmp_amf_ctx_t *ctx, ngx_rtmp_amf_elt_t *elts, 
        size_t nelts)
{
    uint32_t                len;
    size_t                  n;
    u_char                  buf[4];

    /* read length */
    if (ngx_rtmp_amf_get(ctx, buf, 4) != NGX_OK)
        return NGX_ERROR;

    ngx_rtmp_amf_reverse_copy(&len, buf, 4);

    for (n = 0; n < len; ++n) {
        if (ngx_rtmp_amf_read(ctx, n < nelts ? &elts[n] : NULL, 1) != NGX_OK)
            return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t 
ngx_rtmp_amf_read(ngx_rtmp_amf_ctx_t *ctx, ngx_rtmp_amf_elt_t *elts, 
        size_t nelts)
{
    void                   *data;
    uint8_t                 type;
    size_t                  n;
    uint16_t                len;
    ngx_int_t               rc;
    u_char                  buf[8];

    for(n = 0; n < nelts; ++n) {

        switch (ngx_rtmp_amf_get(ctx, &type, sizeof(type))) {
            case NGX_DONE:
                if (elts->type & NGX_RTMP_AMF_OPTIONAL) {
                    return NGX_OK;
                }
            case NGX_ERROR:
                return NGX_ERROR;
        }

        data = (n >= nelts || elts == NULL 
                || (elts->type & ~NGX_RTMP_AMF_OPTIONAL) != type)
                ? NULL
                : elts->data;

        switch (type) {
            case NGX_RTMP_AMF_NUMBER:
                if (ngx_rtmp_amf_get(ctx, buf, 8) != NGX_OK) {
                    return NGX_ERROR;
                }
                ngx_rtmp_amf_reverse_copy(data, buf, 8);
                break;

            case NGX_RTMP_AMF_BOOLEAN:
                if (ngx_rtmp_amf_get(ctx, data, 1) != NGX_OK) {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_STRING:
                if (ngx_rtmp_amf_get(ctx, buf, 2) != NGX_OK) {
                    return NGX_ERROR;
                }
                ngx_rtmp_amf_reverse_copy(&len, buf, 2);

                if (data == NULL) {
                    rc = ngx_rtmp_amf_get(ctx, data, len);

                } else if (elts->len <= len) {
                    rc = ngx_rtmp_amf_get(ctx, data, elts->len - 1);
                    if (rc != NGX_OK)
                        return NGX_ERROR;
                    ((char*)data)[elts->len - 1] = 0;
                    rc = ngx_rtmp_amf_get(ctx, NULL, len - elts->len + 1);

                } else {
                    rc = ngx_rtmp_amf_get(ctx, data, len);
                    ((char*)data)[len] = 0;
                }

                if (rc != NGX_OK) {
                    return NGX_ERROR;
                }

                break;

            case NGX_RTMP_AMF_NULL:
                break;

            case NGX_RTMP_AMF_OBJECT:
                if (ngx_rtmp_amf_read_object(ctx, data, 
                        elts ? elts->len / sizeof(ngx_rtmp_amf_elt_t) : 0
                    ) != NGX_OK) 
                {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_ARRAY:
                if (ngx_rtmp_amf_read_array(ctx, data, 
                        elts ? elts->len / sizeof(ngx_rtmp_amf_elt_t) : 0
                    ) != NGX_OK) 
                {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_END:
                return NGX_OK;

            default:
                return NGX_ERROR;
        }

        if (elts) {
            ++elts;
        }
    }

    return NGX_OK;
}


static ngx_int_t 
ngx_rtmp_amf_write_object(ngx_rtmp_amf_ctx_t *ctx,
        ngx_rtmp_amf_elt_t *elts, size_t nelts)
{
    uint16_t                len;
    size_t                  n;
    u_char                  buf[2];

    for(n = 0; n < nelts; ++n) {

        len = elts[n].name.len;

        if (ngx_rtmp_amf_put(ctx, 
                    ngx_rtmp_amf_reverse_copy(buf, 
                        &len, 2), 2) != NGX_OK) 
        {
            return NGX_ERROR;
        }

        if (ngx_rtmp_amf_put(ctx, elts[n].name.data, len) != NGX_OK) {
            return NGX_ERROR;
        }

        if (ngx_rtmp_amf_write(ctx, &elts[n], 1) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (ngx_rtmp_amf_put(ctx, "\0\0", 2) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t 
ngx_rtmp_amf_write_array(ngx_rtmp_amf_ctx_t *ctx,
        ngx_rtmp_amf_elt_t *elts, size_t nelts)
{
    uint32_t                len;
    size_t                  n;
    u_char                  buf[4];

    len = nelts;
    if (ngx_rtmp_amf_put(ctx, 
                ngx_rtmp_amf_reverse_copy(buf, 
                    &len, 4), 4) != NGX_OK) 
    {
        return NGX_ERROR;
    }

    for(n = 0; n < nelts; ++n) {
        if (ngx_rtmp_amf_write(ctx, &elts[n], 1) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


ngx_int_t 
ngx_rtmp_amf_write(ngx_rtmp_amf_ctx_t *ctx,
        ngx_rtmp_amf_elt_t *elts, size_t nelts)
{
    size_t                  n;
    uint8_t                 type;
    void                   *data;
    uint16_t                len;
    u_char                  buf[8];

    for(n = 0; n < nelts; ++n) {

        type = elts[n].type;
        data = elts[n].data;
        len  = elts[n].len;

        if (ngx_rtmp_amf_put(ctx, &type, sizeof(type)) != NGX_OK)
            return NGX_ERROR;

        switch(type) {
            case NGX_RTMP_AMF_NUMBER:
                if (ngx_rtmp_amf_put(ctx, 
                            ngx_rtmp_amf_reverse_copy(buf, 
                                data, 8), 8) != NGX_OK) 
                {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_BOOLEAN:
                if (ngx_rtmp_amf_put(ctx, data, 1) != NGX_OK) {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_STRING:
                if (len == 0 && data) {
                    len = ngx_strlen((u_char*)data);
                }

                if (ngx_rtmp_amf_put(ctx, 
                            ngx_rtmp_amf_reverse_copy(buf, 
                                &len, 2), 2) != NGX_OK) 
                {
                    return NGX_ERROR;
                }

                if (ngx_rtmp_amf_put(ctx, data, len) != NGX_OK) {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_NULL:
                break;

            case NGX_RTMP_AMF_OBJECT:
                type = NGX_RTMP_AMF_END;
                if (ngx_rtmp_amf_write_object(ctx, data,
                        elts[n].len / sizeof(ngx_rtmp_amf_elt_t)) != NGX_OK
                    || ngx_rtmp_amf_put(ctx, &type, 
                        sizeof(type)) != NGX_OK)
                {
                    return NGX_ERROR;
                }
                break;

            case NGX_RTMP_AMF_ARRAY:
                if (ngx_rtmp_amf_write_array(ctx, data, 
                        elts[n].len / sizeof(ngx_rtmp_amf_elt_t)) != NGX_OK)
                {
                    return NGX_ERROR;
                }
                break;

            default:
                return NGX_ERROR;
        }
    }

    return NGX_OK;
}

