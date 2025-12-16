#include "cutils.h"
#include "quickjs-host.h"
#include "quickjs-internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define JS_HOST_ERROR_CODE_TRANSPORT "HOST_TRANSPORT"
#define JS_HOST_ERROR_TAG_TRANSPORT "host/transport"
#define JS_HOST_ERROR_CODE_ENVELOPE_INVALID "HOST_ENVELOPE_INVALID"
#define JS_HOST_ERROR_TAG_ENVELOPE_INVALID "host/envelope_invalid"

static JSValue js_throw_host_error_str(JSContext *ctx, const char *code, const char *tag, JSValueConst details)
{
    JSValue ret;
    JSAtom code_atom = JS_NewAtom(ctx, code);
    JSAtom tag_atom = JS_NewAtom(ctx, tag);

    if (code_atom == JS_ATOM_NULL || tag_atom == JS_ATOM_NULL) {
        if (code_atom != JS_ATOM_NULL)
            JS_FreeAtom(ctx, code_atom);
        if (tag_atom != JS_ATOM_NULL)
            JS_FreeAtom(ctx, tag_atom);
        return JS_EXCEPTION;
    }

    ret = JS_ThrowHostError(ctx, code_atom, tag_atom, details);
    JS_FreeAtom(ctx, code_atom);
    JS_FreeAtom(ctx, tag_atom);
    return ret;
}

JSValue JS_ThrowHostError(JSContext *ctx, JSAtom code_atom, JSAtom tag_atom, JSValueConst details)
{
    JSValue obj, name, msg, code_val, tag_val, details_val;

    obj = JS_NewError(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;

    name = JS_NewString(ctx, "HostError");
    msg = JS_AtomToString(ctx, tag_atom);
    code_val = JS_AtomToString(ctx, code_atom);
    tag_val = JS_DupValue(ctx, msg);
    details_val = JS_DupValue(ctx, details);

    if (JS_IsException(name) || JS_IsException(msg) || JS_IsException(code_val) ||
        JS_IsException(tag_val) || JS_IsException(details_val)) {
        if (!JS_IsException(name))
            JS_FreeValue(ctx, name);
        if (!JS_IsException(msg))
            JS_FreeValue(ctx, msg);
        if (!JS_IsException(code_val))
            JS_FreeValue(ctx, code_val);
        if (!JS_IsException(tag_val))
            JS_FreeValue(ctx, tag_val);
        if (!JS_IsException(details_val))
            JS_FreeValue(ctx, details_val);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    JS_DefinePropertyValueStr(ctx, obj, "name", name,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, obj, "message", msg,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, obj, "code", code_val,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, obj, "tag", tag_val,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, obj, "details", details_val,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_Throw(ctx, obj);
    return JS_EXCEPTION;
}

JSValue JS_ThrowHostTransportError(JSContext *ctx)
{
    return js_throw_host_error_str(ctx,
                                   JS_HOST_ERROR_CODE_TRANSPORT,
                                   JS_HOST_ERROR_TAG_TRANSPORT,
                                   JS_UNDEFINED);
}

static JSValue js_throw_host_envelope_invalid(JSContext *ctx)
{
    return js_throw_host_error_str(ctx,
                                   JS_HOST_ERROR_CODE_ENVELOPE_INVALID,
                                   JS_HOST_ERROR_TAG_ENVELOPE_INVALID,
                                   JS_UNDEFINED);
}

void JS_FreeHostResponse(JSContext *ctx, JSHostResponse *resp)
{
    if (!ctx || !resp)
        return;

    if (!JS_IsUndefined(resp->ok))
        JS_FreeValue(ctx, resp->ok);
    if (!JS_IsUndefined(resp->err_details))
        JS_FreeValue(ctx, resp->err_details);
    if (resp->err_code_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, resp->err_code_atom);
    if (resp->err_tag_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, resp->err_tag_atom);

    resp->is_error = 0;
    resp->units = 0;
    resp->ok = JS_UNDEFINED;
    resp->err_details = JS_UNDEFINED;
    resp->err_code_atom = JS_ATOM_NULL;
    resp->err_tag_atom = JS_ATOM_NULL;
}

int JS_ParseHostResponse(JSContext *ctx,
                         const uint8_t *data,
                         size_t length,
                         const JSHostResponseValidation *validation,
                         JSHostResponse *out)
{
    JSHostResponse tmp;
    JSValue envelope = JS_UNDEFINED;
    JSValue ok_val = JS_UNDEFINED;
    JSValue err_val = JS_UNDEFINED;
    JSValue units_val = JS_UNDEFINED;
    JSValue err_code_val = JS_UNDEFINED;
    JSValue err_details_val = JS_UNDEFINED;
    JSPropertyEnum *props = NULL;
    JSPropertyEnum *err_props = NULL;
    uint32_t props_len = 0;
    uint32_t err_props_len = 0;
    JSAtom ok_atom = JS_ATOM_NULL;
    JSAtom err_atom = JS_ATOM_NULL;
    JSAtom units_atom = JS_ATOM_NULL;
    JSAtom code_atom = JS_ATOM_NULL;
    JSAtom details_atom = JS_ATOM_NULL;
    BOOL has_pending_exception = FALSE;
    int ret = -1;

    if (!ctx || !validation || !out)
        return -1;

    if (validation->error_count > 0 && !validation->errors) {
        JS_ThrowTypeError(ctx, "host response errors table is required");
        return -1;
    }

    tmp.is_error = 0;
    tmp.units = 0;
    tmp.ok = JS_UNDEFINED;
    tmp.err_code_atom = JS_ATOM_NULL;
    tmp.err_tag_atom = JS_ATOM_NULL;
    tmp.err_details = JS_UNDEFINED;

    if (!data && length > 0) {
        js_throw_host_envelope_invalid(ctx);
        return -1;
    }

    if (length > JS_DV_LIMIT_DEFAULTS.max_encoded_bytes) {
        js_throw_host_envelope_invalid(ctx);
        return -1;
    }

    envelope = JS_DecodeDV(ctx, data, length, &JS_DV_LIMIT_DEFAULTS);
    if (JS_IsException(envelope)) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    if (!JS_IsObject(envelope))
        goto envelope_invalid;

    ok_atom = JS_NewAtom(ctx, "ok");
    err_atom = JS_NewAtom(ctx, "err");
    units_atom = JS_NewAtom(ctx, "units");
    code_atom = JS_NewAtom(ctx, "code");
    details_atom = JS_NewAtom(ctx, "details");
    if (ok_atom == JS_ATOM_NULL || err_atom == JS_ATOM_NULL || units_atom == JS_ATOM_NULL ||
        code_atom == JS_ATOM_NULL || details_atom == JS_ATOM_NULL) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    if (JS_GetOwnPropertyNames(ctx, &props, &props_len, envelope,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    for (uint32_t i = 0; i < props_len; i++) {
        JSAtom atom = props[i].atom;
        if (atom != ok_atom && atom != err_atom && atom != units_atom) {
            goto envelope_invalid;
        }
    }

    ok_val = JS_GetProperty(ctx, envelope, ok_atom);
    if (JS_IsException(ok_val)) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    err_val = JS_GetProperty(ctx, envelope, err_atom);
    if (JS_IsException(err_val)) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    units_val = JS_GetProperty(ctx, envelope, units_atom);
    if (JS_IsException(units_val)) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    BOOL has_ok = !JS_IsUndefined(ok_val);
    BOOL has_err = !JS_IsUndefined(err_val);

    if ((has_ok && has_err) || (!has_ok && !has_err))
        goto envelope_invalid;

    if (JS_IsUndefined(units_val))
        goto envelope_invalid;

    if (!JS_IsNumber(units_val))
        goto envelope_invalid;

    double units_d;
    if (JS_ToFloat64(ctx, &units_d, units_val)) {
        has_pending_exception = TRUE;
        goto envelope_invalid;
    }

    if (!isfinite(units_d))
        goto envelope_invalid;

    if ((units_d == 0.0 && signbit(units_d)) || units_d < 0.0 ||
        units_d > (double)UINT32_MAX || units_d > (double)validation->max_units)
        goto envelope_invalid;

    if (floor(units_d) != units_d)
        goto envelope_invalid;

    tmp.units = (uint32_t)units_d;

    if (has_ok) {
        tmp.is_error = 0;
        tmp.ok = ok_val;
        ok_val = JS_UNDEFINED;
    } else {
        if (!JS_IsObject(err_val))
            goto envelope_invalid;

        if (JS_GetOwnPropertyNames(ctx, &err_props, &err_props_len, err_val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            has_pending_exception = TRUE;
            goto envelope_invalid;
        }

        for (uint32_t i = 0; i < err_props_len; i++) {
            JSAtom atom = err_props[i].atom;
            if (atom != code_atom && atom != details_atom) {
                goto envelope_invalid;
            }
        }

        err_code_val = JS_GetProperty(ctx, err_val, code_atom);
        if (JS_IsException(err_code_val)) {
            has_pending_exception = TRUE;
            goto envelope_invalid;
        }

        if (JS_IsUndefined(err_code_val))
            goto envelope_invalid;

        if (!JS_IsString(err_code_val))
            goto envelope_invalid;

        const char *code_str = JS_ToCString(ctx, err_code_val);
        if (!code_str) {
            has_pending_exception = TRUE;
            goto envelope_invalid;
        }

        JSAtom code_atom_val = JS_NewAtom(ctx, code_str);
        JS_FreeCString(ctx, code_str);
        if (code_atom_val == JS_ATOM_NULL) {
            has_pending_exception = TRUE;
            goto envelope_invalid;
        }

        JSAtom tag_atom_val = JS_ATOM_NULL;
        for (size_t i = 0; i < validation->error_count; i++) {
            if (validation->errors[i].code_atom == code_atom_val) {
                tag_atom_val = validation->errors[i].tag_atom;
                break;
            }
        }

        if (tag_atom_val == JS_ATOM_NULL) {
            JS_FreeAtom(ctx, code_atom_val);
            goto envelope_invalid;
        }

        tmp.is_error = 1;
        tmp.err_code_atom = code_atom_val;
        tmp.err_tag_atom = JS_DupAtom(ctx, tag_atom_val);
        if (tmp.err_tag_atom == JS_ATOM_NULL) {
            has_pending_exception = TRUE;
            goto envelope_invalid;
        }

        err_details_val = JS_GetProperty(ctx, err_val, details_atom);
        if (JS_IsException(err_details_val)) {
            has_pending_exception = TRUE;
            goto envelope_invalid;
        }

        if (!JS_IsUndefined(err_details_val)) {
            tmp.err_details = err_details_val;
            err_details_val = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, err_code_val);
        err_code_val = JS_UNDEFINED;
    }

    ret = 0;
    *out = tmp;
    tmp.ok = JS_UNDEFINED;
    tmp.err_details = JS_UNDEFINED;
    tmp.err_code_atom = JS_ATOM_NULL;
    tmp.err_tag_atom = JS_ATOM_NULL;
    goto done;

envelope_invalid:
    if (has_pending_exception) {
        JSValue pending = JS_GetException(ctx);
        if (!JS_IsException(pending)) {
            BOOL is_out_of_gas = FALSE;

            if (JS_IsError(ctx, pending)) {
                JSValue code_val = JS_GetPropertyStr(ctx, pending, "code");
                if (JS_IsException(code_val)) {
                    JSValue exc2 = JS_GetException(ctx);
                    if (!JS_IsException(exc2))
                        JS_FreeValue(ctx, exc2);
                    JS_FreeValue(ctx, code_val);
                } else {
                    const char *code_str = JS_ToCString(ctx, code_val);
                    if (code_str) {
                        if (strcmp(code_str, "OOG") == 0)
                            is_out_of_gas = TRUE;
                        JS_FreeCString(ctx, code_str);
                    }
                    JS_FreeValue(ctx, code_val);
                }
            }

            if (is_out_of_gas) {
                JS_Throw(ctx, pending);
                JS_SetUncatchableException(ctx, TRUE);
                ret = -1;
                JS_FreeHostResponse(ctx, &tmp);
                goto done;
            }
            JS_FreeValue(ctx, pending);
        }
    }

    js_throw_host_envelope_invalid(ctx);
    ret = -1;
    JS_FreeHostResponse(ctx, &tmp);

done:
    if (props)
        JS_FreePropertyEnum(ctx, props, props_len);
    if (err_props)
        JS_FreePropertyEnum(ctx, err_props, err_props_len);
    if (ok_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, ok_atom);
    if (err_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, err_atom);
    if (units_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, units_atom);
    if (code_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, code_atom);
    if (details_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, details_atom);
    if (!JS_IsUndefined(envelope))
        JS_FreeValue(ctx, envelope);
    if (!JS_IsUndefined(ok_val))
        JS_FreeValue(ctx, ok_val);
    if (!JS_IsUndefined(err_val))
        JS_FreeValue(ctx, err_val);
    if (!JS_IsUndefined(units_val))
        JS_FreeValue(ctx, units_val);
    if (!JS_IsUndefined(err_code_val))
        JS_FreeValue(ctx, err_code_val);
    if (!JS_IsUndefined(err_details_val))
        JS_FreeValue(ctx, err_details_val);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Host manifest parsing and Host.v1 generation (T-040) */

extern void *js_malloc(JSContext *ctx, size_t size);
extern void js_free(JSContext *ctx, void *ptr);
extern void *js_realloc(JSContext *ctx, void *ptr, size_t size);

typedef enum {
    JS_HOST_SCHEMA_STRING,
    JS_HOST_SCHEMA_DV,
    JS_HOST_SCHEMA_NULL,
} JSHostSchemaType;

typedef enum {
    JS_HOST_EFFECT_READ,
    JS_HOST_EFFECT_EMIT,
    JS_HOST_EFFECT_MUTATE,
} JSHostEffect;

typedef struct {
    JSHostSchemaType type;
    uint32_t utf8_max; /* 0 when not provided */
} JSHostArgDef;

typedef struct {
    uint32_t fn_id;
    size_t path_len;
    char **path_segments;
    char *name;
    uint32_t arity;
    JSHostEffect effect;
    JSHostArgDef *args;
    JSHostSchemaType return_type;
    uint32_t gas_base;
    uint32_t gas_k_arg_bytes;
    uint32_t gas_k_ret_bytes;
    uint32_t gas_k_units;
    char *gas_schedule_id;
    uint32_t max_request_bytes;
    uint32_t max_response_bytes;
    uint32_t max_units;
    JSHostErrorEntry *errors;
    size_t error_count;
} JSHostFunctionDef;

struct JSHostManifest {
    JSHostFunctionDef *functions;
    size_t function_count;
};

typedef struct JSHostManifestNode {
    JSContext *ctx;
    JSHostManifest manifest;
    struct JSHostManifestNode *next;
} JSHostManifestNode;

static JSHostManifestNode *js_host_manifest_list = NULL;

static JSHostManifest *js_host_find_manifest(JSContext *ctx)
{
    JSHostManifestNode *node = js_host_manifest_list;
    while (node) {
        if (node->ctx == ctx)
            return &node->manifest;
        node = node->next;
    }
    return NULL;
}

static void js_host_free_function(JSContext *ctx, JSHostFunctionDef *fn)
{
    if (!fn)
        return;

    if (fn->path_segments) {
        for (size_t i = 0; i < fn->path_len; i++) {
            if (fn->path_segments[i])
                js_free(ctx, fn->path_segments[i]);
        }
        js_free(ctx, fn->path_segments);
    }

    if (fn->args)
        js_free(ctx, fn->args);

    if (fn->errors) {
        for (size_t i = 0; i < fn->error_count; i++) {
            if (fn->errors[i].code_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, fn->errors[i].code_atom);
            if (fn->errors[i].tag_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, fn->errors[i].tag_atom);
        }
        js_free(ctx, fn->errors);
    }

    if (fn->gas_schedule_id)
        js_free(ctx, fn->gas_schedule_id);

    if (fn->name)
        js_free(ctx, fn->name);

    memset(fn, 0, sizeof(*fn));
}

static void js_host_manifest_clear(JSContext *ctx, JSHostManifest *manifest)
{
    if (!manifest)
        return;

    if (manifest->functions) {
        for (size_t i = 0; i < manifest->function_count; i++)
            js_host_free_function(ctx, &manifest->functions[i]);
        js_free(ctx, manifest->functions);
    }
    manifest->functions = NULL;
    manifest->function_count = 0;
}

void JS_FreeHostManifest(JSContext *ctx)
{
    JSHostManifestNode *prev = NULL;
    JSHostManifestNode *node = js_host_manifest_list;

    while (node) {
        if (node->ctx == ctx) {
            if (prev)
                prev->next = node->next;
            else
                js_host_manifest_list = node->next;

            js_host_manifest_clear(ctx, &node->manifest);
            js_free(ctx, node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

static int js_host_manifest_error(JSContext *ctx, const char *path, const char *message)
{
    if (path && message)
        JS_ThrowTypeError(ctx, "abi manifest invalid: %s (%s)", message, path);
    else if (message)
        JS_ThrowTypeError(ctx, "abi manifest invalid: %s", message);
    else
        JS_ThrowTypeError(ctx, "abi manifest invalid");
    return -1;
}

static int js_host_expect_object(JSContext *ctx, JSValueConst val, const char *path)
{
    if (!JS_IsObject(val) || JS_IsArray(ctx, val))
        return js_host_manifest_error(ctx, path, "expected object");
    return 0;
}

static int js_host_check_keys(JSContext *ctx,
                              JSValueConst obj,
                              const char **required,
                              size_t required_len,
                              const char **optional,
                              size_t optional_len,
                              const char *path)
{
    JSPropertyEnum *props = NULL;
    uint32_t props_len = 0;
    uint8_t *seen = NULL;
    int ret = -1;

    if (JS_GetOwnPropertyNames(ctx, &props, &props_len, obj,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;

    if (required_len > 0) {
        seen = js_malloc(ctx, required_len);
        if (!seen) {
            JS_FreePropertyEnum(ctx, props, props_len);
            return -1;
        }
        memset(seen, 0, required_len);
    }

    for (uint32_t i = 0; i < props_len; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        BOOL allowed = FALSE;

        if (!name)
            goto cleanup;

        for (size_t j = 0; j < required_len; j++) {
            if (strcmp(name, required[j]) == 0) {
                allowed = TRUE;
                if (seen)
                    seen[j] = 1;
                break;
            }
        }

        if (!allowed) {
            for (size_t j = 0; j < optional_len; j++) {
                if (strcmp(name, optional[j]) == 0) {
                    allowed = TRUE;
                    break;
                }
            }
        }

        JS_FreeCString(ctx, name);

        if (!allowed) {
            js_host_manifest_error(ctx, path, "unknown field");
            goto cleanup;
        }
    }

    for (size_t j = 0; j < required_len; j++) {
        if (seen && !seen[j]) {
            js_host_manifest_error(ctx, path, "missing required field");
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    if (props)
        JS_FreePropertyEnum(ctx, props, props_len);
    if (seen)
        js_free(ctx, seen);
    return ret;
}

static int js_host_copy_non_empty_string(JSContext *ctx,
                                         JSValueConst val,
                                         const char *path,
                                         char **out)
{
    const char *tmp;
    char *copy;

    if (!JS_IsString(val))
        return js_host_manifest_error(ctx, path, "expected string");

    tmp = JS_ToCString(ctx, val);
    if (!tmp)
        return -1;

    if (tmp[0] == '\0') {
        JS_FreeCString(ctx, tmp);
        return js_host_manifest_error(ctx, path, "string must be non-empty");
    }

    copy = js_malloc(ctx, strlen(tmp) + 1);
    if (!copy) {
        JS_FreeCString(ctx, tmp);
        return -1;
    }

    strcpy(copy, tmp);
    JS_FreeCString(ctx, tmp);
    *out = copy;
    return 0;
}

static int js_host_validate_uint32(JSContext *ctx,
                                   JSValueConst val,
                                   const char *path,
                                   uint32_t min,
                                   uint32_t max,
                                   uint32_t *out)
{
    double d;

    if (!JS_IsNumber(val))
        return js_host_manifest_error(ctx, path, "expected integer");

    if (JS_ToFloat64(ctx, &d, val))
        return -1;

    if (!isfinite(d) || floor(d) != d || (d == 0.0 && signbit(d)) || d < (double)min || d > (double)max)
        return js_host_manifest_error(ctx, path, "value out of range");

    *out = (uint32_t)d;
    return 0;
}

static int js_host_array_length(JSContext *ctx, JSValueConst arr, size_t *out_len)
{
    JSValue len_val = JS_UNDEFINED;
    uint32_t len32 = 0;
    int ret = -1;

    len_val = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(len_val))
        return -1;

    if (JS_ToUint32(ctx, &len32, len_val)) {
        JS_FreeValue(ctx, len_val);
        return -1;
    }

    JS_FreeValue(ctx, len_val);
    *out_len = (size_t)len32;
    ret = 0;
    return ret;
}

static int js_host_validate_schema(JSContext *ctx,
                                   JSValueConst schema_val,
                                   const char *path,
                                   JSHostSchemaType *out)
{
    const char *required[] = {"type"};
    JSValue type_val = JS_UNDEFINED;
    const char *type_str;
    int ret = -1;

    if (js_host_expect_object(ctx, schema_val, path))
        return -1;

    if (js_host_check_keys(ctx, schema_val, required, 1, NULL, 0, path))
        return -1;

    type_val = JS_GetPropertyStr(ctx, schema_val, "type");
    if (JS_IsException(type_val))
        goto done;

    if (!JS_IsString(type_val)) {
        js_host_manifest_error(ctx, path, "schema.type must be a string");
        goto done;
    }

    type_str = JS_ToCString(ctx, type_val);
    if (!type_str)
        goto done;

    if (strcmp(type_str, "string") == 0) {
        *out = JS_HOST_SCHEMA_STRING;
    } else if (strcmp(type_str, "dv") == 0) {
        *out = JS_HOST_SCHEMA_DV;
    } else if (strcmp(type_str, "null") == 0) {
        *out = JS_HOST_SCHEMA_NULL;
    } else {
        js_host_manifest_error(ctx, path, "unsupported schema type");
        JS_FreeCString(ctx, type_str);
        goto done;
    }

    JS_FreeCString(ctx, type_str);
    ret = 0;

done:
    if (!JS_IsUndefined(type_val))
        JS_FreeValue(ctx, type_val);
    return ret;
}

static int js_host_validate_js_path(JSContext *ctx,
                                    JSValueConst path_val,
                                    const char *path_desc,
                                    JSHostFunctionDef *out_fn)
{
    size_t len = 0;
    char **segments = NULL;
    JSValue entry = JS_UNDEFINED;
    int ret = -1;

    if (!JS_IsArray(ctx, path_val))
        return js_host_manifest_error(ctx, path_desc, "js_path must be an array");

    if (js_host_array_length(ctx, path_val, &len))
        return -1;

    if (len == 0)
        return js_host_manifest_error(ctx, path_desc, "js_path must contain at least one segment");

    segments = js_malloc(ctx, sizeof(char *) * len);
    if (!segments)
        return -1;
    memset(segments, 0, sizeof(char *) * len);

    for (size_t i = 0; i < len; i++) {
        char key_buf[64];
        snprintf(key_buf, sizeof(key_buf), "%s[%zu]", path_desc, i);

        entry = JS_GetPropertyUint32(ctx, path_val, (uint32_t)i);
        if (JS_IsException(entry))
            goto cleanup;

        if (js_host_copy_non_empty_string(ctx, entry, key_buf, &segments[i]))
            goto cleanup;

        JS_FreeValue(ctx, entry);
        entry = JS_UNDEFINED;

        const char *seg = segments[i];
        for (const char *p = seg; *p; p++) {
            char c = *p;
            if (!((c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-')) {
                js_host_manifest_error(ctx, key_buf, "js_path segment must match [A-Za-z0-9_-]+");
                goto cleanup;
            }
        }

        if (strcmp(seg, "__proto__") == 0 || strcmp(seg, "prototype") == 0 || strcmp(seg, "constructor") == 0) {
            js_host_manifest_error(ctx, key_buf, "js_path segment is forbidden");
            goto cleanup;
        }
    }

    out_fn->path_segments = segments;
    out_fn->path_len = len;
    ret = 0;

cleanup:
    if (ret != 0 && segments) {
        for (size_t i = 0; i < len; i++) {
            if (segments[i])
                js_free(ctx, segments[i]);
        }
        js_free(ctx, segments);
    }
    if (!JS_IsUndefined(entry))
        JS_FreeValue(ctx, entry);
    return ret;
}

static int js_host_validate_error_codes(JSContext *ctx,
                                        JSValueConst errors_val,
                                        const char *path,
                                        JSHostFunctionDef *out_fn)
{
    JSValue entry = JS_UNDEFINED;
    JSValue code_val = JS_UNDEFINED;
    JSValue tag_val = JS_UNDEFINED;
    char *code_str = NULL;
    char *tag_str = NULL;
    int ret = -1;

    if (!JS_IsArray(ctx, errors_val))
        return js_host_manifest_error(ctx, path, "error_codes must be an array");

    if (js_host_array_length(ctx, errors_val, &out_fn->error_count))
        return -1;

    if (out_fn->error_count == 0) {
        out_fn->errors = NULL;
        return 0;
    }

    out_fn->errors = js_malloc(ctx, sizeof(JSHostErrorEntry) * out_fn->error_count);
    if (!out_fn->errors)
        return -1;
    memset(out_fn->errors, 0, sizeof(JSHostErrorEntry) * out_fn->error_count);

    for (size_t i = 0; i < out_fn->error_count; i++) {
        char entry_path[96];
        snprintf(entry_path, sizeof(entry_path), "%s[%zu]", path, i);

        entry = JS_GetPropertyUint32(ctx, errors_val, (uint32_t)i);
        if (JS_IsException(entry))
            goto cleanup;

        const char *required[] = {"code", "tag"};
        if (js_host_expect_object(ctx, entry, entry_path) ||
            js_host_check_keys(ctx, entry, required, 2, NULL, 0, entry_path))
            goto cleanup;

        code_val = JS_GetPropertyStr(ctx, entry, "code");
        if (JS_IsException(code_val))
            goto cleanup;
        if (js_host_copy_non_empty_string(ctx, code_val, entry_path, &code_str))
            goto cleanup;

        tag_val = JS_GetPropertyStr(ctx, entry, "tag");
        if (JS_IsException(tag_val))
            goto cleanup;
        if (js_host_copy_non_empty_string(ctx, tag_val, entry_path, &tag_str))
            goto cleanup;

        if (strcmp(code_str, JS_HOST_ERROR_CODE_TRANSPORT) == 0 ||
            strcmp(code_str, JS_HOST_ERROR_CODE_ENVELOPE_INVALID) == 0) {
            js_host_manifest_error(ctx, entry_path, "reserved error code");
            goto cleanup;
        }

        if (i > 0) {
            JSAtom prev_atom = out_fn->errors[i - 1].code_atom;
            const char *prev = JS_AtomToCString(ctx, prev_atom);
            if (!prev)
                goto cleanup;
            int cmp = strcmp(prev, code_str);
            JS_FreeCString(ctx, prev);
            if (cmp >= 0) {
                js_host_manifest_error(ctx, path, "error_codes must be sorted and unique");
                goto cleanup;
            }
        }

        out_fn->errors[i].code_atom = JS_NewAtom(ctx, code_str);
        out_fn->errors[i].tag_atom = JS_NewAtom(ctx, tag_str);
        if (out_fn->errors[i].code_atom == JS_ATOM_NULL ||
            out_fn->errors[i].tag_atom == JS_ATOM_NULL)
            goto cleanup;

        js_free(ctx, code_str);
        js_free(ctx, tag_str);
        code_str = NULL;
        tag_str = NULL;

        JS_FreeValue(ctx, entry);
        JS_FreeValue(ctx, code_val);
        JS_FreeValue(ctx, tag_val);
        entry = code_val = tag_val = JS_UNDEFINED;
    }

    ret = 0;

cleanup:
    if (code_str)
        js_free(ctx, code_str);
    if (tag_str)
        js_free(ctx, tag_str);
    if (!JS_IsUndefined(entry))
        JS_FreeValue(ctx, entry);
    if (!JS_IsUndefined(code_val))
        JS_FreeValue(ctx, code_val);
    if (!JS_IsUndefined(tag_val))
        JS_FreeValue(ctx, tag_val);
    if (ret != 0 && out_fn->errors) {
        for (size_t i = 0; i < out_fn->error_count; i++) {
            if (out_fn->errors[i].code_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, out_fn->errors[i].code_atom);
            if (out_fn->errors[i].tag_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, out_fn->errors[i].tag_atom);
        }
        js_free(ctx, out_fn->errors);
        out_fn->errors = NULL;
    }
    return ret;
}

static int js_host_validate_limits(JSContext *ctx,
                                   JSValueConst limits_val,
                                   const char *path,
                                   JSHostFunctionDef *out_fn)
{
    const char *required[] = {"max_request_bytes", "max_response_bytes", "max_units"};
    const char *optional[] = {"arg_utf8_max"};
    JSValue max_req = JS_UNDEFINED;
    JSValue max_resp = JS_UNDEFINED;
    JSValue max_units = JS_UNDEFINED;
    JSValue arg_utf8 = JS_UNDEFINED;
    int ret = -1;

    if (js_host_expect_object(ctx, limits_val, path))
        return -1;

    if (js_host_check_keys(ctx, limits_val, required, 3, optional, 1, path))
        return -1;

    max_req = JS_GetPropertyStr(ctx, limits_val, "max_request_bytes");
    if (JS_IsException(max_req))
        goto done;
    if (js_host_validate_uint32(ctx, max_req, path, 1, JS_DV_LIMIT_DEFAULTS.max_encoded_bytes, &out_fn->max_request_bytes))
        goto done;

    max_resp = JS_GetPropertyStr(ctx, limits_val, "max_response_bytes");
    if (JS_IsException(max_resp))
        goto done;
    if (js_host_validate_uint32(ctx, max_resp, path, 1, JS_DV_LIMIT_DEFAULTS.max_encoded_bytes, &out_fn->max_response_bytes))
        goto done;

    max_units = JS_GetPropertyStr(ctx, limits_val, "max_units");
    if (JS_IsException(max_units))
        goto done;
    if (js_host_validate_uint32(ctx, max_units, path, 0, UINT32_MAX, &out_fn->max_units))
        goto done;

    arg_utf8 = JS_GetPropertyStr(ctx, limits_val, "arg_utf8_max");
    if (JS_IsException(arg_utf8))
        goto done;

    if (!JS_IsUndefined(arg_utf8)) {
        size_t len = 0;
        if (!JS_IsArray(ctx, arg_utf8)) {
            js_host_manifest_error(ctx, path, "arg_utf8_max must be an array when present");
            goto done;
        }
        if (js_host_array_length(ctx, arg_utf8, &len))
            goto done;
        if (len != out_fn->arity) {
            js_host_manifest_error(ctx, path, "arg_utf8_max length must equal arity");
            goto done;
        }

        for (size_t i = 0; i < out_fn->arity; i++) {
            JSValue limit_val = JS_GetPropertyUint32(ctx, arg_utf8, (uint32_t)i);
            if (JS_IsException(limit_val))
                goto done;
            uint32_t utf8_limit = 0;
            char idx_path[96];
            snprintf(idx_path, sizeof(idx_path), "%s[%zu]", path, i);

            if (js_host_validate_uint32(ctx,
                                        limit_val,
                                        idx_path,
                                        1,
                                        JS_DV_LIMIT_DEFAULTS.max_string_bytes,
                                        &utf8_limit)) {
                JS_FreeValue(ctx, limit_val);
                goto done;
            }

            JS_FreeValue(ctx, limit_val);

            if (out_fn->args[i].type != JS_HOST_SCHEMA_STRING) {
                js_host_manifest_error(ctx, path, "arg_utf8_max may only be used with string arguments");
                goto done;
            }
            out_fn->args[i].utf8_max = utf8_limit;
        }
    }

    ret = 0;

done:
    if (!JS_IsUndefined(max_req))
        JS_FreeValue(ctx, max_req);
    if (!JS_IsUndefined(max_resp))
        JS_FreeValue(ctx, max_resp);
    if (!JS_IsUndefined(max_units))
        JS_FreeValue(ctx, max_units);
    if (!JS_IsUndefined(arg_utf8))
        JS_FreeValue(ctx, arg_utf8);
    return ret;
}

static int js_host_build_function_name(JSContext *ctx, JSHostFunctionDef *fn)
{
    const char *prefix = "Host.v1";
    size_t total = strlen(prefix) + 1; /* null terminator */

    for (size_t i = 0; i < fn->path_len; i++)
        total += 1 + strlen(fn->path_segments[i]); /* dot + segment */

    fn->name = js_malloc(ctx, total);
    if (!fn->name)
        return -1;

    strcpy(fn->name, prefix);
    for (size_t i = 0; i < fn->path_len; i++) {
        strcat(fn->name, ".");
        strcat(fn->name, fn->path_segments[i]);
    }
    return 0;
}

static int js_host_validate_function(JSContext *ctx,
                                     JSValueConst fn_val,
                                     const char *path,
                                     JSHostFunctionDef *out_fn)
{
    const char *required[] = {"fn_id", "js_path", "effect", "arity", "arg_schema", "return_schema", "gas", "limits", "error_codes"};
    JSValue fn_id = JS_UNDEFINED;
    JSValue js_path = JS_UNDEFINED;
    JSValue effect = JS_UNDEFINED;
    JSValue arity = JS_UNDEFINED;
    JSValue arg_schema = JS_UNDEFINED;
    JSValue return_schema = JS_UNDEFINED;
    JSValue gas = JS_UNDEFINED;
    JSValue limits = JS_UNDEFINED;
    JSValue error_codes = JS_UNDEFINED;
    const char *eff_str = NULL;
    char *schedule_id_str = NULL;
    int ret = -1;

    if (js_host_expect_object(ctx, fn_val, path))
        return -1;

    if (js_host_check_keys(ctx, fn_val, required, sizeof(required) / sizeof(required[0]), NULL, 0, path))
        return -1;

    fn_id = JS_GetPropertyStr(ctx, fn_val, "fn_id");
    if (JS_IsException(fn_id))
        goto done;
    if (js_host_validate_uint32(ctx, fn_id, path, 1, UINT32_MAX, &out_fn->fn_id))
        goto done;

    js_path = JS_GetPropertyStr(ctx, fn_val, "js_path");
    if (JS_IsException(js_path))
        goto done;
    if (js_host_validate_js_path(ctx, js_path, "js_path", out_fn))
        goto done;

    effect = JS_GetPropertyStr(ctx, fn_val, "effect");
    if (JS_IsException(effect))
        goto done;
    if (!JS_IsString(effect)) {
        js_host_manifest_error(ctx, path, "effect must be a string");
        goto done;
    }
    eff_str = JS_ToCString(ctx, effect);
    if (!eff_str)
        goto done;
    if (strcmp(eff_str, "READ") == 0) {
        out_fn->effect = JS_HOST_EFFECT_READ;
    } else if (strcmp(eff_str, "EMIT") == 0) {
        out_fn->effect = JS_HOST_EFFECT_EMIT;
    } else if (strcmp(eff_str, "MUTATE") == 0) {
        out_fn->effect = JS_HOST_EFFECT_MUTATE;
    } else {
        js_host_manifest_error(ctx, path, "unsupported effect");
        goto done;
    }
    JS_FreeCString(ctx, eff_str);
    eff_str = NULL;

    arity = JS_GetPropertyStr(ctx, fn_val, "arity");
    if (JS_IsException(arity))
        goto done;
    if (js_host_validate_uint32(ctx, arity, path, 0, UINT32_MAX, &out_fn->arity))
        goto done;

    arg_schema = JS_GetPropertyStr(ctx, fn_val, "arg_schema");
    if (JS_IsException(arg_schema))
        goto done;
    if (!JS_IsArray(ctx, arg_schema)) {
        js_host_manifest_error(ctx, path, "arg_schema must be an array");
        goto done;
    }

    size_t arg_len = 0;
    if (js_host_array_length(ctx, arg_schema, &arg_len))
        goto done;
    if (arg_len != out_fn->arity) {
        js_host_manifest_error(ctx, path, "arg_schema length must equal arity");
        goto done;
    }

    if (out_fn->arity > 0) {
        out_fn->args = js_malloc(ctx, sizeof(JSHostArgDef) * out_fn->arity);
        if (!out_fn->args)
            goto done;
        memset(out_fn->args, 0, sizeof(JSHostArgDef) * out_fn->arity);
    }

    for (size_t i = 0; i < out_fn->arity; i++) {
        JSValue schema_entry = JS_GetPropertyUint32(ctx, arg_schema, (uint32_t)i);
        if (JS_IsException(schema_entry))
            goto done;
        char schema_path[96];
        snprintf(schema_path, sizeof(schema_path), "arg_schema[%zu]", i);
        if (js_host_validate_schema(ctx, schema_entry, schema_path, &out_fn->args[i].type)) {
            JS_FreeValue(ctx, schema_entry);
            goto done;
        }
        JS_FreeValue(ctx, schema_entry);
    }

    return_schema = JS_GetPropertyStr(ctx, fn_val, "return_schema");
    if (JS_IsException(return_schema))
        goto done;
    if (js_host_validate_schema(ctx, return_schema, "return_schema", &out_fn->return_type))
        goto done;

    gas = JS_GetPropertyStr(ctx, fn_val, "gas");
    if (JS_IsException(gas))
        goto done;
    const char *gas_required[] = {"schedule_id", "base", "k_arg_bytes", "k_ret_bytes", "k_units"};
    if (js_host_expect_object(ctx, gas, "gas") ||
        js_host_check_keys(ctx, gas, gas_required, 5, NULL, 0, "gas"))
        goto done;

    JSValue base = JS_GetPropertyStr(ctx, gas, "base");
    JSValue k_arg = JS_GetPropertyStr(ctx, gas, "k_arg_bytes");
    JSValue k_ret = JS_GetPropertyStr(ctx, gas, "k_ret_bytes");
    JSValue k_units = JS_GetPropertyStr(ctx, gas, "k_units");
    JSValue schedule_id = JS_GetPropertyStr(ctx, gas, "schedule_id");
    if (JS_IsException(base) || JS_IsException(k_arg) || JS_IsException(k_ret) ||
        JS_IsException(k_units) || JS_IsException(schedule_id)) {
        JS_FreeValue(ctx, base);
        JS_FreeValue(ctx, k_arg);
        JS_FreeValue(ctx, k_ret);
        JS_FreeValue(ctx, k_units);
        JS_FreeValue(ctx, schedule_id);
        goto done;
    }

    if (js_host_validate_uint32(ctx, base, "gas.base", 0, UINT32_MAX, &out_fn->gas_base) ||
        js_host_validate_uint32(ctx, k_arg, "gas.k_arg_bytes", 0, UINT32_MAX, &out_fn->gas_k_arg_bytes) ||
        js_host_validate_uint32(ctx, k_ret, "gas.k_ret_bytes", 0, UINT32_MAX, &out_fn->gas_k_ret_bytes) ||
        js_host_validate_uint32(ctx, k_units, "gas.k_units", 0, UINT32_MAX, &out_fn->gas_k_units) ||
        js_host_copy_non_empty_string(ctx, schedule_id, "gas.schedule_id", &schedule_id_str)) {
        JS_FreeValue(ctx, base);
        JS_FreeValue(ctx, k_arg);
        JS_FreeValue(ctx, k_ret);
        JS_FreeValue(ctx, k_units);
        JS_FreeValue(ctx, schedule_id);
        goto done;
    }
    out_fn->gas_schedule_id = schedule_id_str;
    schedule_id_str = NULL;

    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, k_arg);
    JS_FreeValue(ctx, k_ret);
    JS_FreeValue(ctx, k_units);
    JS_FreeValue(ctx, schedule_id);

    limits = JS_GetPropertyStr(ctx, fn_val, "limits");
    if (JS_IsException(limits))
        goto done;
    if (js_host_validate_limits(ctx, limits, "limits", out_fn))
        goto done;

    error_codes = JS_GetPropertyStr(ctx, fn_val, "error_codes");
    if (JS_IsException(error_codes))
        goto done;
    if (js_host_validate_error_codes(ctx, error_codes, "error_codes", out_fn))
        goto done;

    /* Gas overflow guard: base + (req * k_arg) + (resp * k_ret) + (units * k_units) */
    {
        uint64_t total = out_fn->gas_base;
        uint64_t arg_part = (uint64_t)out_fn->gas_k_arg_bytes * (uint64_t)out_fn->max_request_bytes;
        uint64_t ret_part = (uint64_t)out_fn->gas_k_ret_bytes * (uint64_t)out_fn->max_response_bytes;
        uint64_t unit_part = (uint64_t)out_fn->gas_k_units * (uint64_t)out_fn->max_units;

        if (arg_part > UINT64_MAX - total ||
            ret_part > UINT64_MAX - total - arg_part ||
            unit_part > UINT64_MAX - total - arg_part - ret_part) {
            js_host_manifest_error(ctx, "gas", "gas charges overflow uint64 bounds");
            goto done;
        }
    }

    if (js_host_build_function_name(ctx, out_fn))
        goto done;

    ret = 0;

done:
    if (!JS_IsUndefined(fn_id))
        JS_FreeValue(ctx, fn_id);
    if (!JS_IsUndefined(js_path))
        JS_FreeValue(ctx, js_path);
    if (!JS_IsUndefined(effect))
        JS_FreeValue(ctx, effect);
    if (!JS_IsUndefined(arity))
        JS_FreeValue(ctx, arity);
    if (!JS_IsUndefined(arg_schema))
        JS_FreeValue(ctx, arg_schema);
    if (!JS_IsUndefined(return_schema))
        JS_FreeValue(ctx, return_schema);
    if (!JS_IsUndefined(gas))
        JS_FreeValue(ctx, gas);
    if (!JS_IsUndefined(limits))
        JS_FreeValue(ctx, limits);
    if (!JS_IsUndefined(error_codes))
        JS_FreeValue(ctx, error_codes);
    if (eff_str)
        JS_FreeCString(ctx, eff_str);
    if (schedule_id_str)
        js_free(ctx, schedule_id_str);

    if (ret != 0) {
        if (out_fn->name) {
            js_free(ctx, out_fn->name);
            out_fn->name = NULL;
        }
    }
    return ret;
}

static BOOL js_host_paths_conflict(const JSHostFunctionDef *a, const JSHostFunctionDef *b)
{
    size_t len = a->path_len < b->path_len ? a->path_len : b->path_len;
    for (size_t i = 0; i < len; i++) {
        if (strcmp(a->path_segments[i], b->path_segments[i]) != 0)
            return FALSE;
    }
    return TRUE;
}

static int js_host_validate_manifest(JSContext *ctx, JSValueConst manifest_val, JSHostManifest *out_manifest)
{
    const char *required[] = {"abi_id", "abi_version", "functions"};
    JSValue abi_id = JS_UNDEFINED;
    JSValue abi_version = JS_UNDEFINED;
    JSValue functions = JS_UNDEFINED;
    char *abi_id_str = NULL;
    int ret = -1;

    if (js_host_expect_object(ctx, manifest_val, "manifest"))
        return -1;

    if (js_host_check_keys(ctx, manifest_val, required, 3, NULL, 0, "manifest"))
        return -1;

    abi_id = JS_GetPropertyStr(ctx, manifest_val, "abi_id");
    if (JS_IsException(abi_id))
        goto done;
    if (js_host_copy_non_empty_string(ctx, abi_id, "manifest.abi_id", &abi_id_str))
        goto done;
    if (strcmp(abi_id_str, "Host.v1") != 0) {
        js_host_manifest_error(ctx, "manifest.abi_id", "unsupported abi_id (expected Host.v1)");
        goto done;
    }

    abi_version = JS_GetPropertyStr(ctx, manifest_val, "abi_version");
    if (JS_IsException(abi_version))
        goto done;
    uint32_t version = 0;
    if (js_host_validate_uint32(ctx, abi_version, "manifest.abi_version", 1, UINT32_MAX, &version))
        goto done;
    if (version != 1) {
        js_host_manifest_error(ctx, "manifest.abi_version", "unsupported abi_version (expected 1)");
        goto done;
    }

    functions = JS_GetPropertyStr(ctx, manifest_val, "functions");
    if (JS_IsException(functions))
        goto done;
    if (!JS_IsArray(ctx, functions)) {
        js_host_manifest_error(ctx, "manifest.functions", "functions must be an array");
        goto done;
    }

    if (js_host_array_length(ctx, functions, &out_manifest->function_count))
        goto done;
    if (out_manifest->function_count == 0) {
        js_host_manifest_error(ctx, "manifest.functions", "functions must contain at least one entry");
        goto done;
    }

    out_manifest->functions = js_malloc(ctx, sizeof(JSHostFunctionDef) * out_manifest->function_count);
    if (!out_manifest->functions)
        goto done;
    memset(out_manifest->functions, 0, sizeof(JSHostFunctionDef) * out_manifest->function_count);

    uint32_t prev_fn_id = 0;
    for (size_t i = 0; i < out_manifest->function_count; i++) {
        JSValue fn_val = JS_GetPropertyUint32(ctx, functions, (uint32_t)i);
        if (JS_IsException(fn_val))
            goto done;

        char fn_path[96];
        snprintf(fn_path, sizeof(fn_path), "manifest.functions[%zu]", i);

        if (js_host_validate_function(ctx, fn_val, fn_path, &out_manifest->functions[i])) {
            JS_FreeValue(ctx, fn_val);
            goto done;
        }

        JS_FreeValue(ctx, fn_val);

        if (i > 0 && out_manifest->functions[i].fn_id <= prev_fn_id) {
            js_host_manifest_error(ctx, "manifest.functions", "functions must be sorted by ascending fn_id");
            goto done;
        }
        prev_fn_id = out_manifest->functions[i].fn_id;
    }

    for (size_t i = 0; i < out_manifest->function_count; i++) {
        for (size_t j = i + 1; j < out_manifest->function_count; j++) {
            if (js_host_paths_conflict(&out_manifest->functions[i], &out_manifest->functions[j])) {
                js_host_manifest_error(ctx, "manifest.functions", "js_path collision detected");
                goto done;
            }
        }
    }

    ret = 0;

done:
    if (abi_id_str)
        js_free(ctx, abi_id_str);
    if (!JS_IsUndefined(abi_id))
        JS_FreeValue(ctx, abi_id);
    if (!JS_IsUndefined(abi_version))
        JS_FreeValue(ctx, abi_version);
    if (!JS_IsUndefined(functions))
        JS_FreeValue(ctx, functions);

    if (ret != 0)
        js_host_manifest_clear(ctx, out_manifest);
    return ret;
}

static int js_host_namespace_push(JSContext *ctx,
                                  JSValue ns,
                                  JSValue **out,
                                  size_t *count,
                                  size_t *capacity)
{
    void *ptr = JS_VALUE_GET_PTR(ns);
    for (size_t i = 0; i < *count; i++) {
        if (JS_VALUE_GET_PTR((*out)[i]) == ptr)
            return 0;
    }

    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        JSValue *resized = js_realloc(ctx, *out, sizeof(JSValue) * new_cap);
        if (!resized)
            return -1;
        *out = resized;
        *capacity = new_cap;
    }

    (*out)[*count] = JS_DupValue(ctx, ns);
    (*count)++;
    return 0;
}

static int js_host_get_or_create_child(JSContext *ctx,
                                       JSValue parent,
                                       const char *name,
                                       JSValue *out_child,
                                       JSValue **namespaces,
                                       size_t *ns_count,
                                       size_t *ns_capacity)
{
    JSValue child = JS_UNDEFINED;
    JSAtom atom = JS_NewAtom(ctx, name);
    int ret = -1;

    if (atom == JS_ATOM_NULL)
        return -1;

    child = JS_GetProperty(ctx, parent, atom);
    if (JS_IsException(child))
        goto done;

    if (JS_IsUndefined(child)) {
        child = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(child))
            goto done;
        if (JS_DefinePropertyValue(ctx,
                                   parent,
                                   atom,
                                   JS_DupValue(ctx, child),
                                   JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                                       JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE) < 0)
            goto done;
    } else if (!JS_IsObject(child)) {
        js_host_manifest_error(ctx, name, "Host namespace collision");
        goto done;
    }

    if (js_host_namespace_push(ctx, child, namespaces, ns_count, ns_capacity))
        goto done;

    *out_child = child;
    child = JS_UNDEFINED;
    ret = 0;

done:
    if (atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, atom);
    if (!JS_IsUndefined(child))
        JS_FreeValue(ctx, child);
    return ret;
}

static int js_host_charge_pre(JSContext *ctx, const JSHostFunctionDef *fn, size_t req_len)
{
    uint64_t charge = fn->gas_base;
    uint64_t arg_part = (uint64_t)fn->gas_k_arg_bytes * (uint64_t)req_len;

    if (arg_part > UINT64_MAX - charge) {
        JS_ThrowTypeError(ctx, "host_call gas overflow");
        return -1;
    }

    charge += arg_part;
    return JS_UseGas(ctx, charge);
}

static int js_host_charge_post(JSContext *ctx, const JSHostFunctionDef *fn, size_t resp_len, uint32_t units)
{
    uint64_t charge = 0;
    uint64_t resp_part = (uint64_t)fn->gas_k_ret_bytes * (uint64_t)resp_len;
    uint64_t unit_part = (uint64_t)fn->gas_k_units * (uint64_t)units;

    if (resp_part > UINT64_MAX - charge || unit_part > UINT64_MAX - charge - resp_part) {
        JS_ThrowTypeError(ctx, "host_call gas overflow");
        return -1;
    }

    charge += resp_part + unit_part;
    return JS_UseGas(ctx, charge);
}

static JSValue js_host_call_wrapper(JSContext *ctx,
                                    JSValueConst this_val,
                                    int argc,
                                    JSValueConst *argv,
                                    int magic)
{
    JSHostManifest *manifest = js_host_find_manifest(ctx);
    JSDvBuffer req_buf = {0};
    JSValue args_array = JS_UNDEFINED;
    JSHostResponse resp;
    JSHostCallResult result = {0};
    JSValue ret = JS_EXCEPTION;

    if (!manifest) {
        JS_ThrowTypeError(ctx, "host manifest is not initialized");
        return JS_EXCEPTION;
    }

    if (magic < 0 || (size_t)magic >= manifest->function_count) {
        JS_ThrowTypeError(ctx, "invalid host function binding");
        return JS_EXCEPTION;
    }

    const JSHostFunctionDef *fn = &manifest->functions[magic];

    if (argc != (int)fn->arity) {
        JS_ThrowTypeError(ctx, "%s expects %u arguments", fn->name ? fn->name : "Host function", fn->arity);
        return JS_EXCEPTION;
    }

    for (size_t i = 0; i < fn->arity; i++) {
        const JSHostArgDef *arg = &fn->args[i];
        JSValueConst val = argv[i];

        switch (arg->type) {
        case JS_HOST_SCHEMA_STRING: {
            if (!JS_IsString(val)) {
                JS_ThrowTypeError(ctx, "%s argument %zu must be a string", fn->name ? fn->name : "Host function", i + 1);
                return JS_EXCEPTION;
            }
            if (arg->utf8_max > 0) {
                size_t utf8_len = 0;
                const char *str = JS_ToCStringLen2(ctx, &utf8_len, val, 0);
                if (!str)
                    return JS_EXCEPTION;
                JS_FreeCString(ctx, str);
                if (utf8_len > arg->utf8_max) {
                    JS_ThrowTypeError(ctx,
                                      "%s argument %zu exceeds utf8 limit (%zu > %u)",
                                      fn->name ? fn->name : "Host function",
                                      i + 1,
                                      utf8_len,
                                      arg->utf8_max);
                    return JS_EXCEPTION;
                }
            }
            break;
        }
        case JS_HOST_SCHEMA_DV:
            break;
        case JS_HOST_SCHEMA_NULL:
            if (!JS_IsNull(val)) {
                JS_ThrowTypeError(ctx, "%s argument %zu must be null", fn->name ? fn->name : "Host function", i + 1);
                return JS_EXCEPTION;
            }
            break;
        }
    }

    if (JS_RunGCCheckpoint(ctx))
        return JS_EXCEPTION;

    args_array = JS_NewArray(ctx);
    if (JS_IsException(args_array))
        return JS_EXCEPTION;

    for (size_t i = 0; i < fn->arity; i++) {
        if (JS_SetPropertyUint32(ctx, args_array, (uint32_t)i, JS_DupValue(ctx, argv[i])) < 0) {
            JS_FreeValue(ctx, args_array);
            return JS_EXCEPTION;
        }
    }

    JSDvLimits dv_limits = JS_DV_LIMIT_DEFAULTS;
    dv_limits.max_encoded_bytes = fn->max_request_bytes;

    if (JS_EncodeDV(ctx, args_array, &dv_limits, &req_buf)) {
        JS_FreeValue(ctx, args_array);
        JS_FreeDVBuffer(ctx, &req_buf);
        return JS_EXCEPTION;
    }

    JS_FreeValue(ctx, args_array);
    args_array = JS_UNDEFINED;

    if (js_host_charge_pre(ctx, fn, req_buf.length)) {
        JS_FreeDVBuffer(ctx, &req_buf);
        return JS_EXCEPTION;
    }

    if (JS_RunGCCheckpoint(ctx)) {
        JS_FreeDVBuffer(ctx, &req_buf);
        return JS_EXCEPTION;
    }

    if (JS_HostCall(ctx,
                    fn->fn_id,
                    req_buf.data,
                    req_buf.length,
                    fn->max_request_bytes,
                    fn->max_response_bytes,
                    &result)) {
        JS_FreeDVBuffer(ctx, &req_buf);
        return JS_EXCEPTION;
    }

    JS_FreeDVBuffer(ctx, &req_buf);

    JSHostResponseValidation validation = {
        .max_units = fn->max_units,
        .errors = fn->errors,
        .error_count = fn->error_count,
    };

    if (JS_ParseHostResponse(ctx, result.data, result.length, &validation, &resp))
        return JS_EXCEPTION;

    if (js_host_charge_post(ctx, fn, result.length, resp.units)) {
        JS_FreeHostResponse(ctx, &resp);
        return JS_EXCEPTION;
    }

    if (JS_RunGCCheckpoint(ctx)) {
        JS_FreeHostResponse(ctx, &resp);
        return JS_EXCEPTION;
    }

    if (resp.is_error) {
        ret = JS_ThrowHostError(ctx, resp.err_code_atom, resp.err_tag_atom, resp.err_details);
        JS_FreeHostResponse(ctx, &resp);
        return ret;
    }

    switch (fn->return_type) {
    case JS_HOST_SCHEMA_STRING:
        if (!JS_IsString(resp.ok)) {
            JS_FreeHostResponse(ctx, &resp);
            return js_throw_host_envelope_invalid(ctx);
        }
        break;
    case JS_HOST_SCHEMA_NULL:
        if (!JS_IsNull(resp.ok)) {
            JS_FreeHostResponse(ctx, &resp);
            return js_throw_host_envelope_invalid(ctx);
        }
        break;
    case JS_HOST_SCHEMA_DV:
        break;
    }

    ret = resp.ok;
    resp.ok = JS_UNDEFINED;
    JS_FreeHostResponse(ctx, &resp);
    return ret;
}

static int js_host_install_functions(JSContext *ctx, JSHostManifest *manifest)
{
    JSValue global = JS_UNDEFINED;
    JSValue host = JS_UNDEFINED;
    JSValue host_v1 = JS_UNDEFINED;
    JSValue *namespaces = NULL;
    size_t ns_count = 0;
    size_t ns_capacity = 0;
    int ret = -1;

    global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global))
        goto done;

    if (js_host_get_or_create_child(ctx, global, "Host", &host, &namespaces, &ns_count, &ns_capacity))
        goto done;

    if (js_host_get_or_create_child(ctx, host, "v1", &host_v1, &namespaces, &ns_count, &ns_capacity))
        goto done;

    for (size_t i = 0; i < manifest->function_count; i++) {
        JSValue current = JS_DupValue(ctx, host_v1);
        if (JS_IsException(current))
            goto done;

        JSValue next = JS_UNDEFINED;
        JSHostFunctionDef *fn = &manifest->functions[i];

        for (size_t j = 0; j + 1 < fn->path_len; j++) {
            if (js_host_get_or_create_child(ctx,
                                            current,
                                            fn->path_segments[j],
                                            &next,
                                            &namespaces,
                                            &ns_count,
                                            &ns_capacity)) {
                JS_FreeValue(ctx, current);
                goto done;
            }
            JS_FreeValue(ctx, current);
            current = next;
            next = JS_UNDEFINED;
        }

        JSAtom fn_atom = JS_NewAtom(ctx, fn->path_segments[fn->path_len - 1]);
        if (fn_atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, current);
            goto done;
        }

        JSValue cfunc = JS_NewCFunctionMagic(ctx,
                                             js_host_call_wrapper,
                                             fn->path_segments[fn->path_len - 1],
                                             fn->arity,
                                             JS_CFUNC_generic_magic,
                                             (int)i);
        if (JS_IsException(cfunc)) {
            JS_FreeAtom(ctx, fn_atom);
            JS_FreeValue(ctx, current);
            goto done;
        }

        if (JS_DefinePropertyValue(ctx,
                                   current,
                                   fn_atom,
                                   cfunc,
                                   JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                                       JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE) < 0) {
            JS_FreeAtom(ctx, fn_atom);
            JS_FreeValue(ctx, current);
            goto done;
        }

        JS_FreeAtom(ctx, fn_atom);
        JS_FreeValue(ctx, current);
    }

    for (size_t i = 0; i < ns_count; i++) {
        if (JS_PreventExtensions(ctx, namespaces[i]) < 0)
            goto done;
    }

    ret = 0;

done:
    if (namespaces) {
        for (size_t i = 0; i < ns_count; i++)
            JS_FreeValue(ctx, namespaces[i]);
        js_free(ctx, namespaces);
    }
    if (!JS_IsUndefined(host_v1))
        JS_FreeValue(ctx, host_v1);
    if (!JS_IsUndefined(host))
        JS_FreeValue(ctx, host);
    if (!JS_IsUndefined(global))
        JS_FreeValue(ctx, global);
    return ret;
}

int JS_InitHostFromManifest(JSContext *ctx, const uint8_t *manifest_bytes, size_t manifest_size)
{
    JSValue manifest_val = JS_UNDEFINED;
    JSHostManifest manifest = {0};
    JSHostManifestNode *node = NULL;
    int ret = -1;

    if (!ctx || !manifest_bytes || manifest_size == 0) {
        JS_ThrowTypeError(ctx, "abi manifest is required");
        return -1;
    }

    if (js_host_find_manifest(ctx)) {
        JS_ThrowTypeError(ctx, "abi manifest is already initialized");
        return -1;
    }

    manifest_val = JS_DecodeDV(ctx, manifest_bytes, manifest_size, &JS_DV_LIMIT_DEFAULTS);
    if (JS_IsException(manifest_val))
        return -1;

    if (js_host_validate_manifest(ctx, manifest_val, &manifest))
        goto done;

    node = js_malloc(ctx, sizeof(JSHostManifestNode));
    if (!node)
        goto done;

    node->ctx = ctx;
    node->manifest = manifest;
    node->next = NULL;
    memset(&manifest, 0, sizeof(manifest)); /* ownership moved */

    if (js_host_install_functions(ctx, &node->manifest))
        goto done;

    node->next = js_host_manifest_list;
    js_host_manifest_list = node;
    node = NULL;

    ret = 0;

done:
    if (!JS_IsUndefined(manifest_val))
        JS_FreeValue(ctx, manifest_val);
    if (node) {
        js_host_manifest_clear(ctx, &node->manifest);
        js_free(ctx, node);
    } else if (ret != 0) {
        js_host_manifest_clear(ctx, &manifest);
    }
    return ret;
}
