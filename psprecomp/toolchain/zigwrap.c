/* zig-cc / zig-c++ / zig-ar / zig-ranlib: one tiny launcher per tool, so CMake and ffmpeg's configure (which want a single
 * executable per tool, not "zig cc") can use the bundled zig.  Built by build_bundle.py with the bundled zig itself:
 *
 *   zig cc -O2 -DZW_TARGET='"x86_64-linux-gnu.2.28"' zigwrap.c -o bin/zig-cc[.exe]   (and copies for the other names)
 *
 * The tool is chosen from this executable's own name (zig-cc -> "zig cc -target <ZW_TARGET> ...", zig-ar -> "zig ar ...");
 * zig itself is expected at ../zig/zig[.exe] next to bin/.  Environment: ZIGWRAP_EXTRA = extra compiler flags (space
 * separated, cc/c++ only).  Everything else is passed through unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define SEP '\\'
#define ZIG_REL "..\\zig\\zig.exe"
#else
#include <unistd.h>
#include <limits.h>
#define SEP '/'
#define ZIG_REL "../zig/zig"
#endif

#ifndef ZW_TARGET
#error "define ZW_TARGET, e.g. -DZW_TARGET='\"x86_64-linux-gnu.2.28\"'"
#endif

static void die(const char *m) { fprintf(stderr, "zigwrap: %s\n", m); exit(127); }

int main(int argc, char **argv) {
    char self[4096];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, self, sizeof self);
    if (n == 0 || n >= sizeof self) die("cannot find own path");
#else
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) die("cannot find own path");
    self[n] = 0;
#endif
    char *slash = strrchr(self, SEP);
    if (!slash) die("bad own path");
    const char *name = slash + 1;
    char dir[4096];
    size_t dl = (size_t)(slash - self);
    memcpy(dir, self, dl); dir[dl] = 0;

    char zig[8192];
    snprintf(zig, sizeof zig, "%s%c%s", dir, SEP, ZIG_REL);

    const char *sub = NULL; int is_cc = 0;
    if (!strncmp(name, "zig-cc", 6)) { sub = "cc"; is_cc = 1; }
    else if (!strncmp(name, "zig-c++", 7)) { sub = "c++"; is_cc = 1; }
    else if (!strncmp(name, "zig-ar", 6)) sub = "ar";
    else if (!strncmp(name, "zig-ranlib", 10)) sub = "ranlib";
    else if (!strncmp(name, "zig-rc", 6)) sub = "rc";
    else die("unknown tool name (expected zig-cc, zig-c++, zig-ar, zig-ranlib, zig-rc)");

    char **av = calloc((size_t)argc + 64, sizeof *av);
    int k = 0;
    av[k++] = zig; av[k++] = (char *)sub;
    if (is_cc) { av[k++] = "-target"; av[k++] = ZW_TARGET; }
    if (is_cc) {
        const char *extra = getenv("ZIGWRAP_EXTRA");
        if (extra && *extra) {
            char *copy = strdup(extra);
            for (char *t = strtok(copy, " "); t && k < argc + 60; t = strtok(NULL, " ")) av[k++] = t;
        }
    }
    for (int i = 1; i < argc; i++) av[k++] = argv[i];
    av[k] = NULL;
#ifdef _WIN32
    /* _spawnv does not quote: build the quoted argv ourselves */
    for (int i = 0; i < k; i++) {
        size_t l = strlen(av[i]);
        char *q = malloc(l * 2 + 3); size_t o = 0; int need = (l == 0) || strpbrk(av[i], " \t\"") != NULL;
        if (need) q[o++] = '"';
        for (size_t j = 0; j < l; j++) { if (av[i][j] == '"') q[o++] = '\\'; q[o++] = av[i][j]; }
        if (need) q[o++] = '"';
        q[o] = 0; av[i] = q;
    }
    intptr_t rc = _spawnv(_P_WAIT, zig, (const char *const *)av);
    if (rc == -1) die("cannot run zig");
    return (int)rc;
#else
    execv(zig, av);
    die("cannot run zig");
    return 127;
#endif
}
