#ifndef QUICKJS_INTERNAL_H
#define QUICKJS_INTERNAL_H

#include "quickjs.h"
#include <stddef.h>
#include <stdint.h>

/* Internal helpers not exposed in the public QuickJS API surface. */
void js_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]);
void js_sha256_to_hex(const uint8_t hash[32], char out_hex[65]);

typedef JSValue (*JSCompileRegExpFunc)(JSContext *ctx, JSValueConst pattern,
                                       JSValueConst flags);

void *js_malloc_rt(JSRuntime *rt, size_t size);
void js_free_rt(JSRuntime *rt, void *ptr);

void js_deterministic_clear_eval_obj(JSContext *ctx);
void js_deterministic_set_regexp(JSContext *ctx, JSValueConst ctor, JSCompileRegExpFunc func);
void js_deterministic_set_promise_ctor(JSContext *ctx, JSValueConst ctor);
void js_deterministic_set_random_state(JSContext *ctx, uint32_t state);
void js_deterministic_set_mode(JSContext *ctx, JS_BOOL enabled);
JS_BOOL js_deterministic_manifest_is_initialized(JSContext *ctx);
void js_deterministic_set_manifest_state(JSContext *ctx,
                                         uint8_t *manifest_bytes,
                                         size_t manifest_size,
                                         const uint8_t hash[32],
                                         const char hash_hex[65],
                                         uint8_t *context_blob,
                                         size_t context_blob_size);

int js_deterministic_init_context(JSContext *ctx);

int js_reserve_host_response_buffer(JSContext *ctx, uint32_t capacity);
uint8_t *js_get_host_response_buffer(JSContext *ctx);
JSHostCallFunc *js_get_host_call_func(JSRuntime *rt);
void *js_get_host_call_opaque(JSRuntime *rt);
JS_BOOL js_runtime_in_host_call(JSRuntime *rt);
void js_runtime_set_in_host_call(JSRuntime *rt, JS_BOOL in_call);

#endif /* QUICKJS_INTERNAL_H */
