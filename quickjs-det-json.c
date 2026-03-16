/* Deterministic JSON built-ins for deterministic mode only.
   Included by quickjs.c so this implementation can reuse internal helpers
   such as StringBuffer, string_getc(), js_get_length64(), and
   JS_GetOwnPropertyNamesInternal() without widening the public/internal
   header surface. */

#define JS_GAS_JSON_PARSE_BASE 8
#define JS_GAS_JSON_PARSE_INPUT_BYTE 1
#define JS_GAS_JSON_PARSE_VALUE 3
#define JS_GAS_JSON_PARSE_OBJECT_ENTRY 2
#define JS_GAS_JSON_PARSE_ARRAY_ELEMENT 2

#define JS_GAS_JSON_STRINGIFY_BASE 8
#define JS_GAS_JSON_STRINGIFY_VALUE 3
#define JS_GAS_JSON_STRINGIFY_OBJECT_ENTRY 2
#define JS_GAS_JSON_STRINGIFY_ARRAY_ELEMENT 2
#define JS_GAS_JSON_STRINGIFY_OUTPUT_BYTE 1
#define JS_GAS_JSON_STRINGIFY_SORT_COMPARISON 1

#define JS_GAS_SITE_JSON_PARSE UINT32_C(3001)
#define JS_GAS_SITE_JSON_STRINGIFY UINT32_C(3002)

typedef struct JSDetJSONStringifyKeyEntry {
    JSAtom atom;
    uint8_t *encoded_key;
    size_t encoded_key_len;
} JSDetJSONStringifyKeyEntry;

typedef struct JSDetJSONStringifyState {
    StringBuffer *b;
    JSObject **stack;
    uint32_t stack_len;
    uint32_t stack_cap;
    uint64_t output_bytes;
} JSDetJSONStringifyState;

static int js_det_json_add_u64(uint64_t *out, uint64_t a, uint64_t b)
{
    if (a > UINT64_MAX - b)
        return -1;
    *out = a + b;
    return 0;
}

static int js_det_json_mul_u64(uint64_t *out, uint64_t a, uint64_t b)
{
    if (a != 0 && b > UINT64_MAX / a)
        return -1;
    *out = a * b;
    return 0;
}

static int js_det_json_parse_add_count(JSContext *ctx,
                                       uint64_t *total,
                                       uint64_t amount,
                                       const char *label)
{
    if (js_det_json_add_u64(total, *total, amount) != 0) {
        JS_ThrowTypeError(ctx, "JSON.parse %s overflow", label);
        return -1;
    }
    return 0;
}

static int js_det_json_parse_charge(JSContext *ctx,
                                    uint64_t gas_cost,
                                    BOOL count_call,
                                    uint64_t input_bytes,
                                    uint64_t value_count,
                                    uint64_t object_entry_count,
                                    uint64_t array_element_count)
{
    if (JS_UseGasAt(ctx,
                    gas_cost,
                    JS_GAS_SITE_JSON_PARSE,
                    JS_GAS_CHARGE_KIND_JSON_PARSE,
                    value_count + object_entry_count + array_element_count) != 0)
        return -1;
    js_gas_trace_record_json_parse(ctx, gas_cost, count_call, input_bytes,
                                   value_count, object_entry_count,
                                   array_element_count);
    return 0;
}

static int js_det_json_stringify_charge(JSContext *ctx,
                                        uint64_t gas_cost,
                                        BOOL count_call,
                                        uint64_t output_bytes,
                                        uint64_t value_count,
                                        uint64_t object_entry_count,
                                        uint64_t array_element_count,
                                        uint64_t sort_comparison_count)
{
    if (JS_UseGasAt(ctx,
                    gas_cost,
                    JS_GAS_SITE_JSON_STRINGIFY,
                    JS_GAS_CHARGE_KIND_JSON_STRINGIFY,
                    value_count + object_entry_count + array_element_count) != 0)
        return -1;
    js_gas_trace_record_json_stringify(ctx, gas_cost, count_call, output_bytes,
                                       value_count, object_entry_count,
                                       array_element_count,
                                       sort_comparison_count);
    return 0;
}

static int js_det_json_charge_units(JSContext *ctx,
                                    uint64_t unit_cost,
                                    uint64_t unit_count,
                                    BOOL count_call,
                                    uint64_t output_bytes,
                                    uint64_t value_count,
                                    uint64_t object_entry_count,
                                    uint64_t array_element_count,
                                    uint64_t sort_comparison_count)
{
    uint64_t gas_cost;

    if (unit_count == 0)
        return 0;
    if (js_det_json_mul_u64(&gas_cost, unit_cost, unit_count) != 0) {
        JS_ThrowTypeError(ctx, "JSON.stringify gas overflow");
        return -1;
    }
    return js_det_json_stringify_charge(ctx, gas_cost, count_call, output_bytes,
                                        value_count, object_entry_count,
                                        array_element_count,
                                        sort_comparison_count);
}

static int js_det_json_string_utf8_len(uint32_t c)
{
    if (c < 0x80)
        return 1;
    if (c < 0x800)
        return 2;
    if (c < 0x10000)
        return 3;
    return 4;
}

static int js_det_json_stringify_emit_ascii(JSContext *ctx,
                                            JSDetJSONStringifyState *state,
                                            const char *bytes,
                                            size_t len)
{
    uint64_t next_output_bytes;

    if (len == 0)
        return 0;
    if (js_det_json_add_u64(&next_output_bytes, state->output_bytes, len) != 0) {
        JS_ThrowTypeError(ctx, "JSON.stringify output byte count overflow");
        return -1;
    }
    if (next_output_bytes > JS_DV_LIMIT_DEFAULTS.max_encoded_bytes) {
        JS_ThrowTypeError(ctx, "JSON.stringify output exceeds maxOutputBytes (%" PRIu64 " > %u)",
                          next_output_bytes, JS_DV_LIMIT_DEFAULTS.max_encoded_bytes);
        return -1;
    }
    if (js_det_json_charge_units(ctx, JS_GAS_JSON_STRINGIFY_OUTPUT_BYTE, len,
                                 FALSE, len, 0, 0, 0, 0) != 0) {
        return -1;
    }
    if (string_buffer_write8(state->b, (const uint8_t *)bytes, len) != 0)
        return -1;
    state->output_bytes = next_output_bytes;
    return 0;
}

static int js_det_json_stringify_emit_codepoint(JSContext *ctx,
                                                JSDetJSONStringifyState *state,
                                                uint32_t c)
{
    uint64_t len = js_det_json_string_utf8_len(c);
    uint64_t next_output_bytes;

    if (js_det_json_add_u64(&next_output_bytes, state->output_bytes, len) != 0) {
        JS_ThrowTypeError(ctx, "JSON.stringify output byte count overflow");
        return -1;
    }
    if (next_output_bytes > JS_DV_LIMIT_DEFAULTS.max_encoded_bytes) {
        JS_ThrowTypeError(ctx, "JSON.stringify output exceeds maxOutputBytes (%" PRIu64 " > %u)",
                          next_output_bytes, JS_DV_LIMIT_DEFAULTS.max_encoded_bytes);
        return -1;
    }
    if (js_det_json_charge_units(ctx, JS_GAS_JSON_STRINGIFY_OUTPUT_BYTE, len,
                                 FALSE, len, 0, 0, 0, 0) != 0) {
        return -1;
    }
    if (string_buffer_putc(state->b, c) != 0)
        return -1;
    state->output_bytes = next_output_bytes;
    return 0;
}

static int js_det_json_prepare_string(JSContext *ctx,
                                      JSValueConst value,
                                      const char *label,
                                      JSValue *out_str,
                                      size_t *out_utf8_len)
{
    JSValue str;
    JSString *p;
    const char *utf8 = NULL;
    size_t utf8_len = 0;

    str = JS_ToStringCheckObject(ctx, value);
    if (JS_IsException(str))
        return -1;

    p = JS_VALUE_GET_STRING(str);
    if (js_string_find_invalid_codepoint(p) >= 0) {
        JS_FreeValue(ctx, str);
        JS_ThrowTypeError(ctx, "%s contains lone surrogate code points", label);
        return -1;
    }

    utf8 = JS_ToCStringLen(ctx, &utf8_len, str);
    if (!utf8) {
        JS_FreeValue(ctx, str);
        return -1;
    }
    JS_FreeCString(ctx, utf8);

    if (utf8_len > JS_DV_LIMIT_DEFAULTS.max_string_bytes) {
        JS_FreeValue(ctx, str);
        JS_ThrowTypeError(ctx, "%s exceeds maxStringBytes (%zu > %u)",
                          label, utf8_len, JS_DV_LIMIT_DEFAULTS.max_string_bytes);
        return -1;
    }

    *out_str = str;
    if (out_utf8_len)
        *out_utf8_len = utf8_len;
    return 0;
}

static int js_det_json_validate_string(JSContext *ctx,
                                       JSValueConst value,
                                       const char *label)
{
    JSValue str = JS_UNDEFINED;
    int ret;

    ret = js_det_json_prepare_string(ctx, value, label, &str, NULL);
    JS_FreeValue(ctx, str);
    return ret;
}

static int js_det_json_stringify_quote(JSContext *ctx,
                                       JSDetJSONStringifyState *state,
                                       JSValueConst value)
{
    JSValue str = JS_UNDEFINED;
    JSString *p;
    int i;
    uint32_t c;
    char buf[16];

    if (js_det_json_prepare_string(ctx, value, "JSON.stringify string",
                                   &str, NULL) != 0) {
        return -1;
    }

    p = JS_VALUE_GET_STRING(str);
    if (js_det_json_stringify_emit_ascii(ctx, state, "\"", 1) != 0)
        goto fail;
    for (i = 0; i < p->len; ) {
        c = string_getc(p, &i);
        switch (c) {
        case '\t':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\t", 2) != 0)
                goto fail;
            break;
        case '\r':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\r", 2) != 0)
                goto fail;
            break;
        case '\n':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\n", 2) != 0)
                goto fail;
            break;
        case '\b':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\b", 2) != 0)
                goto fail;
            break;
        case '\f':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\f", 2) != 0)
                goto fail;
            break;
        case '\"':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\\"", 2) != 0)
                goto fail;
            break;
        case '\\':
            if (js_det_json_stringify_emit_ascii(ctx, state, "\\\\", 2) != 0)
                goto fail;
            break;
        default:
            if (c < 32 || is_surrogate(c)) {
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                if (js_det_json_stringify_emit_ascii(ctx, state, buf, 6) != 0)
                    goto fail;
            } else if (js_det_json_stringify_emit_codepoint(ctx, state, c) != 0) {
                goto fail;
            }
            break;
        }
    }
    if (js_det_json_stringify_emit_ascii(ctx, state, "\"", 1) != 0)
        goto fail;

    JS_FreeValue(ctx, str);
    return 0;

fail:
    JS_FreeValue(ctx, str);
    return -1;
}

static int js_det_json_stringify_emit_primitive(JSContext *ctx,
                                                JSDetJSONStringifyState *state,
                                                JSValue value)
{
    JSValue str;
    const char *utf8 = NULL;
    size_t utf8_len = 0;
    uint64_t next_output_bytes;

    str = JS_ToStringFree(ctx, value);
    if (JS_IsException(str))
        return -1;

    utf8 = JS_ToCStringLen(ctx, &utf8_len, str);
    if (!utf8) {
        JS_FreeValue(ctx, str);
        return -1;
    }
    JS_FreeCString(ctx, utf8);

    if (js_det_json_add_u64(&next_output_bytes, state->output_bytes, utf8_len) != 0) {
        JS_FreeValue(ctx, str);
        JS_ThrowTypeError(ctx, "JSON.stringify output byte count overflow");
        return -1;
    }
    if (next_output_bytes > JS_DV_LIMIT_DEFAULTS.max_encoded_bytes) {
        JS_FreeValue(ctx, str);
        JS_ThrowTypeError(ctx, "JSON.stringify output exceeds maxOutputBytes (%" PRIu64 " > %u)",
                          next_output_bytes, JS_DV_LIMIT_DEFAULTS.max_encoded_bytes);
        return -1;
    }
    if (js_det_json_charge_units(ctx, JS_GAS_JSON_STRINGIFY_OUTPUT_BYTE,
                                 utf8_len, FALSE, utf8_len, 0, 0, 0, 0) != 0) {
        JS_FreeValue(ctx, str);
        return -1;
    }
    if (string_buffer_concat_value_free(state->b, str) != 0)
        return -1;
    state->output_bytes = next_output_bytes;
    return 0;
}

static int js_det_json_parse_preflight_value(JSParseState *s,
                                             uint32_t depth);
static int js_det_json_stringify_value(JSContext *ctx,
                                       JSDetJSONStringifyState *state,
                                       JSValueConst value,
                                       uint32_t depth);

static int js_det_json_parse_validate_string(JSContext *ctx, JSValueConst value)
{
    return js_det_json_validate_string(ctx, value, "JSON.parse string");
}

static int js_det_json_parse_validate_number(JSContext *ctx, JSValueConst value)
{
    switch (JS_VALUE_GET_NORM_TAG(value)) {
    case JS_TAG_INT:
        return 0;
    case JS_TAG_FLOAT64:
        if (!isfinite(JS_VALUE_GET_FLOAT64(value))) {
            JS_ThrowTypeError(ctx, "JSON.parse result contains a non-finite number");
            return -1;
        }
        return 0;
    default:
        JS_ThrowTypeError(ctx, "JSON.parse produced an unsupported value type");
        return -1;
    }
}

static int js_det_json_parse_unexpected_token(JSParseState *s)
{
    if (s->token.val == TOK_EOF) {
        js_parse_error(s, "Unexpected end of JSON input");
    } else {
        js_parse_error(s, "unexpected token: '%.*s'",
                       (int)(s->buf_ptr - s->token.ptr), s->token.ptr);
    }
    return -1;
}

static int js_det_json_parse_preflight_object(JSParseState *s,
                                              uint32_t depth)
{
    JSContext *ctx = s->ctx;
    uint64_t entry_count = 0;

    if (depth + 1 > JS_DV_LIMIT_DEFAULTS.max_depth) {
        JS_ThrowTypeError(ctx, "JSON.parse maxDepth %u exceeded",
                          JS_DV_LIMIT_DEFAULTS.max_depth);
        return -1;
    }
    s->det_json_string_label = "JSON.parse key";
    if (json_next_token(s) != 0)
        return -1;

    if (s->token.val != '}') {
        for (;;) {
            if (s->token.val != TOK_STRING) {
                js_parse_error(s, "expecting property name");
                return -1;
            }
            if (js_det_json_parse_add_count(ctx, &entry_count, 1,
                                            "object entry count") != 0) {
                return -1;
            }
            if (entry_count > JS_DV_LIMIT_DEFAULTS.max_map_length) {
                JS_ThrowTypeError(ctx, "JSON.parse object entries exceed maxMapLength (%" PRIu64 " > %u)",
                                  entry_count, JS_DV_LIMIT_DEFAULTS.max_map_length);
                return -1;
            }
            if (js_det_json_parse_charge(ctx, JS_GAS_JSON_PARSE_OBJECT_ENTRY,
                                         FALSE, 0, 0, 1, 0) != 0) {
                return -1;
            }
            if (!s->det_json_skip_string_materialization &&
                js_det_json_validate_string(ctx, s->token.u.str.str,
                                            "JSON.parse key") != 0) {
                return -1;
            }
            if (json_next_token(s) != 0)
                return -1;
            s->det_json_string_label = "JSON.parse string";
            if (json_parse_expect(s, ':') != 0)
                return -1;
            if (js_det_json_parse_preflight_value(s, depth + 1) != 0)
                return -1;
            if (s->token.val != ',')
                break;
            s->det_json_string_label = "JSON.parse key";
            if (json_next_token(s) != 0)
                return -1;
        }
    }

    if (json_parse_expect(s, '}') != 0)
        return -1;
    return 0;
}

static int js_det_json_parse_preflight_array(JSParseState *s,
                                             uint32_t depth)
{
    JSContext *ctx = s->ctx;
    uint64_t length = 0;

    if (depth + 1 > JS_DV_LIMIT_DEFAULTS.max_depth) {
        JS_ThrowTypeError(ctx, "JSON.parse maxDepth %u exceeded",
                          JS_DV_LIMIT_DEFAULTS.max_depth);
        return -1;
    }
    s->det_json_string_label = "JSON.parse string";
    if (json_next_token(s) != 0)
        return -1;

    if (s->token.val != ']') {
        for (;;) {
            if (js_det_json_parse_add_count(ctx, &length, 1,
                                            "array length") != 0) {
                return -1;
            }
            if (length > JS_DV_LIMIT_DEFAULTS.max_array_length) {
                JS_ThrowTypeError(ctx, "JSON.parse array length exceeds maxArrayLength (%" PRIu64 " > %u)",
                                  length, JS_DV_LIMIT_DEFAULTS.max_array_length);
                return -1;
            }
            if (js_det_json_parse_charge(ctx, JS_GAS_JSON_PARSE_ARRAY_ELEMENT,
                                         FALSE, 0, 0, 0, 1) != 0) {
                return -1;
            }
            if (js_det_json_parse_preflight_value(s, depth + 1) != 0)
                return -1;
            if (s->token.val != ',')
                break;
            s->det_json_string_label = "JSON.parse string";
            if (json_next_token(s) != 0)
                return -1;
        }
    }

    if (json_parse_expect(s, ']') != 0)
        return -1;
    return 0;
}

static int js_det_json_parse_preflight_value(JSParseState *s,
                                             uint32_t depth)
{
    JSContext *ctx = s->ctx;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return -1;
    }
    if (js_det_json_parse_charge(ctx, JS_GAS_JSON_PARSE_VALUE, FALSE, 0, 1, 0, 0) != 0)
        return -1;

    switch (s->token.val) {
    case '{':
        return js_det_json_parse_preflight_object(s, depth);
    case '[':
        return js_det_json_parse_preflight_array(s, depth);
    case TOK_STRING:
        if (!s->det_json_skip_string_materialization &&
            js_det_json_parse_validate_string(ctx, s->token.u.str.str) != 0)
            return -1;
        if (json_next_token(s) != 0)
            return -1;
        return 0;
    case TOK_NUMBER:
        if (js_det_json_parse_validate_number(ctx, s->token.u.num.val) != 0)
            return -1;
        if (json_next_token(s) != 0)
            return -1;
        return 0;
    case TOK_IDENT:
        if (s->token.u.ident.atom == JS_ATOM_false ||
            s->token.u.ident.atom == JS_ATOM_true ||
            s->token.u.ident.atom == JS_ATOM_null) {
            if (json_next_token(s) != 0)
                return -1;
            return 0;
        }
        return js_det_json_parse_unexpected_token(s);
    default:
        return js_det_json_parse_unexpected_token(s);
    }
}

static int js_det_json_stringify_stack_contains(JSDetJSONStringifyState *state,
                                                JSValueConst value)
{
    JSObject *obj = JS_VALUE_GET_OBJ(value);

    for (uint32_t i = 0; i < state->stack_len; i++) {
        if (state->stack[i] == obj)
            return 1;
    }
    return 0;
}

static int js_det_json_stringify_stack_push(JSContext *ctx,
                                            JSDetJSONStringifyState *state,
                                            JSValueConst value)
{
    JSObject **new_stack;
    uint32_t new_cap;

    if (state->stack_len == state->stack_cap) {
        new_cap = state->stack_cap ? state->stack_cap * 2 : 8;
        new_stack = js_realloc(ctx, state->stack, sizeof(*new_stack) * new_cap);
        if (!new_stack) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        state->stack = new_stack;
        state->stack_cap = new_cap;
    }
    state->stack[state->stack_len++] = JS_VALUE_GET_OBJ(value);
    return 0;
}

static void js_det_json_stringify_stack_pop(JSDetJSONStringifyState *state)
{
    if (state->stack_len > 0)
        state->stack_len--;
}

static int js_det_json_is_plain_object(JSContext *ctx, JSValueConst value)
{
    JSValue proto = JS_UNDEFINED;
    JSValue object_proto = JS_UNDEFINED;
    BOOL same_proto = FALSE;
    int ret;

    if (!JS_IsObject(value))
        return 0;
    ret = JS_IsArray(ctx, value);
    if (ret < 0)
        return -1;
    if (ret)
        return 0;
    if (JS_VALUE_GET_OBJ(value)->class_id != JS_CLASS_OBJECT)
        return 0;

    proto = JS_GetPrototype(ctx, value);
    if (JS_IsException(proto))
        return -1;
    if (JS_IsNull(proto)) {
        JS_FreeValue(ctx, proto);
        return 1;
    }

    object_proto = JS_GetClassProto(ctx, JS_CLASS_OBJECT);
    same_proto = JS_StrictEq(ctx, proto, object_proto);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, object_proto);
    return same_proto ? 1 : 0;
}

static int js_det_json_compare_encoded_keys_raw(const uint8_t *a,
                                                size_t a_len,
                                                const uint8_t *b,
                                                size_t b_len)
{
    int cmp;

    if (a_len != b_len)
        return a_len < b_len ? -1 : 1;
    cmp = memcmp(a, b, a_len);
    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    return 0;
}

static size_t js_det_json_encode_text_prefix(uint8_t header[9], size_t byte_len)
{
    uint64_t value = byte_len;

    if (byte_len <= 23) {
        header[0] = (uint8_t)((3 << 5) | byte_len);
        return 1;
    }
    if (byte_len <= 0xff) {
        header[0] = (uint8_t)((3 << 5) | 24);
        header[1] = (uint8_t)byte_len;
        return 2;
    }
    if (byte_len <= 0xffff) {
        header[0] = (uint8_t)((3 << 5) | 25);
        header[1] = (uint8_t)(byte_len >> 8);
        header[2] = (uint8_t)byte_len;
        return 3;
    }
    if (byte_len <= 0xffffffffULL) {
        header[0] = (uint8_t)((3 << 5) | 26);
        header[1] = (uint8_t)(byte_len >> 24);
        header[2] = (uint8_t)(byte_len >> 16);
        header[3] = (uint8_t)(byte_len >> 8);
        header[4] = (uint8_t)byte_len;
        return 5;
    }

    header[0] = (uint8_t)((3 << 5) | 27);
    header[1] = (uint8_t)(value >> 56);
    header[2] = (uint8_t)(value >> 48);
    header[3] = (uint8_t)(value >> 40);
    header[4] = (uint8_t)(value >> 32);
    header[5] = (uint8_t)(value >> 24);
    header[6] = (uint8_t)(value >> 16);
    header[7] = (uint8_t)(value >> 8);
    header[8] = (uint8_t)value;
    return 9;
}

static int js_det_json_build_encoded_key(JSContext *ctx,
                                         JSValueConst key,
                                         uint8_t **out_bytes,
                                         size_t *out_len)
{
    JSValue str = JS_UNDEFINED;
    const char *utf8 = NULL;
    size_t utf8_len;
    uint8_t header[9];
    size_t header_len;
    uint8_t *encoded;

    if (js_det_json_prepare_string(ctx, key, "JSON.stringify key",
                                   &str, &utf8_len) != 0) {
        return -1;
    }
    utf8 = JS_ToCStringLen(ctx, &utf8_len, str);
    if (!utf8) {
        JS_FreeValue(ctx, str);
        return -1;
    }

    header_len = js_det_json_encode_text_prefix(header, utf8_len);
    encoded = js_malloc(ctx, header_len + utf8_len);
    if (!encoded) {
        JS_FreeCString(ctx, utf8);
        JS_FreeValue(ctx, str);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }

    memcpy(encoded, header, header_len);
    memcpy(encoded + header_len, utf8, utf8_len);
    JS_FreeCString(ctx, utf8);
    JS_FreeValue(ctx, str);

    *out_bytes = encoded;
    *out_len = header_len + utf8_len;
    return 0;
}

static void js_det_json_free_key_entries(JSContext *ctx,
                                         JSDetJSONStringifyKeyEntry *entries,
                                         uint32_t count)
{
    if (!entries)
        return;
    for (uint32_t i = 0; i < count; i++) {
        js_free(ctx, entries[i].encoded_key);
    }
    js_free(ctx, entries);
}

static int js_det_json_compare_key_entries(JSContext *ctx,
                                           const JSDetJSONStringifyKeyEntry *a,
                                           const JSDetJSONStringifyKeyEntry *b,
                                           int *out_cmp)
{
    if (js_det_json_stringify_charge(ctx,
                                     JS_GAS_JSON_STRINGIFY_SORT_COMPARISON,
                                     FALSE, 0, 0, 0, 0, 1) != 0) {
        return -1;
    }
    *out_cmp = js_det_json_compare_encoded_keys_raw(a->encoded_key,
                                                    a->encoded_key_len,
                                                    b->encoded_key,
                                                    b->encoded_key_len);
    return 0;
}

static int js_det_json_sort_key_entries_range(JSContext *ctx,
                                              JSDetJSONStringifyKeyEntry *entries,
                                              JSDetJSONStringifyKeyEntry *tmp,
                                              uint32_t start,
                                              uint32_t end)
{
    uint32_t mid;
    uint32_t i, j, k;

    if (end - start <= 1)
        return 0;

    mid = start + (end - start) / 2;
    if (js_det_json_sort_key_entries_range(ctx, entries, tmp, start, mid) != 0)
        return -1;
    if (js_det_json_sort_key_entries_range(ctx, entries, tmp, mid, end) != 0)
        return -1;

    i = start;
    j = mid;
    k = start;
    while (i < mid && j < end) {
        int cmp;

        if (js_det_json_compare_key_entries(ctx, &entries[i], &entries[j], &cmp) != 0)
            return -1;
        if (cmp <= 0) {
            tmp[k++] = entries[i++];
        } else {
            tmp[k++] = entries[j++];
        }
    }
    while (i < mid)
        tmp[k++] = entries[i++];
    while (j < end)
        tmp[k++] = entries[j++];
    memcpy(entries + start, tmp + start, (end - start) * sizeof(*entries));
    return 0;
}

static int js_det_json_sort_key_entries(JSContext *ctx,
                                        JSDetJSONStringifyKeyEntry *entries,
                                        uint32_t count)
{
    JSDetJSONStringifyKeyEntry *tmp;
    int ret;

    if (count <= 1)
        return 0;
    tmp = js_malloc(ctx, sizeof(*tmp) * count);
    if (!tmp) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    ret = js_det_json_sort_key_entries_range(ctx, entries, tmp, 0, count);
    js_free(ctx, tmp);
    return ret;
}

static int js_det_json_stringify_get_own_data_property(JSContext *ctx,
                                                       JSValueConst obj,
                                                       JSAtom prop,
                                                       JSValue *out_value,
                                                       BOOL *out_present)
{
    JSPropertyDescriptor desc;
    int ret;

    ret = JS_GetOwnProperty(ctx, &desc, obj, prop);
    if (ret <= 0) {
        *out_present = FALSE;
        return ret;
    }
    if (desc.flags & JS_PROP_GETSET) {
        js_free_desc(ctx, &desc);
        JS_ThrowTypeError(ctx, "JSON.stringify does not support accessor properties");
        return -1;
    }

    *out_present = TRUE;
    *out_value = desc.value;
    desc.value = JS_UNDEFINED;
    js_free_desc(ctx, &desc);
    return 0;
}

static int js_det_json_stringify_array(JSContext *ctx,
                                       JSDetJSONStringifyState *state,
                                       JSValueConst value,
                                       uint32_t depth)
{
    int64_t len64;
    int64_t i;

    if (depth + 1 > JS_DV_LIMIT_DEFAULTS.max_depth) {
        JS_ThrowTypeError(ctx, "JSON.stringify maxDepth %u exceeded",
                          JS_DV_LIMIT_DEFAULTS.max_depth);
        return -1;
    }
    if (js_get_length64(ctx, &len64, value) != 0)
        return -1;
    if (len64 < 0 || (uint64_t)len64 > JS_DV_LIMIT_DEFAULTS.max_array_length) {
        JS_ThrowTypeError(ctx, "JSON.stringify array length exceeds maxArrayLength (%" PRIu64 " > %u)",
                          len64 < 0 ? 0 : (uint64_t)len64,
                          JS_DV_LIMIT_DEFAULTS.max_array_length);
        return -1;
    }
    if (js_det_json_stringify_stack_contains(state, value)) {
        JS_ThrowTypeError(ctx, "JSON.stringify does not support circular references");
        return -1;
    }
    if (js_det_json_stringify_stack_push(ctx, state, value) != 0)
        return -1;

    if (js_det_json_stringify_emit_ascii(ctx, state, "[", 1) != 0)
        goto fail;
    for (i = 0; i < len64; i++) {
        JSAtom atom = JS_ATOM_NULL;
        JSValue element;
        BOOL has_element;

        if (i > 0 && js_det_json_stringify_emit_ascii(ctx, state, ",", 1) != 0)
            goto fail;
        if (js_det_json_stringify_charge(ctx, JS_GAS_JSON_STRINGIFY_ARRAY_ELEMENT,
                                         FALSE, 0, 0, 0, 1, 0) != 0) {
            goto fail;
        }
        atom = JS_NewAtomUInt32(ctx, i);
        if (atom == JS_ATOM_NULL)
            goto fail;
        if (js_det_json_stringify_get_own_data_property(ctx, value, atom,
                                                        &element, &has_element) != 0) {
            JS_FreeAtom(ctx, atom);
            goto fail;
        }
        JS_FreeAtom(ctx, atom);
        if (!has_element) {
            if (js_det_json_stringify_value(ctx, state, JS_NULL, depth + 1) != 0)
                goto fail;
            continue;
        }
        if (js_det_json_stringify_value(ctx, state, element, depth + 1) != 0) {
            JS_FreeValue(ctx, element);
            goto fail;
        }
        JS_FreeValue(ctx, element);
    }
    if (js_det_json_stringify_emit_ascii(ctx, state, "]", 1) != 0)
        goto fail;

    js_det_json_stringify_stack_pop(state);
    return 0;

fail:
    js_det_json_stringify_stack_pop(state);
    return -1;
}

static int js_det_json_stringify_object(JSContext *ctx,
                                        JSDetJSONStringifyState *state,
                                        JSValueConst value,
                                        uint32_t depth)
{
    JSPropertyEnum *props = NULL;
    JSDetJSONStringifyKeyEntry *entries = NULL;
    uint32_t prop_len = 0;
    int ret;

    if (depth + 1 > JS_DV_LIMIT_DEFAULTS.max_depth) {
        JS_ThrowTypeError(ctx, "JSON.stringify maxDepth %u exceeded",
                          JS_DV_LIMIT_DEFAULTS.max_depth);
        return -1;
    }
    if (js_det_json_stringify_stack_contains(state, value)) {
        JS_ThrowTypeError(ctx, "JSON.stringify does not support circular references");
        return -1;
    }
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &prop_len,
                                       JS_VALUE_GET_OBJ(value),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) {
        return -1;
    }
    if (prop_len > JS_DV_LIMIT_DEFAULTS.max_map_length) {
        JS_FreePropertyEnum(ctx, props, prop_len);
        JS_ThrowTypeError(ctx, "JSON.stringify object entries exceed maxMapLength (%u > %u)",
                          prop_len, JS_DV_LIMIT_DEFAULTS.max_map_length);
        return -1;
    }
    if (prop_len > 0) {
        entries = js_mallocz(ctx, sizeof(*entries) * prop_len);
        if (!entries) {
            JS_FreePropertyEnum(ctx, props, prop_len);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (uint32_t i = 0; i < prop_len; i++) {
            JSValue key = JS_AtomToString(ctx, props[i].atom);
            if (JS_IsException(key)) {
                JS_FreePropertyEnum(ctx, props, prop_len);
                js_det_json_free_key_entries(ctx, entries, prop_len);
                return -1;
            }
            entries[i].atom = props[i].atom;
            ret = js_det_json_build_encoded_key(ctx, key,
                                                &entries[i].encoded_key,
                                                &entries[i].encoded_key_len);
            JS_FreeValue(ctx, key);
            if (ret != 0) {
                JS_FreePropertyEnum(ctx, props, prop_len);
                js_det_json_free_key_entries(ctx, entries, prop_len);
                return -1;
            }
        }
        if (js_det_json_sort_key_entries(ctx, entries, prop_len) != 0) {
            JS_FreePropertyEnum(ctx, props, prop_len);
            js_det_json_free_key_entries(ctx, entries, prop_len);
            return -1;
        }
    }
    if (js_det_json_stringify_stack_push(ctx, state, value) != 0) {
        JS_FreePropertyEnum(ctx, props, prop_len);
        js_det_json_free_key_entries(ctx, entries, prop_len);
        return -1;
    }

    if (js_det_json_stringify_emit_ascii(ctx, state, "{", 1) != 0)
        goto fail;
    for (uint32_t i = 0; i < prop_len; i++) {
        JSValue key = JS_UNDEFINED;
        JSValue element = JS_UNDEFINED;
        BOOL has_element;

        if (i > 0 && js_det_json_stringify_emit_ascii(ctx, state, ",", 1) != 0)
            goto element_fail;
        if (js_det_json_stringify_charge(ctx, JS_GAS_JSON_STRINGIFY_OBJECT_ENTRY,
                                         FALSE, 0, 0, 1, 0, 0) != 0) {
            goto element_fail;
        }
        key = JS_AtomToString(ctx, entries[i].atom);
        if (JS_IsException(key))
            goto element_fail;
        if (js_det_json_stringify_quote(ctx, state, key) != 0)
            goto element_fail;
        if (js_det_json_stringify_emit_ascii(ctx, state, ":", 1) != 0)
            goto element_fail;
        if (js_det_json_stringify_get_own_data_property(ctx, value, entries[i].atom,
                                                        &element, &has_element) != 0) {
            goto element_fail;
        }
        if (!has_element) {
            JS_ThrowTypeError(ctx, "JSON.stringify object property disappeared during serialization");
            goto element_fail;
        }
        if (js_det_json_stringify_value(ctx, state, element, depth + 1) != 0)
            goto element_fail;
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, element);
        continue;

element_fail:
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, element);
        goto fail;
    }
    if (js_det_json_stringify_emit_ascii(ctx, state, "}", 1) != 0)
        goto fail;

    js_det_json_stringify_stack_pop(state);
    JS_FreePropertyEnum(ctx, props, prop_len);
    js_det_json_free_key_entries(ctx, entries, prop_len);
    return 0;

fail:
    js_det_json_stringify_stack_pop(state);
    JS_FreePropertyEnum(ctx, props, prop_len);
    js_det_json_free_key_entries(ctx, entries, prop_len);
    return -1;
}

static int js_det_json_stringify_value(JSContext *ctx,
                                       JSDetJSONStringifyState *state,
                                       JSValueConst value,
                                       uint32_t depth)
{
    int is_plain_object;
    int is_array;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return -1;
    }
    if (js_det_json_stringify_charge(ctx, JS_GAS_JSON_STRINGIFY_VALUE,
                                     FALSE, 0, 1, 0, 0, 0) != 0) {
        return -1;
    }

    switch (JS_VALUE_GET_NORM_TAG(value)) {
    case JS_TAG_NULL:
    case JS_TAG_BOOL:
    case JS_TAG_INT:
        return js_det_json_stringify_emit_primitive(ctx, state,
                                                    JS_DupValue(ctx, value));
    case JS_TAG_FLOAT64:
        if (!isfinite(JS_VALUE_GET_FLOAT64(value))) {
            JS_ThrowTypeError(ctx, "JSON.stringify only supports finite numbers");
            return -1;
        }
        return js_det_json_stringify_emit_primitive(ctx, state,
                                                    JS_DupValue(ctx, value));
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return js_det_json_stringify_quote(ctx, state, value);
    case JS_TAG_OBJECT:
        break;
    default:
        JS_ThrowTypeError(ctx, "JSON.stringify only supports null, booleans, strings, finite numbers, arrays, and plain objects");
        return -1;
    }

    if (JS_IsFunction(ctx, value)) {
        JS_ThrowTypeError(ctx, "JSON.stringify only supports null, booleans, strings, finite numbers, arrays, and plain objects");
        return -1;
    }

    is_plain_object = js_det_json_is_plain_object(ctx, value);
    if (is_plain_object < 0)
        return -1;
    is_array = JS_IsArray(ctx, value);
    if (is_array < 0)
        return -1;
    if (is_array) {
        return js_det_json_stringify_array(ctx, state, value, depth);
    }
    if (is_plain_object) {
        return js_det_json_stringify_object(ctx, state, value, depth);
    }

    JS_ThrowTypeError(ctx, "JSON.stringify only supports null, booleans, strings, finite numbers, arrays, and plain objects");
    return -1;
}

static JSValue js_deterministic_json_parse(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    JSParseState s1, *s = &s1;
    JSValue value;
    const char *str;
    size_t len;
    uint64_t input_gas;

    if (js_det_json_parse_charge(ctx, JS_GAS_JSON_PARSE_BASE,
                                 TRUE, 0, 0, 0, 0) != 0) {
        return JS_EXCEPTION;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        return JS_ThrowTypeError(ctx, "JSON.parse reviver is not supported in deterministic mode");
    }

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (len > JS_DV_LIMIT_DEFAULTS.max_encoded_bytes) {
        JS_FreeCString(ctx, str);
        return JS_ThrowTypeError(ctx, "JSON.parse input exceeds maxInputBytes (%zu > %u)",
                                 len, JS_DV_LIMIT_DEFAULTS.max_encoded_bytes);
    }
    if (js_det_json_mul_u64(&input_gas, JS_GAS_JSON_PARSE_INPUT_BYTE, len) != 0) {
        JS_FreeCString(ctx, str);
        return JS_ThrowTypeError(ctx, "JSON.parse gas overflow");
    }
    if (js_det_json_parse_charge(ctx, input_gas, FALSE, len, 0, 0, 0) != 0) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    js_parse_init(ctx, s, str, len, "<input>");
    s->ext_json = FALSE;
    s->det_json_max_string_bytes = JS_DV_LIMIT_DEFAULTS.max_string_bytes;
    s->det_json_string_label = "JSON.parse string";
    s->det_json_skip_string_materialization = TRUE;
    if (json_next_token(s) != 0) {
        JS_FreeCString(ctx, str);
        free_token(s, &s->token);
        return JS_EXCEPTION;
    }
    if (js_det_json_parse_preflight_value(s, 0) != 0) {
        JS_FreeCString(ctx, str);
        free_token(s, &s->token);
        return JS_EXCEPTION;
    }
    if (s->token.val != TOK_EOF) {
        js_parse_error(s, "unexpected data at the end");
        JS_FreeCString(ctx, str);
        free_token(s, &s->token);
        return JS_EXCEPTION;
    }
    free_token(s, &s->token);

    value = JS_ParseJSON(ctx, str, len, "<input>");
    JS_FreeCString(ctx, str);
    if (JS_IsException(value))
        return value;
    return value;
}

static JSValue js_deterministic_json_stringify(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc,
                                               JSValueConst *argv)
{
    JSDetJSONStringifyState state_s, *state = &state_s;
    StringBuffer b_s;
    JSValue ret;

    if (js_det_json_stringify_charge(ctx, JS_GAS_JSON_STRINGIFY_BASE,
                                     TRUE, 0, 0, 0, 0, 0) != 0) {
        return JS_EXCEPTION;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        return JS_ThrowTypeError(ctx, "JSON.stringify replacer is not supported in deterministic mode");
    }
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        return JS_ThrowTypeError(ctx, "JSON.stringify space is not supported in deterministic mode");
    }

    state->stack = NULL;
    state->stack_len = 0;
    state->stack_cap = 0;
    state->output_bytes = 0;

    if (string_buffer_init(ctx, &b_s, 32) != 0) {
        js_free(ctx, state->stack);
        return JS_EXCEPTION;
    }
    state->b = &b_s;

    if (js_det_json_stringify_value(ctx, state, argv[0], 0) != 0) {
        ret = JS_EXCEPTION;
        string_buffer_free(&b_s);
        js_free(ctx, state->stack);
        return ret;
    }

    ret = string_buffer_end(&b_s);
    js_free(ctx, state->stack);
    return ret;
}

int js_deterministic_install_json(JSContext *ctx)
{
    JSValue global;
    JSValue json;
    JSValue parse_fn;
    JSValue stringify_fn;
    int ret;

    global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global))
        return -1;

    json = JS_GetPropertyStr(ctx, global, "JSON");
    JS_FreeValue(ctx, global);
    if (JS_IsException(json))
        return -1;
    if (!JS_IsObject(json)) {
        JS_FreeValue(ctx, json);
        return -1;
    }

    parse_fn = JS_NewCFunction(ctx, js_deterministic_json_parse, "parse", 2);
    if (JS_IsException(parse_fn)) {
        JS_FreeValue(ctx, json);
        return -1;
    }
    stringify_fn = JS_NewCFunction(ctx, js_deterministic_json_stringify,
                                   "stringify", 3);
    if (JS_IsException(stringify_fn)) {
        JS_FreeValue(ctx, parse_fn);
        JS_FreeValue(ctx, json);
        return -1;
    }

    ret = JS_DefinePropertyValueStr(ctx, json, "parse", JS_DupValue(ctx, parse_fn),
                                    JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                                        JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE);
    if (ret < 0)
        goto fail;

    ret = JS_DefinePropertyValueStr(ctx, json, "stringify", JS_DupValue(ctx, stringify_fn),
                                    JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                                        JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE);
    if (ret < 0)
        goto fail;

    JS_FreeValue(ctx, parse_fn);
    JS_FreeValue(ctx, stringify_fn);
    JS_FreeValue(ctx, json);
    return 0;

fail:
    JS_FreeValue(ctx, parse_fn);
    JS_FreeValue(ctx, stringify_fn);
    JS_FreeValue(ctx, json);
    return -1;
}
