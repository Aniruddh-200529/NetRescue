#ifndef NETRESCUE_API_H
#define NETRESCUE_API_H

/*
 * Browser-facing engine API (compiled to WebAssembly with Emscripten, and natively for tests and the
 * netrescue_api command-line fallback).
 *
 * One entry point takes a JSON request {"cmd": "...", ...} and returns a JSON response:
 *   {"ok": true,  "api_version": 1, "result": {...}}
 *   {"ok": false, "api_version": 1, "error": "message"}
 * The returned pointer stays valid until the next call. The full command reference is docs/api.md.
 * The engine keeps one session (network, endpoints, configuration, live simulation, last experiment);
 * no internal pointers ever cross the interface.
 */

#define NR_API_VERSION 1

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define NR_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define NR_EXPORT
#endif

NR_EXPORT const char *nr_api_call(const char *request_json);
NR_EXPORT void nr_api_shutdown(void);

#endif
