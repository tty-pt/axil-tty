#ifndef TTYPT_WS_H
#define TTYPT_WS_H

/*
 * ws.h — Client WebSocket streaming transport interface for WASM & Native.
 * Unreserved client prefix avoids colliding with server-side axil_* symbols.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __wasm__
__attribute__((import_module("env"), import_name("ws_connect")))
void ws_connect(const char *url, size_t len);

__attribute__((import_module("env"), import_name("ws_send")))
void ws_send(const void *buf, size_t len);

__attribute__((import_module("env"), import_name("ws_close")))
void ws_close(void);

__attribute__((import_module("env"), import_name("term_write")))
void term_write(const char *buf, size_t len);
#else
static inline void ws_connect(const char *url, size_t len) { (void)url; (void)len; }
static inline void ws_send(const void *buf, size_t len) { (void)buf; (void)len; }
static inline void ws_close(void) {}
static inline void term_write(const char *buf, size_t len) { (void)buf; (void)len; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_WS_H */
