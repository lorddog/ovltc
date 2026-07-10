/**
 * ovlTC — A native Total Commander packer plugin (WCX, 64-bit) for .ovl files.
 *
 * Copyright (c) 2026 LordDog
 * Open source — feel free to use, modify, and distribute.
 * Credits are appreciated but not required.
 */
#include "ovl_oodle.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* SINTa OodleLZ_Decompress(const void*, SINTa, void*, SINTa, int, int, int,
                             void*, SINTa, void*, void*, void*, SINTa, int);
   On x64 there is no separate stdcall/cdecl distinction (unified MS x64 ABI). */
typedef intptr_t (*OodleLZ_Decompress_t)(
    const void *compBuf, intptr_t compBufSize,
    void *rawBuf, intptr_t rawLen,
    int fuzzSafe, int checkCRC, int verbosity,
    void *decBufBase, intptr_t decBufSize,
    void *fpCallback, void *callbackUserData,
    void *decoderMemory, intptr_t decoderMemorySize,
    int threadPhase);

int ovl_oodle_load(const wchar_t *dir, ovl_oodle_t *out) {
    memset(out, 0, sizeof(*out));

    wchar_t pattern[MAX_PATH];
    if (_snwprintf(pattern, MAX_PATH - 1, L"%s\\oo2core_*win64.dll", dir) < 0) return 0;
    pattern[MAX_PATH - 1] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    wchar_t full_path[MAX_PATH];
    int ok = _snwprintf(full_path, MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName) >= 0;
    full_path[MAX_PATH - 1] = L'\0';
    FindClose(h);
    if (!ok) return 0;

    HMODULE dll = LoadLibraryW(full_path);
    if (!dll) return 0;

    void *fn = (void *)GetProcAddress(dll, "OodleLZ_Decompress");
    if (!fn) {
        FreeLibrary(dll);
        return 0;
    }

    out->dll = dll;
    out->decompress_fn = fn;
    return 1;
}

int ovl_oodle_load_upward(const wchar_t *start_dir, int max_levels, ovl_oodle_t *out) {
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, start_dir, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';

    size_t len = wcslen(dir);
    while (len > 0 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/')) {
        dir[--len] = L'\0';
    }

    for (int level = 0; level <= max_levels; level++) {
        if (ovl_oodle_load(dir, out)) return 1;

        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash) break;
        if (slash - dir <= 2) break; /* reached "C:\" (drive root) */
        *slash = L'\0';
    }
    return 0;
}

int ovl_oodle_decompress(ovl_oodle_t *o, const unsigned char *compressed, size_t compressed_size,
                          unsigned char *out, size_t out_size) {
    if (!o || !o->decompress_fn) return 0;
    OodleLZ_Decompress_t fn = (OodleLZ_Decompress_t)o->decompress_fn;

    intptr_t ret = fn(compressed, (intptr_t)compressed_size,
                       out, (intptr_t)out_size,
                       0, 0, 0,
                       NULL, 0,
                       NULL, NULL,
                       NULL, 0,
                       3 /* OodleLZ_Decode_ThreadPhaseAll */);

    return ret == (intptr_t)out_size;
}

void ovl_oodle_unload(ovl_oodle_t *o) {
    if (o->dll) FreeLibrary(o->dll);
    memset(o, 0, sizeof(*o));
}
