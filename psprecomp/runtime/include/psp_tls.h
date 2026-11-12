#pragma once
// PSP_TLS_FAST: thread_local variables on the game thread's hot path. On desktop the initial-exec model makes an access a
// single %fs-relative load instead of a __tls_get_addr call. NOT on Android: bionic refuses initial-exec TLS in a dlopen()ed
// library ("dlopen failed: TLS symbol (null)"); there API 29 native ELF TLS is used as is.
#if defined(__ANDROID__) || defined(_WIN32) || defined(__APPLE__) || !defined(__GNUC__)
#define PSP_TLS_FAST
#else
#define PSP_TLS_FAST __attribute__((tls_model("initial-exec")))
#endif
