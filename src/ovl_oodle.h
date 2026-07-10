/**
 * ovlTC — A native Total Commander packer plugin (WCX, 64-bit) for .ovl files.
 *
 * Copyright (c) 2026 LordDog
 * Open source — feel free to use, modify, and distribute.
 * Credits are appreciated but not required.
 *
 * Dynamic loading of oo2core_*_win64.dll (Oodle) for decompression. The DLL
 * is proprietary (part of the game) and is NOT bundled; it is located at
 * runtime in the directory of the opened .ovl file (and its parents).
 */
#ifndef OVL_OODLE_H
#define OVL_OODLE_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HMODULE dll;
    void *decompress_fn; /* OodleLZ_Decompress */
} ovl_oodle_t;

/* Searches for oo2core_*win64.dll in the given directory and loads it.
   Returns 1 on success, 0 if no matching DLL could be found/loaded. */
int ovl_oodle_load(const wchar_t *dir, ovl_oodle_t *out);

/* Like ovl_oodle_load, but additionally searches up to max_levels parent
   directories (the Oodle DLL usually sits in a game's install root, while
   .ovl files can be nested much deeper). */
int ovl_oodle_load_upward(const wchar_t *start_dir, int max_levels, ovl_oodle_t *out);

/* Decompresses compressed (compressed_size bytes) into out (must be exactly
   out_size bytes). Returns 1 on success. */
int ovl_oodle_decompress(ovl_oodle_t *o, const unsigned char *compressed, size_t compressed_size,
                          unsigned char *out, size_t out_size);

void ovl_oodle_unload(ovl_oodle_t *o);

#ifdef __cplusplus
}
#endif

#endif /* OVL_OODLE_H */
