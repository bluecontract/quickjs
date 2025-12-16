#ifndef QUICKJS_HOST_H
#define QUICKJS_HOST_H

#include "quickjs.h"

typedef struct JSHostErrorEntry {
    JSAtom code_atom;
    JSAtom tag_atom;
} JSHostErrorEntry;

typedef struct JSHostResponseValidation {
    uint32_t max_units;
    const JSHostErrorEntry *errors;
    size_t error_count;
} JSHostResponseValidation;

typedef struct JSHostResponse {
    int is_error;
    uint32_t units;
    JSValue ok;
    JSAtom err_code_atom;
    JSAtom err_tag_atom;
    JSValue err_details;
} JSHostResponse;

typedef struct JSHostManifest JSHostManifest;

/* Host response envelope helpers (T-039). */
JSValue JS_ThrowHostError(JSContext *ctx, JSAtom code_atom, JSAtom tag_atom, JSValueConst details);
JSValue JS_ThrowHostTransportError(JSContext *ctx);
int JS_ParseHostResponse(JSContext *ctx,
                         const uint8_t *data,
                         size_t length,
                         const JSHostResponseValidation *validation,
                         JSHostResponse *out);
void JS_FreeHostResponse(JSContext *ctx, JSHostResponse *resp);
int JS_InitHostFromManifest(JSContext *ctx, const uint8_t *manifest_bytes, size_t manifest_size);
void JS_FreeHostManifest(JSContext *ctx);

#endif /* QUICKJS_HOST_H */
