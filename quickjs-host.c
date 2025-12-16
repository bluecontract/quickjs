#include "cutils.h"
#include "quickjs-host.h"
#include "quickjs-internal.h"
#include <math.h>
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
