
/*
 * Copyright (C) Dmitry Volyntsev
 * Copyright (C) NGINX, Inc.
 */

#include <njs_core.h>
#include <njs_json.h>
#include <njs_date.h>
#include <njs_regexp.h>
#include <string.h>


typedef struct {
    njs_vm_t                   *vm;
    nxt_mp_t                   *pool;
    nxt_uint_t                 depth;
    const u_char               *start;
    const u_char               *end;
} njs_json_parse_ctx_t;


typedef struct {
    njs_value_t                value;

    uint8_t                    written;       /* 1 bit */

    enum {
       NJS_JSON_OBJECT_START,
       NJS_JSON_OBJECT_CONTINUE,
       NJS_JSON_OBJECT_TO_JSON_REPLACED,
       NJS_JSON_OBJECT_REPLACED,
       NJS_JSON_ARRAY_START,
       NJS_JSON_ARRAY_CONTINUE,
       NJS_JSON_ARRAY_TO_JSON_REPLACED,
       NJS_JSON_ARRAY_REPLACED
    }                          type:8;

    uint32_t                   index;
    njs_array_t                *keys;
    njs_value_t                *prop_value;
} njs_json_state_t;


typedef struct {
    union {
        njs_continuation_t     cont;
        u_char                 padding[NJS_CONTINUATION_SIZE];
    } u;
    /*
     * This retval value must be aligned so the continuation is padded
     * to aligned size.
     */
    njs_value_t                retval;

    nxt_array_t                stack;
    njs_json_state_t           *state;
    njs_function_t             *function;
} njs_json_parse_t;


typedef struct njs_chb_node_s njs_chb_node_t;

struct njs_chb_node_s {
    njs_chb_node_t             *next;
    u_char                     *start;
    u_char                     *pos;
    u_char                     *end;
};


typedef struct {
    union {
        njs_continuation_t     cont;
        u_char                 padding[NJS_CONTINUATION_SIZE];
    } u;
    /*
     * This retval value must be aligned so the continuation is padded
     * to aligned size.
     */
    njs_value_t                retval;
    njs_value_t                key;

    njs_vm_t                   *vm;
    nxt_mp_t                   *pool;
    njs_chb_node_t             *nodes;
    njs_chb_node_t             *last;
    nxt_array_t                stack;
    njs_json_state_t           *state;

    njs_value_t                replacer;
    nxt_str_t                  space;
} njs_json_stringify_t;


static const u_char *njs_json_parse_value(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_object(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_array(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_string(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_number(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
nxt_inline uint32_t njs_json_unicode(const u_char *p);
static const u_char *njs_json_skip_space(const u_char *start,
    const u_char *end);

static njs_ret_t njs_json_parse_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t unused, njs_index_t unused2);
static njs_ret_t njs_json_parse_continuation_apply(njs_vm_t *vm,
    njs_json_parse_t *parse);
static njs_json_state_t *njs_json_push_parse_state(njs_vm_t *vm,
    njs_json_parse_t *parse, njs_value_t *value);
static njs_json_state_t *njs_json_pop_parse_state(njs_json_parse_t *parse);
static void njs_json_parse_exception(njs_json_parse_ctx_t *ctx,
    const char *msg, const u_char *pos);

static njs_ret_t njs_json_stringify_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t unused, njs_index_t unused2);
static njs_function_t *njs_object_to_json_function(njs_vm_t *vm,
    njs_value_t *value);
static njs_ret_t njs_json_stringify_to_json(njs_vm_t *vm,
    njs_json_stringify_t* stringify, njs_function_t *function,
    njs_value_t *key, njs_value_t *value);
static njs_ret_t njs_json_stringify_replacer(njs_vm_t *vm,
    njs_json_stringify_t* stringify, njs_value_t *key, njs_value_t *value);
static njs_ret_t njs_json_stringify_array(njs_vm_t *vm,
    njs_json_stringify_t *stringify);
static njs_json_state_t *njs_json_push_stringify_state(njs_vm_t *vm,
    njs_json_stringify_t *stringify, const njs_value_t *value);
static njs_json_state_t *njs_json_pop_stringify_state(
    njs_json_stringify_t *stringify);

static nxt_int_t njs_json_append_value(njs_json_stringify_t *stringify,
    const njs_value_t *value);
static nxt_int_t njs_json_append_string(njs_json_stringify_t *stringify,
    const njs_value_t *value, char quote);
static nxt_int_t njs_json_append_number(njs_json_stringify_t *stringify,
    const njs_value_t *value);

static njs_value_t *njs_json_wrap_value(njs_vm_t *vm, const njs_value_t *value);


#define NJS_JSON_BUF_MIN_SIZE       128

#define njs_json_buf_written(stringify, bytes)                              \
    (stringify)->last->pos += (bytes);

#define njs_json_buf_node_size(n) (size_t) ((n)->pos - (n)->start)
#define njs_json_buf_node_room(n) (size_t) ((n)->end - (n)->pos)

static nxt_int_t njs_json_buf_append(njs_json_stringify_t *stringify,
    const char *msg, size_t len);
static u_char *njs_json_buf_reserve(njs_json_stringify_t *stringify,
    size_t size);
static nxt_int_t njs_json_buf_pullup(njs_json_stringify_t *stringify,
    nxt_str_t *str);


static const njs_object_prop_t  njs_json_object_properties[];


static njs_ret_t
njs_json_parse(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    njs_value_t           *value, *wrapper;
    const njs_value_t     *text, *reviver;
    const u_char          *p, *end;
    njs_json_parse_t      *parse;
    njs_string_prop_t     string;
    njs_json_parse_ctx_t  ctx;

    value = nxt_mp_alloc(vm->mem_pool, sizeof(njs_value_t));
    if (nxt_slow_path(value == NULL)) {
        njs_memory_error(vm);
        return NXT_ERROR;
    }

    text = njs_arg(args, nargs, 1);

    if (njs_is_undefined(text)) {
        text = &njs_string_undefined;
    }

    (void) njs_string_prop(&string, text);

    p = string.start;
    end = p + string.size;

    ctx.vm = vm;
    ctx.pool = vm->mem_pool;
    ctx.depth = 32;
    ctx.start = string.start;
    ctx.end = end;

    p = njs_json_skip_space(p, end);
    if (nxt_slow_path(p == end)) {
        njs_json_parse_exception(&ctx, "Unexpected end of input", p);
        return NXT_ERROR;
    }

    p = njs_json_parse_value(&ctx, value, p);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    p = njs_json_skip_space(p, end);
    if (nxt_slow_path(p != end)) {
        njs_json_parse_exception(&ctx, "Unexpected token", p);
        return NXT_ERROR;
    }

    reviver = njs_arg(args, nargs, 2);

    if (njs_is_function(reviver) && njs_is_object(value)) {
        wrapper = njs_json_wrap_value(vm, value);
        if (nxt_slow_path(wrapper == NULL)) {
            goto memory_error;
        }

        parse = njs_vm_continuation(vm);
        parse->u.cont.function = njs_json_parse_continuation;
        parse->function = reviver->data.u.function;

        if (nxt_array_init(&parse->stack, NULL, 4, sizeof(njs_json_state_t),
                           &njs_array_mem_proto, vm->mem_pool)
            == NULL)
        {
            goto memory_error;
        }

        if (njs_json_push_parse_state(vm, parse, wrapper) == NULL) {
            goto memory_error;
        }

        return njs_json_parse_continuation(vm, args, nargs, unused);
    }

    vm->retval = *value;

    return NXT_OK;

memory_error:

    njs_memory_error(vm);

    return NXT_ERROR;
}


njs_ret_t
njs_vm_json_parse(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs)
{
    njs_function_t  *parse;

    parse = njs_json_object_properties[0].value.data.u.function;

    return njs_vm_call(vm, parse, args, nargs);
}


static njs_ret_t
njs_json_stringify(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    double                num;
    nxt_int_t             i;
    njs_ret_t             ret;
    njs_value_t           *wrapper;
    const njs_value_t     *replacer, *space;
    njs_json_stringify_t  *stringify;

    stringify = njs_vm_continuation(vm);
    stringify->vm = vm;
    stringify->pool = vm->mem_pool;
    stringify->u.cont.function = njs_json_stringify_continuation;
    stringify->nodes = NULL;
    stringify->last = NULL;

    replacer = njs_arg(args, nargs, 2);

    if (njs_is_function(replacer) || njs_is_array(replacer)) {
        stringify->replacer = *replacer;
        if (njs_is_array(replacer)) {
            ret = njs_json_stringify_array(vm, stringify);
            if (nxt_slow_path(ret != NXT_OK)) {
                goto memory_error;
            }
        }

    } else {
        stringify->replacer = njs_value_undefined;
    }

    stringify->space.length = 0;

    space = njs_arg(args, nargs, 3);

    if (njs_is_string(space) || njs_is_number(space)) {
        if (njs_is_string(space)) {
            njs_string_get(space, &stringify->space);
            stringify->space.length = nxt_min(stringify->space.length, 10);

        } else {
            num = space->data.u.number;
            if (!isnan(num) && !isinf(num) && num > 0) {
                num = nxt_min(num, 10);

                stringify->space.length = (size_t) num;
                stringify->space.start = nxt_mp_alloc(vm->mem_pool,
                                                      (size_t) num + 1);
                if (nxt_slow_path(stringify->space.start == NULL)) {
                    goto memory_error;
                }

                for (i = 0; i < (int) num; i++) {
                    stringify->space.start[i] = ' ';
                }
            }
        }
    }

    if (nxt_array_init(&stringify->stack, NULL, 4, sizeof(njs_json_state_t),
                       &njs_array_mem_proto, vm->mem_pool)
        == NULL)
    {
        goto memory_error;
    }

    wrapper = njs_json_wrap_value(vm, njs_arg(args, nargs, 1));
    if (nxt_slow_path(wrapper == NULL)) {
        goto memory_error;
    }

    if (njs_json_push_stringify_state(vm, stringify, wrapper) == NULL) {
        goto memory_error;
    }

    return njs_json_stringify_continuation(vm, args, nargs, unused);

memory_error:

    njs_memory_error(vm);

    return NXT_ERROR;
}


njs_ret_t
njs_vm_json_stringify(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs)
{
    njs_function_t  *stringify;

    stringify = njs_json_object_properties[1].value.data.u.function;

    return njs_vm_call(vm, stringify, args, nargs);
}


static const u_char *
njs_json_parse_value(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    switch (*p) {
    case '{':
        return njs_json_parse_object(ctx, value, p);

    case '[':
        return njs_json_parse_array(ctx, value, p);

    case '"':
        return njs_json_parse_string(ctx, value, p);

    case 't':
        if (nxt_fast_path(ctx->end - p >= 4 && memcmp(p, "true", 4) == 0)) {
            *value = njs_value_true;

            return p + 4;
        }

        goto error;

    case 'f':
        if (nxt_fast_path(ctx->end - p >= 5 && memcmp(p, "false", 5) == 0)) {
            *value = njs_value_false;

            return p + 5;
        }

        goto error;

    case 'n':
        if (nxt_fast_path(ctx->end - p >= 4 && memcmp(p, "null", 4) == 0)) {
            *value = njs_value_null;

            return p + 4;
        }

        goto error;
    }

    if (nxt_fast_path(*p == '-' || (*p - '0') <= 9)) {
        return njs_json_parse_number(ctx, value, p);
    }

error:

    njs_json_parse_exception(ctx, "Unexpected token", p);

    return NULL;
}


static const u_char *
njs_json_parse_object(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    nxt_int_t           ret;
    njs_object_t        *object;
    njs_value_t         *prop_name, *prop_value;
    njs_object_prop_t   *prop;
    nxt_lvlhsh_query_t  lhq;

    if (nxt_slow_path(--ctx->depth == 0)) {
        njs_json_parse_exception(ctx, "Nested too deep", p);
        return NULL;
    }

    object = njs_object_alloc(ctx->vm);
    if (nxt_slow_path(object == NULL)) {
        goto memory_error;
    }

    prop = NULL;

    for ( ;; ) {
        p = njs_json_skip_space(p + 1, ctx->end);
        if (nxt_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p != '"') {
            if (nxt_fast_path(*p == '}')) {
                if (nxt_slow_path(prop != NULL)) {
                    njs_json_parse_exception(ctx, "Trailing comma", p - 1);
                    return NULL;
                }

                break;
            }

            goto error_token;
        }

        prop_name = nxt_mp_alloc(ctx->pool, sizeof(njs_value_t));
        if (nxt_slow_path(prop_name == NULL)) {
            goto memory_error;
        }

        p = njs_json_parse_string(ctx, prop_name, p);
        if (nxt_slow_path(p == NULL)) {
            /* The exception is set by the called function. */
            return NULL;
        }

        p = njs_json_skip_space(p, ctx->end);
        if (nxt_slow_path(p == ctx->end || *p != ':')) {
            goto error_token;
        }

        p = njs_json_skip_space(p + 1, ctx->end);
        if (nxt_slow_path(p == ctx->end)) {
            goto error_end;
        }

        prop_value = nxt_mp_alloc(ctx->pool, sizeof(njs_value_t));
        if (nxt_slow_path(prop_value == NULL)) {
            goto memory_error;
        }

        p = njs_json_parse_value(ctx, prop_value, p);
        if (nxt_slow_path(p == NULL)) {
            /* The exception is set by the called function. */
            return NULL;
        }

        prop = njs_object_prop_alloc(ctx->vm, prop_name, prop_value, 1);
        if (nxt_slow_path(prop == NULL)) {
            goto memory_error;
        }

        njs_string_get(prop_name, &lhq.key);
        lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);
        lhq.value = prop;
        lhq.replace = 1;
        lhq.pool = ctx->pool;
        lhq.proto = &njs_object_hash_proto;

        ret = nxt_lvlhsh_insert(&object->hash, &lhq);
        if (nxt_slow_path(ret != NXT_OK)) {
            njs_internal_error(ctx->vm, "lvlhsh insert/replace failed");
            return NULL;
        }

        p = njs_json_skip_space(p, ctx->end);
        if (nxt_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p != ',') {
            if (nxt_fast_path(*p == '}')) {
                break;
            }

            goto error_token;
        }
    }

    value->data.u.object = object;
    value->type = NJS_OBJECT;
    value->data.truth = 1;

    ctx->depth++;

    return p + 1;

error_token:

    njs_json_parse_exception(ctx, "Unexpected token", p);

    return NULL;

error_end:

    njs_json_parse_exception(ctx, "Unexpected end of input", p);

    return NULL;

memory_error:

    njs_memory_error(ctx->vm);

    return NULL;
}


static const u_char *
njs_json_parse_array(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    nxt_int_t    ret;
    njs_array_t  *array;
    njs_value_t  *element;

    if (nxt_slow_path(--ctx->depth == 0)) {
        njs_json_parse_exception(ctx, "Nested too deep", p);
        return NULL;
    }

    array = njs_array_alloc(ctx->vm, 0, 0);
    if (nxt_slow_path(array == NULL)) {
        return NULL;
    }

    element = NULL;

    for ( ;; ) {
        p = njs_json_skip_space(p + 1, ctx->end);
        if (nxt_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p == ']') {
            if (nxt_slow_path(element != NULL)) {
                njs_json_parse_exception(ctx, "Trailing comma", p - 1);
                return NULL;
            }

            break;
        }

        element = nxt_mp_alloc(ctx->pool, sizeof(njs_value_t));
        if (nxt_slow_path(element == NULL)) {
            njs_memory_error(ctx->vm);
            return NULL;
        }

        p = njs_json_parse_value(ctx, element, p);
        if (nxt_slow_path(p == NULL)) {
            return NULL;
        }

        ret = njs_array_add(ctx->vm, array, element);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NULL;
        }

        p = njs_json_skip_space(p, ctx->end);
        if (nxt_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p != ',') {
            if (nxt_fast_path(*p == ']')) {
                break;
            }

            goto error_token;
        }
    }

    value->data.u.array = array;
    value->type = NJS_ARRAY;
    value->data.truth = 1;

    ctx->depth++;

    return p + 1;

error_token:

    njs_json_parse_exception(ctx, "Unexpected token", p);

    return NULL;

error_end:

    njs_json_parse_exception(ctx, "Unexpected end of input", p);

    return NULL;
}


static const u_char *
njs_json_parse_string(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    u_char        ch, *s, *dst;
    size_t        size, surplus;
    ssize_t       length;
    uint32_t      utf, utf_low;
    njs_ret_t     ret;
    const u_char  *start, *last;

    enum {
        sw_usual = 0,
        sw_escape,
        sw_encoded1,
        sw_encoded2,
        sw_encoded3,
        sw_encoded4,
    } state;

    start = p + 1;

    dst = NULL;
    state = 0;
    surplus = 0;

    for (p = start; p < ctx->end; p++) {
        ch = *p;

        switch (state) {

        case sw_usual:

            if (ch == '"') {
                break;
            }

            if (ch == '\\') {
                state = sw_escape;
                continue;
            }

            if (nxt_fast_path(ch >= ' ')) {
                continue;
            }

            njs_json_parse_exception(ctx, "Forbidden source char", p);

            return NULL;

        case sw_escape:

            switch (ch) {
            case '"':
            case '\\':
            case '/':
            case 'n':
            case 'r':
            case 't':
            case 'b':
            case 'f':
                surplus++;
                state = sw_usual;
                continue;

            case 'u':
                /*
                 * Basic unicode 6 bytes "\uXXXX" in JSON
                 * and up to 3 bytes in UTF-8.
                 *
                 * Surrogate pair: 12 bytes "\uXXXX\uXXXX" in JSON
                 * and 3 or 4 bytes in UTF-8.
                 */
                surplus += 3;
                state = sw_encoded1;
                continue;
            }

            njs_json_parse_exception(ctx, "Unknown escape char", p);

            return NULL;

        case sw_encoded1:
        case sw_encoded2:
        case sw_encoded3:
        case sw_encoded4:

            if (nxt_fast_path((ch >= '0' && ch <= '9')
                              || (ch >= 'A' && ch <= 'F')
                              || (ch >= 'a' && ch <= 'f')))
            {
                state = (state == sw_encoded4) ? sw_usual : state + 1;
                continue;
            }

            njs_json_parse_exception(ctx, "Invalid Unicode escape sequence", p);

            return NULL;
        }

        break;
    }

    if (nxt_slow_path(p == ctx->end)) {
        njs_json_parse_exception(ctx, "Unexpected end of input", p);
        return NULL;
    }

    /* Points to the ending quote mark. */
    last = p;

    size = last - start - surplus;

    if (surplus != 0) {
        p = start;

        dst = nxt_mp_alloc(ctx->pool, size);
        if (nxt_slow_path(dst == NULL)) {
            njs_memory_error(ctx->vm);;
            return NULL;
        }

        s = dst;

        do {
            ch = *p++;

            if (ch != '\\') {
                *s++ = ch;
                continue;
            }

            ch = *p++;

            switch (ch) {
            case '"':
            case '\\':
            case '/':
                *s++ = ch;
                continue;

            case 'n':
                *s++ = '\n';
                continue;

            case 'r':
                *s++ = '\r';
                continue;

            case 't':
                *s++ = '\t';
                continue;

            case 'b':
                *s++ = '\b';
                continue;

            case 'f':
                *s++ = '\f';
                continue;
            }

            /* "\uXXXX": Unicode escape sequence. */

            utf = njs_json_unicode(p);
            p += 4;

            if (utf >= 0xd800 && utf <= 0xdfff) {

                /* Surrogate pair. */

                if (utf > 0xdbff || p[0] != '\\' || p[1] != 'u') {
                    njs_json_parse_exception(ctx, "Invalid Unicode char", p);
                    return NULL;
                }

                p += 2;

                utf_low = njs_json_unicode(p);
                p += 4;

                if (nxt_slow_path(utf_low < 0xdc00 || utf_low > 0xdfff)) {
                    njs_json_parse_exception(ctx, "Invalid surrogate pair", p);
                    return NULL;
                }

                utf = njs_string_surrogate_pair(utf, utf_low);
            }

            s = nxt_utf8_encode(s, utf);

        } while (p != last);

        size = s - dst;
        start = dst;
    }

    length = nxt_utf8_length(start, size);
    if (nxt_slow_path(length < 0)) {
        length = 0;
    }

    ret = njs_string_new(ctx->vm, value, (u_char *) start, size, length);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NULL;
    }

    if (dst != NULL) {
        nxt_mp_free(ctx->pool, dst);
    }

    return last + 1;
}


static const u_char *
njs_json_parse_number(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    double        num;
    nxt_int_t     sign;
    const u_char  *start;

    sign = 1;

    if (*p == '-') {
        if (p + 1 == ctx->end) {
            goto error;
        }

        p++;
        sign = -1;
    }

    start = p;
    num = njs_number_dec_parse(&p, ctx->end);
    if (p != start) {
        *value = njs_value_zero;
        value->data.u.number = sign * num;

        return p;
    }

error:

    njs_json_parse_exception(ctx, "Unexpected number", p);

    return NULL;
}


nxt_inline uint32_t
njs_json_unicode(const u_char *p)
{
    u_char      c;
    uint32_t    utf;
    nxt_uint_t  i;

    utf = 0;

    for (i = 0; i < 4; i++) {
        utf <<= 4;
        c = p[i] | 0x20;
        c -= '0';
        if (c > 9) {
            c += '0' - 'a' + 10;
        }

        utf |= c;
    }

    return utf;
}


static const u_char *
njs_json_skip_space(const u_char *start, const u_char *end)
{
    const u_char  *p;

    for (p = start; nxt_fast_path(p != end); p++) {

        switch (*p) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            continue;
        }

        break;
    }

    return p;
}


#define njs_json_is_non_empty(_value)                                         \
    (((_value)->type == NJS_OBJECT)                                           \
      && !nxt_lvlhsh_is_empty(&(_value)->data.u.object->hash))                \
     || (((_value)->type == NJS_ARRAY) && (_value)->data.u.array->length != 0)


static njs_ret_t
njs_json_parse_continuation(njs_vm_t *vm, njs_value_t *args, nxt_uint_t unused,
    njs_index_t unused2)
{
    nxt_int_t           ret;
    njs_value_t         *key, *value;
    njs_json_state_t    *state;
    njs_json_parse_t    *parse;
    njs_object_prop_t   *prop;
    nxt_lvlhsh_query_t  lhq;

    parse = njs_vm_continuation(vm);
    state = parse->state;

    for ( ;; ) {
        switch (state->type) {
        case NJS_JSON_OBJECT_START:
            if (state->index < state->keys->length) {
                key = &state->keys->start[state->index];
                njs_string_get(key, &lhq.key);
                lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);
                lhq.proto = &njs_object_hash_proto;

                ret = nxt_lvlhsh_find(&state->value.data.u.object->hash, &lhq);
                if (nxt_slow_path(ret == NXT_DECLINED)) {
                    state->index++;
                    break;
                }

                prop = lhq.value;

                if (prop->type == NJS_WHITEOUT) {
                    state->index++;
                    break;
                }

                state->prop_value = &prop->value;

                if (njs_json_is_non_empty(&prop->value)) {
                    state = njs_json_push_parse_state(vm, parse, &prop->value);
                    if (state == NULL) {
                        goto memory_error;
                    }

                    break;
                }

            } else {
                state = njs_json_pop_parse_state(parse);
                if (state == NULL) {
                    vm->retval = parse->retval;

                    return NXT_OK;
                }
            }

            return njs_json_parse_continuation_apply(vm, parse);

        case NJS_JSON_OBJECT_REPLACED:
            key = &state->keys->start[state->index];
            njs_string_get(key, &lhq.key);
            lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);
            lhq.replace = 1;
            lhq.proto = &njs_object_hash_proto;
            lhq.pool = vm->mem_pool;

            if (njs_is_undefined(&parse->retval)) {
                ret = nxt_lvlhsh_delete(&state->value.data.u.object->hash,
                                        &lhq);

            } else {
                prop = njs_object_prop_alloc(vm, key, &parse->retval, 1);
                if (nxt_slow_path(prop == NULL)) {
                    goto memory_error;
                }

                lhq.value = prop;
                ret = nxt_lvlhsh_insert(&state->value.data.u.object->hash,
                                        &lhq);
            }

            if (nxt_slow_path(ret != NXT_OK)) {
                njs_internal_error(vm, "lvlhsh insert/replace failed");
                return NXT_ERROR;
            }

            state->index++;
            state->type = NJS_JSON_OBJECT_START;

            break;

        case NJS_JSON_ARRAY_START:
            if (state->index < state->value.data.u.array->length) {
                value = &state->value.data.u.array->start[state->index];

                if (njs_json_is_non_empty(value)) {
                    state = njs_json_push_parse_state(vm, parse, value);
                    if (state == NULL) {
                        goto memory_error;
                    }

                    break;
                }

            } else {
                (void) njs_json_pop_parse_state(parse);
            }

            return njs_json_parse_continuation_apply(vm, parse);

        case NJS_JSON_ARRAY_REPLACED:
            value = &state->value.data.u.array->start[state->index];
            *value = parse->retval;

            state->index++;
            state->type = NJS_JSON_ARRAY_START;

            break;

        default:
            njs_internal_error(vm, "Unexpected state %d in JSON.parse()",
                               state->type);
            return NXT_ERROR;
        }
    }

memory_error:

    njs_memory_error(vm);

    return NXT_ERROR;
}


static njs_ret_t
njs_json_parse_continuation_apply(njs_vm_t *vm, njs_json_parse_t *parse)
{
    njs_value_t       arguments[3];
    njs_json_state_t  *state;

    state = parse->state;

    arguments[0] = state->value;

    switch (state->type) {
    case NJS_JSON_OBJECT_START:
        arguments[1] = state->keys->start[state->index];
        arguments[2] = *state->prop_value;

        state->type = NJS_JSON_OBJECT_REPLACED;
        break;

    case NJS_JSON_ARRAY_START:
        njs_uint32_to_string(&arguments[1], state->index);
        arguments[2] = state->value.data.u.array->start[state->index];

        state->type = NJS_JSON_ARRAY_REPLACED;
        break;

    default:
        njs_internal_error(vm, "Unexpected state %d in JSON.parse() apply",
                           state->type);
        return NXT_ERROR;
    }

    njs_set_invalid(&parse->retval);

    return njs_function_apply(vm, parse->function, arguments, 3,
                              (njs_index_t) &parse->retval);
}


static njs_json_state_t *
njs_json_push_parse_state(njs_vm_t *vm, njs_json_parse_t *parse,
    njs_value_t *value)
{
    njs_json_state_t  *state;

    state = nxt_array_add(&parse->stack, &njs_array_mem_proto, vm->mem_pool);
    if (state != NULL) {
        state = nxt_array_last(&parse->stack);
        state->value = *value;
        state->index = 0;

        if (njs_is_array(value)) {
            state->type = NJS_JSON_ARRAY_START;

        } else {
            state->type = NJS_JSON_OBJECT_START;
            state->prop_value = NULL;
            state->keys = njs_value_own_enumerate(vm, value, NJS_ENUM_KEYS, 0);
            if (state->keys == NULL) {
                return NULL;
            }
        }
    }

    parse->state = state;

    return state;
}


static njs_json_state_t *
njs_json_pop_parse_state(njs_json_parse_t *parse)
{
    if (parse->stack.items > 1) {
        parse->stack.items--;
        parse->state = nxt_array_last(&parse->stack);
        return parse->state;
    }

    return NULL;
}


static void
njs_json_parse_exception(njs_json_parse_ctx_t *ctx, const char *msg,
    const u_char *pos)
{
    ssize_t  length;

    length = nxt_utf8_length(ctx->start, pos - ctx->start);
    if (nxt_slow_path(length < 0)) {
        length = 0;
    }

    njs_syntax_error(ctx->vm, "%s at position %z", msg, length);
}


#define njs_json_is_object(value)                                             \
    (((value)->type == NJS_OBJECT)                                            \
     || ((value)->type == NJS_ARRAY)                                          \
     || ((value)->type >= NJS_REGEXP))


#define njs_json_stringify_append(str, len)                                   \
    ret = njs_json_buf_append(stringify, str, len);                           \
    if (ret != NXT_OK) {                                                      \
        goto memory_error;                                                    \
    }


#define njs_json_stringify_indent(times)                                      \
    if (stringify->space.length != 0) {                                       \
        njs_json_stringify_append("\n", 1);                                   \
        for (i = 0; i < (nxt_int_t) (times) - 1; i++) {                       \
            njs_json_stringify_append((char *) stringify->space.start,        \
                                      stringify->space.length);               \
        }                                                                     \
    }


#define njs_json_stringify_append_key(key)                                    \
    if (state->written) {                                                     \
        njs_json_stringify_append(",", 1);                                    \
        njs_json_stringify_indent(stringify->stack.items);                    \
    }                                                                         \
                                                                              \
    state->written = 1;                                                       \
    njs_json_append_string(stringify, key, '\"');                             \
    njs_json_stringify_append(":", 1);                                        \
    if (stringify->space.length != 0) {                                       \
        njs_json_stringify_append(" ", 1);                                    \
    }


#define njs_json_stringify_append_value(value)                                \
    state->written = 1;                                                       \
    ret = njs_json_append_value(stringify, value);                            \
    if (nxt_slow_path(ret != NXT_OK)) {                                       \
        if (ret == NXT_DECLINED) {                                            \
            return NXT_ERROR;                                                 \
        }                                                                     \
                                                                              \
        goto memory_error;                                                    \
    }


static njs_ret_t
njs_json_stringify_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t unused, njs_index_t unused2)
{
    u_char                *start;
    size_t                size;
    ssize_t               length;
    nxt_int_t             i;
    njs_ret_t             ret;
    nxt_str_t             str;
    njs_value_t           *key, *value;
    njs_function_t        *to_json;
    njs_json_state_t      *state;
    njs_object_prop_t     *prop;
    nxt_lvlhsh_query_t    lhq;
    njs_json_stringify_t  *stringify;

    stringify = njs_vm_continuation(vm);
    state = stringify->state;

    for ( ;; ) {
        switch (state->type) {
        case NJS_JSON_OBJECT_START:
            njs_json_stringify_append("{", 1);
            njs_json_stringify_indent(stringify->stack.items);
            state->type = NJS_JSON_OBJECT_CONTINUE;

            /* Fall through. */

        case NJS_JSON_OBJECT_CONTINUE:
            if (state->index >= state->keys->length) {
                njs_json_stringify_indent(stringify->stack.items - 1);
                njs_json_stringify_append("}", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            key = &state->keys->start[state->index++];
            njs_string_get(key, &lhq.key);
            lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);
            lhq.proto = &njs_object_hash_proto;

            ret = nxt_lvlhsh_find(&state->value.data.u.object->hash, &lhq);
            if (nxt_slow_path(ret == NXT_DECLINED)) {
                break;
            }

            prop = lhq.value;

            if (!prop->enumerable
                || njs_is_undefined(&prop->value)
                || !njs_is_valid(&prop->value)
                || njs_is_function(&prop->value))
            {
                break;
            }

            if (njs_is_object(&prop->value)) {
                to_json = njs_object_to_json_function(vm, &prop->value);
                if (to_json != NULL) {
                    return njs_json_stringify_to_json(vm, stringify, to_json,
                                                      &prop->name,
                                                      &prop->value);
                }
            }

            if (njs_is_function(&stringify->replacer)) {
                return njs_json_stringify_replacer(vm, stringify, &prop->name,
                                                   &prop->value);
            }

            njs_json_stringify_append_key(&prop->name);

            if (njs_json_is_object(&prop->value)) {
                state = njs_json_push_stringify_state(vm, stringify,
                                                      &prop->value);
                if (state == NULL) {
                    return NXT_ERROR;
                }

                break;
            }

            njs_json_stringify_append_value(&prop->value);

            break;

        case NJS_JSON_OBJECT_TO_JSON_REPLACED:
            if (njs_is_undefined(&stringify->retval)) {
                state->type = NJS_JSON_OBJECT_CONTINUE;
                break;
            }

            if (njs_is_function(&stringify->replacer)) {
                return njs_json_stringify_replacer(vm, stringify,
                                                   &stringify->key,
                                                   &stringify->retval);
            }

            /* Fall through. */

        case NJS_JSON_OBJECT_REPLACED:
            state->type = NJS_JSON_OBJECT_CONTINUE;

            if (njs_is_undefined(&stringify->retval)) {
                break;
            }

            njs_json_stringify_append_key(&stringify->key);

            value = &stringify->retval;
            if (njs_is_object(value)) {
                state = njs_json_push_stringify_state(vm, stringify, value);
                if (state == NULL) {
                    return NXT_ERROR;
                }

                break;
            }

            njs_json_stringify_append_value(value);

            break;

        case NJS_JSON_ARRAY_START:
            njs_json_stringify_append("[", 1);
            njs_json_stringify_indent(stringify->stack.items);
            state->type = NJS_JSON_ARRAY_CONTINUE;

            /* Fall through. */

        case NJS_JSON_ARRAY_CONTINUE:
            if (state->index >= state->value.data.u.array->length) {
                njs_json_stringify_indent(stringify->stack.items - 1);
                njs_json_stringify_append("]", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->stack.items);
            }

            value = &state->value.data.u.array->start[state->index++];

            if (njs_is_object(value)) {
                to_json = njs_object_to_json_function(vm, value);
                if (to_json != NULL) {
                    return njs_json_stringify_to_json(vm, stringify, to_json,
                                                      NULL, value);
                }

            }

            if (njs_is_function(&stringify->replacer)) {
                return njs_json_stringify_replacer(vm, stringify, NULL, value);
            }

            if (njs_json_is_object(value)) {
                state = njs_json_push_stringify_state(vm, stringify, value);
                if (state == NULL) {
                    return NXT_ERROR;
                }

                break;
            }

            njs_json_stringify_append_value(value);

            break;

        case NJS_JSON_ARRAY_TO_JSON_REPLACED:
            if (!njs_is_undefined(&stringify->retval)
                && njs_is_function(&stringify->replacer))
            {
                return njs_json_stringify_replacer(vm, stringify, NULL,
                                                   &stringify->retval);
            }

            /* Fall through. */

        case NJS_JSON_ARRAY_REPLACED:
            state->type = NJS_JSON_ARRAY_CONTINUE;

            if (njs_json_is_object(&stringify->retval)) {
                state = njs_json_push_stringify_state(vm, stringify,
                                                      &stringify->retval);
                if (state == NULL) {
                    return NXT_ERROR;
                }

                break;
            }

            njs_json_stringify_append_value(&stringify->retval);

            break;
        }
    }

done:

    ret = njs_json_buf_pullup(stringify, &str);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto memory_error;
    }

    /*
     * The value to stringify is wrapped as '{"": value}'.
     * An empty object means empty result.
     */
    if (str.length <= nxt_length("{\n\n}")) {
        vm->retval = njs_value_undefined;
        goto release;
    }

    /* Stripping the wrapper's data. */

    start = str.start;
    size = str.length;

    start += nxt_length("{\"\":");
    size -= nxt_length("{\"\":}");

    if (stringify->space.length != 0) {
        start += nxt_length("\n ");
        size -= nxt_length("\n \n");
    }

    length = nxt_utf8_length(start, size);
    if (nxt_slow_path(length < 0)) {
        length = 0;
    }

    ret = njs_string_new(vm, &vm->retval, start, size, length);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto memory_error;
    }

release:

    nxt_mp_free(vm->mem_pool, str.start);

    return NXT_OK;

memory_error:

    njs_memory_error(vm);

    return NXT_ERROR;
}


static njs_function_t *
njs_object_to_json_function(njs_vm_t *vm, njs_value_t *value)
{
    njs_object_prop_t   *prop;
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = NJS_TO_JSON_HASH;
    lhq.key = nxt_string_value("toJSON");

    prop = njs_object_property(vm, value->data.u.object, &lhq);

    if (prop != NULL && njs_is_function(&prop->value)) {
        return prop->value.data.u.function;
    }

    return NULL;
}


static njs_ret_t
njs_json_stringify_to_json(njs_vm_t *vm, njs_json_stringify_t* stringify,
    njs_function_t *function, njs_value_t *key, njs_value_t *value)
{
    njs_value_t       arguments[2];
    njs_json_state_t  *state;

    njs_set_invalid(&stringify->retval);

    state = stringify->state;

    arguments[0] = *value;

    switch (state->type) {
    case NJS_JSON_OBJECT_START:
    case NJS_JSON_OBJECT_CONTINUE:
        if (key != NULL) {
            arguments[1] = *key;
            stringify->key = *key;

        } else {
            njs_string_short_set(&arguments[1], 0, 0);
            njs_string_short_set(&stringify->key, 0, 0);
        }

        state->type = NJS_JSON_OBJECT_TO_JSON_REPLACED;
        break;

    case NJS_JSON_ARRAY_START:
    case NJS_JSON_ARRAY_CONTINUE:
        njs_uint32_to_string(&arguments[1], state->index - 1);

        state->type = NJS_JSON_ARRAY_TO_JSON_REPLACED;
        break;

    default:
        njs_internal_error(vm, "Unexpected state %d in JSON.stringify() apply",
                           state->type);
        return NXT_ERROR;
    }

    return njs_function_apply(vm, function, arguments, 2,
                              (njs_index_t) &stringify->retval);
}


static njs_ret_t
njs_json_stringify_replacer(njs_vm_t *vm, njs_json_stringify_t* stringify,
    njs_value_t *key, njs_value_t *value)
{
    njs_value_t       arguments[3];
    njs_json_state_t  *state;

    state = stringify->state;

    arguments[0] = state->value;

    switch (state->type) {
    case NJS_JSON_OBJECT_START:
    case NJS_JSON_OBJECT_CONTINUE:
    case NJS_JSON_OBJECT_TO_JSON_REPLACED:
        arguments[1] = *key;
        stringify->key = *key;
        arguments[2] = *value;

        state->type = NJS_JSON_OBJECT_REPLACED;
        break;

    case NJS_JSON_ARRAY_START:
    case NJS_JSON_ARRAY_CONTINUE:
    case NJS_JSON_ARRAY_TO_JSON_REPLACED:
        njs_uint32_to_string(&arguments[1], state->index - 1);
        arguments[2] = *value;

        state->type = NJS_JSON_ARRAY_REPLACED;
        break;

    default:
        njs_internal_error(vm, "Unexpected state %d in "
                           "JSON.stringify() replacer", state->type);
        return NXT_ERROR;
    }

    njs_set_invalid(&stringify->retval);

    return njs_function_apply(vm, stringify->replacer.data.u.function,
                              arguments, 3, (njs_index_t) &stringify->retval);
}


static njs_ret_t
njs_json_stringify_array(njs_vm_t *vm, njs_json_stringify_t  *stringify)
{
    njs_ret_t    ret;
    uint32_t     i, n, k, properties_length, array_length;
    njs_value_t  *value, num_value;
    njs_array_t  *properties, *array;

    properties_length = 1;
    array = stringify->replacer.data.u.array;
    array_length = array->length;

    for (i = 0; i < array_length; i++) {
        if (njs_is_valid(&array->start[i])) {
            properties_length++;
        }
    }

    properties = njs_array_alloc(vm, properties_length, NJS_ARRAY_SPARE);
    if (nxt_slow_path(properties == NULL)) {
        return NXT_ERROR;
    }

    n = 0;
    properties->start[n++] = njs_string_empty;

    for (i = 0; i < array_length; i++) {
        value = &array->start[i];

        if (!njs_is_valid(&array->start[i])) {
            continue;
        }

        switch (value->type) {
        case NJS_OBJECT_NUMBER:
            value = &value->data.u.object_value->value;
            /* Fall through. */

        case NJS_NUMBER:
            ret = njs_number_to_string(vm, &num_value, value);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }

            value = &num_value;
            break;

        case NJS_OBJECT_STRING:
            value = &value->data.u.object_value->value;
            break;

        case NJS_STRING:
            break;

        default:
            continue;
        }

        for (k = 0; k < n; k ++) {
            if (njs_values_strict_equal(value, &properties->start[k]) == 1) {
                break;
            }
        }

        if (k == n) {
            properties->start[n++] = *value;
        }
    }

    properties->length = n;
    stringify->replacer.data.u.array = properties;

    return NXT_OK;
}


static njs_json_state_t *
njs_json_push_stringify_state(njs_vm_t *vm, njs_json_stringify_t *stringify,
    const njs_value_t *value)
{
    njs_json_state_t  *state;

    if (stringify->stack.items >= 32) {
        njs_type_error(stringify->vm,
                       "Nested too deep or a cyclic structure");
        return NULL;
    }

    state = nxt_array_add(&stringify->stack, &njs_array_mem_proto,
                          vm->mem_pool);
    if (nxt_slow_path(state == NULL)) {
        njs_memory_error(vm);
        return NULL;
    }

    state = nxt_array_last(&stringify->stack);
    state->value = *value;
    state->index = 0;
    state->written = 0;

    if (njs_is_array(value)) {
        state->type = NJS_JSON_ARRAY_START;

    } else {
        state->type = NJS_JSON_OBJECT_START;
        state->prop_value = NULL;

        if (njs_is_array(&stringify->replacer)) {
            state->keys = stringify->replacer.data.u.array;

        } else {
            if (njs_is_external(value)) {
                state->keys = njs_extern_keys_array(vm, value->external.proto);

            } else {
                state->keys = njs_value_own_enumerate(vm, value, NJS_ENUM_KEYS,
                                                      0);
            }

            if (state->keys == NULL) {
                return NULL;
            }
        }
    }

    stringify->state = state;
    return state;
}


static njs_json_state_t *
njs_json_pop_stringify_state(njs_json_stringify_t *stringify)
{
    if (stringify->stack.items > 1) {
        stringify->stack.items--;
        stringify->state = nxt_array_last(&stringify->stack);
        stringify->state->written = 1;
        return stringify->state;
    }

    return NULL;
}


static nxt_int_t
njs_json_append_value(njs_json_stringify_t *stringify, const njs_value_t *value)
{
    switch (value->type) {
    case NJS_OBJECT_STRING:
        value = &value->data.u.object_value->value;
        /* Fall through. */

    case NJS_STRING:
        return njs_json_append_string(stringify, value, '\"');

    case NJS_OBJECT_NUMBER:
        value = &value->data.u.object_value->value;
        /* Fall through. */

    case NJS_NUMBER:
        return njs_json_append_number(stringify, value);

    case NJS_OBJECT_BOOLEAN:
        value = &value->data.u.object_value->value;
        /* Fall through. */

    case NJS_BOOLEAN:
        if (njs_is_true(value)) {
            return njs_json_buf_append(stringify, "true", 4);

        } else {
            return njs_json_buf_append(stringify, "false", 5);
        }

    case NJS_UNDEFINED:
    case NJS_NULL:
    case NJS_INVALID:
    case NJS_FUNCTION:
    default:
        return njs_json_buf_append(stringify, "null", 4);
    }
}


static nxt_int_t
njs_json_append_string(njs_json_stringify_t *stringify,
    const njs_value_t *value, char quote)
{
    u_char             c, *dst, *dst_end;
    size_t             length;
    const u_char       *p, *end;
    njs_string_prop_t  str;

    static char   hex2char[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                   '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

    (void) njs_string_prop(&str, (njs_value_t *) value);

    p = str.start;
    end = p + str.size;
    length = str.length;

    dst = njs_json_buf_reserve(stringify, 64);
    if (nxt_slow_path(dst == NULL)) {
        return NXT_ERROR;
    }

    dst_end = dst + 64;

    *dst++ = quote;

    while (p < end) {

        if (*p < ' '
            || *p == '\\'
            || (*p == '\"' && quote == '\"'))
        {
            c = (u_char) *p++;
            *dst++ = '\\';

            switch (c) {
            case '\\':
                *dst++ = '\\';
                break;
            case '"':
                *dst++ = '\"';
                break;
            case '\r':
                *dst++ = 'r';
                break;
            case '\n':
                *dst++ = 'n';
                break;
            case '\t':
                *dst++ = 't';
                break;
            case '\b':
                *dst++ = 'b';
                break;
            case '\f':
                *dst++ = 'f';
                break;
            default:
                *dst++ = 'u';
                *dst++ = '0';
                *dst++ = '0';
                *dst++ = hex2char[(c & 0xf0) >> 4];
                *dst++ = hex2char[c & 0x0f];
            }
        }

        /*
         * Control characters less than space are encoded using 6 bytes
         * "\uXXXX".  Checking there is at least 6 bytes of destination storage
         * space.
         */

        while (p < end && (dst_end - dst) > 6) {
            if (*p < ' ' || (*p == '\"' && quote == '\"') || *p == '\\') {
                break;
            }

            if (length != 0) {
                /* UTF-8 or ASCII string. */
                dst = nxt_utf8_copy(dst, &p, end);

            } else {
                /* Byte string. */
                *dst++ = *p++;
            }
        }

        if (dst_end - dst <= 6) {
            njs_json_buf_written(stringify, dst - stringify->last->pos);

            dst = njs_json_buf_reserve(stringify, 64);
            if (nxt_slow_path(dst == NULL)) {
                return NXT_ERROR;
            }

            dst_end = dst + 64;
        }
    }

    njs_json_buf_written(stringify, dst - stringify->last->pos);

    njs_json_buf_append(stringify, &quote, 1);

    return NXT_OK;
}


static nxt_int_t
njs_json_append_number(njs_json_stringify_t *stringify,
    const njs_value_t *value)
{
    u_char  *p;
    size_t  size;
    double  num;

    num = value->data.u.number;

    if (isnan(num) || isinf(num)) {
        return njs_json_buf_append(stringify, "null", 4);

    } else {
        p = njs_json_buf_reserve(stringify, 64);
        if (nxt_slow_path(p == NULL)) {
            return NXT_ERROR;
        }

        size = nxt_dtoa(num, (char *) p);

        njs_json_buf_written(stringify, size);
    }

    return NXT_OK;
}


/*
 * Wraps a value as '{"": <value>}'.
 */
static njs_value_t *
njs_json_wrap_value(njs_vm_t *vm, const njs_value_t *value)
{
    nxt_int_t           ret;
    njs_value_t         *wrapper;
    njs_object_prop_t   *prop;
    nxt_lvlhsh_query_t  lhq;

    wrapper = nxt_mp_alloc(vm->mem_pool, sizeof(njs_value_t));
    if (nxt_slow_path(wrapper == NULL)) {
        return NULL;
    }

    wrapper->data.u.object = njs_object_alloc(vm);
    if (nxt_slow_path(wrapper->data.u.object == NULL)) {
        return NULL;
    }

    wrapper->type = NJS_OBJECT;
    wrapper->data.truth = 1;

    lhq.replace = 0;
    lhq.proto = &njs_object_hash_proto;
    lhq.pool = vm->mem_pool;
    lhq.key = nxt_string_value("");
    lhq.key_hash = NXT_DJB_HASH_INIT;

    prop = njs_object_prop_alloc(vm, &njs_string_empty, value, 1);
    if (nxt_slow_path(prop == NULL)) {
        return NULL;
    }

    lhq.value = prop;

    ret = nxt_lvlhsh_insert(&wrapper->data.u.object->hash, &lhq);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NULL;
    }

    return wrapper;
}


static nxt_int_t
njs_json_buf_append(njs_json_stringify_t *stringify, const char *msg,
    size_t len)
{
    u_char  *p;

    if (len != 0) {
        p = njs_json_buf_reserve(stringify, len);
        if (nxt_slow_path(p == NULL)) {
            return NXT_ERROR;
        }

        memcpy(p, msg, len);

        njs_json_buf_written(stringify, len);
    }

    return NXT_OK;
}


static u_char *
njs_json_buf_reserve(njs_json_stringify_t *stringify, size_t size)
{
    njs_chb_node_t  *n;

    if (nxt_slow_path(size == 0)) {
        return NULL;
    }

    n = stringify->last;

    if (nxt_fast_path(n != NULL && njs_json_buf_node_room(n) >= size)) {
        return n->pos;
    }

    if (size < NJS_JSON_BUF_MIN_SIZE) {
        size = NJS_JSON_BUF_MIN_SIZE;
    }

    n = nxt_mp_alloc(stringify->pool, sizeof(njs_chb_node_t) + size);
    if (nxt_slow_path(n == NULL)) {
        return NULL;
    }

    n->next = NULL;
    n->start = (u_char *) n + sizeof(njs_chb_node_t);
    n->pos = n->start;
    n->end = n->pos + size;

    if (stringify->last != NULL) {
        stringify->last->next = n;

    } else {
        stringify->nodes = n;
    }

    stringify->last = n;

    return n->start;
}


static nxt_int_t
njs_json_buf_pullup(njs_json_stringify_t *stringify, nxt_str_t *str)
{
    u_char          *start;
    size_t          size;
    njs_chb_node_t  *n;

    n = stringify->nodes;

    if (n == NULL) {
        str->length = 0;
        str->start = NULL;
        return NXT_OK;
    }

    if (n->next == NULL) {
        str->length = njs_json_buf_node_size(n);
        str->start = n->start;
        return NXT_OK;
    }

    size = 0;

    while (n != NULL) {
        size += njs_json_buf_node_size(n);
        n = n->next;
    }

    start = nxt_mp_alloc(stringify->pool, size);
    if (nxt_slow_path(start == NULL)) {
        return NXT_ERROR;
    }

    n = stringify->nodes;
    str->length = size;
    str->start = start;

    while (n != NULL) {
        size = njs_json_buf_node_size(n);
        memcpy(start, n->start, size);
        start += size;
        n = n->next;
    }

    return NXT_OK;
}


static const njs_object_prop_t  njs_json_object_properties[] =
{
    /* JSON.parse(). */
    {
        .type = NJS_METHOD,
        .name = njs_string("parse"),
        .value = njs_native_function(njs_json_parse,
                                    njs_continuation_size(njs_json_parse_t),
                                    NJS_SKIP_ARG, NJS_STRING_ARG,
                                    NJS_OBJECT_ARG),
    },

    /* JSON.stringify(). */
    {
        .type = NJS_METHOD,
        .name = njs_string("stringify"),
        .value = njs_native_function(njs_json_stringify,
                                    njs_continuation_size(njs_json_stringify_t),
                                    NJS_SKIP_ARG, NJS_SKIP_ARG, NJS_SKIP_ARG,
                                    NJS_SKIP_ARG),
    },
};


const njs_object_init_t  njs_json_object_init = {
    nxt_string("JSON"),
    njs_json_object_properties,
    nxt_nitems(njs_json_object_properties),
};


#define njs_dump(str)                                                         \
    ret = njs_json_buf_append(stringify, str, nxt_length(str));               \
    if (nxt_slow_path(ret != NXT_OK)) {                                       \
        goto memory_error;                                                    \
    }


#define njs_dump_item(str)                                                    \
    if (written) {                                                            \
        njs_json_buf_append(stringify, ",", 1);                               \
    }                                                                         \
                                                                              \
    written = 1;                                                              \
    ret = njs_json_buf_append(stringify, str, nxt_length(str));               \
    if (nxt_slow_path(ret != NXT_OK)) {                                       \
        goto memory_error;                                                    \
    }


static nxt_int_t
njs_dump_value(njs_json_stringify_t *stringify, const njs_value_t *value,
    nxt_uint_t console)
{
    njs_ret_t           ret;
    nxt_str_t           str;
    nxt_uint_t          written;
    njs_value_t         str_val;
    const njs_extern_t  *ext_proto;
    u_char              buf[32], *p;

    njs_ret_t           (*to_string)(njs_vm_t *, njs_value_t *,
                                     const njs_value_t *);

    switch (value->type) {
    case NJS_OBJECT_STRING:
        value = &value->data.u.object_value->value;

        njs_string_get(value, &str);

        njs_dump("[String: ");
        njs_json_append_string(stringify, value, '\'');
        njs_dump("]")
        break;

    case NJS_STRING:
        njs_string_get(value, &str);

        if (!console || stringify->stack.items != 0) {
            return njs_json_append_string(stringify, value, '\'');
        }

        return njs_json_buf_append(stringify, (char *) str.start, str.length);

        break;

    case NJS_OBJECT_NUMBER:
        value = &value->data.u.object_value->value;

        ret = njs_number_to_string(stringify->vm, &str_val, value);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        njs_string_get(&str_val, &str);

        njs_dump("[Number: ");
        njs_json_buf_append(stringify, (char *) str.start, str.length);
        njs_dump("]")
        break;

    case NJS_OBJECT_BOOLEAN:
        value = &value->data.u.object_value->value;

        if (njs_is_true(value)) {
            njs_dump("[Boolean: true]");

        } else {
            njs_dump("[Boolean: false]");
        }

        break;

    case NJS_BOOLEAN:
        if (njs_is_true(value)) {
            njs_dump("true");

        } else {
            njs_dump("false");
        }

        break;

    case NJS_UNDEFINED:
        njs_dump("undefined");
        break;

    case NJS_NULL:
        njs_dump("null");
        break;

    case NJS_INVALID:
        njs_dump("<empty>");
        break;

    case NJS_FUNCTION:
        if (value->data.u.function->native) {
            njs_dump("[Function: native]");

        } else {
            njs_dump("[Function]");
        }

        break;

    case NJS_EXTERNAL:
        ext_proto = value->external.proto;

        written = 0;
        njs_dump_item("{type:");

        switch (ext_proto->type) {
        case NJS_EXTERN_PROPERTY:
            njs_dump("\"property\"");
            break;
        case NJS_EXTERN_METHOD:
            njs_dump("\"method\"");
            break;
        case NJS_EXTERN_OBJECT:
            njs_dump("\"object\"");
            break;
        case NJS_EXTERN_CASELESS_OBJECT:
            njs_dump("\"caseless_object\"");
            break;
        }

        njs_dump_item("props:[");
        written = 0;

        if (ext_proto->get != NULL) {
            njs_dump_item("\"getter\"");
        }

        if (ext_proto->set != NULL) {
            njs_dump_item("\"setter\"");
        }

        if (ext_proto->function != NULL) {
            njs_dump_item("\"method\"");
        }

        if (ext_proto->find != NULL) {
            njs_dump_item("\"find\"");
        }

        if (ext_proto->foreach != NULL) {
            njs_dump_item("\"foreach\"");
        }

        if (ext_proto->next != NULL) {
            njs_dump_item("\"next\"");
        }

        return njs_json_buf_append(stringify, "]}", 2);

    case NJS_NUMBER:
    case NJS_REGEXP:
    case NJS_DATE:
    case NJS_OBJECT_ERROR:
    case NJS_OBJECT_EVAL_ERROR:
    case NJS_OBJECT_INTERNAL_ERROR:
    case NJS_OBJECT_RANGE_ERROR:
    case NJS_OBJECT_REF_ERROR:
    case NJS_OBJECT_SYNTAX_ERROR:
    case NJS_OBJECT_TYPE_ERROR:
    case NJS_OBJECT_URI_ERROR:

        switch (value->type) {
        case NJS_NUMBER:
            to_string = njs_number_to_string;
            break;

        case NJS_REGEXP:
            to_string = njs_regexp_to_string;
            break;

        case NJS_DATE:
            to_string = njs_date_to_string;
            break;

        default:
            to_string = njs_error_to_string;
        }

        ret = to_string(stringify->vm, &str_val, value);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        njs_string_get(&str_val, &str);

        return njs_json_buf_append(stringify, (char *) str.start, str.length);

    default:
        p = nxt_sprintf(buf, buf + nxt_length(buf), "[Unknown value type:%uD]",
                        value->type);
        return njs_json_buf_append(stringify, (char *) buf, p - buf);
    }

    return ret;

memory_error:

    njs_memory_error(stringify->vm);

    return NXT_ERROR;
}


#define njs_dump_is_object(value)                                             \
    (((value)->type == NJS_OBJECT)                                            \
     || ((value)->type == NJS_ARRAY)                                          \
     || ((value)->type == NJS_OBJECT_VALUE)                                   \
     || ((value)->type == NJS_EXTERNAL                                        \
         && !nxt_lvlhsh_is_empty(&(value)->external.proto->hash)))


#define njs_dump_append_value(value)                                          \
    state->written = 1;                                                       \
    ret = njs_dump_value(stringify, value, console);                          \
    if (nxt_slow_path(ret != NXT_OK)) {                                       \
        if (ret == NXT_DECLINED) {                                            \
            goto exception;                                                   \
        }                                                                     \
                                                                              \
        goto memory_error;                                                    \
    }


njs_ret_t
njs_vm_value_dump(njs_vm_t *vm, nxt_str_t *retval, const njs_value_t *value,
    nxt_uint_t console, nxt_uint_t indent)
{
    nxt_int_t             i;
    njs_ret_t             ret;
    nxt_str_t             str;
    njs_value_t           *key, *val, ext_val;
    njs_json_state_t      *state;
    njs_object_prop_t     *prop;
    nxt_lvlhsh_query_t    lhq;
    njs_json_stringify_t  *stringify;

    if (njs_vm_backtrace(vm) != NULL) {
        goto exception;
    }

    stringify = nxt_mp_alloc(vm->mem_pool, sizeof(njs_json_stringify_t));

    if (nxt_slow_path(stringify == NULL)) {
        goto memory_error;
    }

    stringify->vm = vm;
    stringify->pool = vm->mem_pool;
    stringify->nodes = NULL;
    stringify->last = NULL;
    stringify->stack.items = 0;

    if (!njs_dump_is_object(value)) {
        ret = njs_dump_value(stringify, value, console);
        if (nxt_slow_path(ret != NXT_OK)) {
            goto memory_error;
        }

        goto done;
    }

    stringify->space.length = indent;
    stringify->space.start = nxt_mp_alloc(vm->mem_pool, indent);
    if (nxt_slow_path(stringify->space.start == NULL)) {
        goto memory_error;
    }

    nxt_memset(stringify->space.start, ' ', indent);

    if (nxt_array_init(&stringify->stack, NULL, 4, sizeof(njs_json_state_t),
                       &njs_array_mem_proto, vm->mem_pool)
        == NULL)
    {
        goto memory_error;
    }

    if (njs_json_push_stringify_state(vm, stringify, value) == NULL) {
        goto memory_error;
    }

    state = stringify->state;

    for ( ;; ) {
        switch (state->type) {
        case NJS_JSON_OBJECT_START:
            njs_json_stringify_append("{", 1);
            njs_json_stringify_indent(stringify->stack.items + 1);
            state->type = NJS_JSON_OBJECT_CONTINUE;

            /* Fall through. */

        case NJS_JSON_OBJECT_CONTINUE:
            if (state->index >= state->keys->length) {
                njs_json_stringify_indent(stringify->stack.items);
                njs_json_stringify_append("}", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            key = &state->keys->start[state->index++];
            njs_string_get(key, &lhq.key);
            lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);

            if (njs_is_external(&state->value)) {
                lhq.proto = &njs_extern_hash_proto;

                ret = nxt_lvlhsh_find(&state->value.external.proto->hash, &lhq);
                if (nxt_slow_path(ret == NXT_DECLINED)) {
                    break;
                }

                ext_val.type = NJS_EXTERNAL;
                ext_val.data.truth = 1;
                ext_val.external.proto = lhq.value;

                val = &ext_val;

            } else {
                lhq.proto = &njs_object_hash_proto;

                ret = nxt_lvlhsh_find(&state->value.data.u.object->hash, &lhq);
                if (nxt_slow_path(ret == NXT_DECLINED)) {
                    break;
                }

                prop = lhq.value;
                val = &prop->value;

                if (prop->type == NJS_WHITEOUT || !prop->enumerable) {
                    break;
                }
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->stack.items + 1);
            }

            state->written = 1;
            njs_json_stringify_append((char *) lhq.key.start, lhq.key.length);
            njs_json_stringify_append(":", 1);
            if (stringify->space.length != 0) {
                njs_json_stringify_append(" ", 1);
            }

            if (njs_dump_is_object(val)) {
                state = njs_json_push_stringify_state(vm, stringify, val);
                if (state == NULL) {
                    goto exception;
                }

                break;
            }

            njs_dump_append_value(val);

            break;

        case NJS_JSON_ARRAY_START:
            njs_json_stringify_append("[", 1);
            njs_json_stringify_indent(stringify->stack.items + 1);
            state->type = NJS_JSON_ARRAY_CONTINUE;

            /* Fall through. */

        case NJS_JSON_ARRAY_CONTINUE:
            if (state->index >= state->value.data.u.array->length) {
                njs_json_stringify_indent(stringify->stack.items);
                njs_json_stringify_append("]", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->stack.items + 1);
            }

            val = &state->value.data.u.array->start[state->index++];

            if (njs_dump_is_object(val)) {
                state = njs_json_push_stringify_state(vm, stringify, val);
                if (state == NULL) {
                    goto exception;
                }

                break;
            }

            njs_dump_append_value(val);

            break;

        default:
            nxt_unreachable();
        }
    }

done:

    ret = njs_json_buf_pullup(stringify, &str);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto memory_error;
    }

    *retval = str;

    return NXT_OK;

memory_error:

    njs_memory_error(vm);

exception:

    njs_vm_value_to_ext_string(vm, retval, &vm->retval, 1);

    return NXT_OK;
}
