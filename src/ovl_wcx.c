/**
 * ovlTC — A native Total Commander packer plugin (WCX, 64-bit) for .ovl files.
 *
 * Copyright (c) 2026 LordDog
 * Open source — feel free to use, modify, and distribute.
 * Credits are appreciated but not required.
 *
 * Implements the read-only Unicode WCX interface:
 *   OpenArchiveW / ReadHeaderExW / ProcessFileW / CloseArchive /
 *   SetChangeVolProcW / SetProcessDataProcW / GetPackerCaps
 *
 * On open, the whole .ovl file is parsed and every contained archive
 * (STATIC embedded in the header, plus any external .ovs files found next
 * to the .ovl) is decompressed to build the full entry list (names + sizes).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#include "wcxhead.h"
#include "ovl_format.h"
#include "ovl_oodle.h"

/* Debug output to stderr, only active when the OVL_WCX_DEBUG environment
   variable is set. Used for diagnostics during development; stays silent
   in normal (release) operation. */
static int ovl_debug_enabled(void) {
    static int checked = 0, enabled = 0;
    if (!checked) {
        checked = 1;
        enabled = GetEnvironmentVariableA("OVL_WCX_DEBUG", NULL, 0) > 0;
    }
    return enabled;
}
#define DBG(...) do { if (ovl_debug_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------------------
 * Data structures
 * ------------------------------------------------------------------------- */

typedef struct {
    wchar_t name[600];   /* display name including subfolders (with '\\') */
    uint64_t size;
    int archive_index;   /* index into ctx->decomp_bufs */
    uint64_t data_offset;
} ovl_entry_t;

typedef struct {
    unsigned char *raw;
    size_t raw_size;

    ovl_header_t header;

    unsigned char **decomp_bufs;  /* parallel to header.archives */
    size_t *decomp_sizes;
    int *owns_buf;                /* 1 if decomp_bufs[i] was allocated separately */

    ovl_entry_t *entries;
    int entry_count;
    int entry_capacity;
    int cur_index;

    wchar_t arc_path[1024];
    int dos_time;                 /* DOS date/time derived from the .ovl file's mtime */

    ovl_oodle_t oodle;
    int oodle_tried;
    int oodle_ok;

    tProcessDataProcW process_data_proc;
    tChangeVolProcW change_vol_proc;
} ovl_wcx_handle_t;

/* ---------------------------------------------------------------------------
 * Helper functions
 * ------------------------------------------------------------------------- */

static void ascii_to_wide(const char *s, wchar_t *out, size_t out_count) {
    size_t i = 0;
    for (; s[i] != '\0' && i < out_count - 1; i++) out[i] = (wchar_t)(unsigned char)s[i];
    out[i] = L'\0';
}

/* Extends an absolute path with the \\?\ prefix so CreateFileW/CreateDirectoryW
   don't fail on the classic MAX_PATH limit (260 characters) -- with long
   resource names (textures, materials) plus deep destination folders in TC
   that limit is reached quickly. UNC paths and already-prefixed paths are
   left unchanged. */
static void to_long_path(const wchar_t *path, wchar_t *out, size_t out_count) {
    if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
        wcsncpy(out, path, out_count - 1);
        out[out_count - 1] = L'\0';
        return;
    }
    if (path[0] == L'\\' && path[1] == L'\\') {
        /* UNC path: \\server\share\... -> \\?\UNC\server\share\... */
        _snwprintf(out, out_count - 1, L"\\\\?\\UNC\\%s", path + 2);
    } else {
        _snwprintf(out, out_count - 1, L"\\\\?\\%s", path);
    }
    out[out_count - 1] = L'\0';
}

static int read_whole_file_w(const wchar_t *path, unsigned char **out_buf, size_t *out_size) {
    *out_buf = NULL;
    *out_size = 0;

    wchar_t long_path[2200];
    to_long_path(path, long_path, 2200);

    HANDLE f = CreateFileW(long_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > 0x7FFFFFFFLL) {
        CloseHandle(f);
        return 0;
    }

    size_t sz = (size_t)size.QuadPart;
    unsigned char *buf = (unsigned char *)malloc(sz ? sz : 1);
    if (!buf) { CloseHandle(f); return 0; }

    size_t total_read = 0;
    while (total_read < sz) {
        DWORD to_read = (DWORD)((sz - total_read) > (1u << 24) ? (1u << 24) : (sz - total_read));
        DWORD got = 0;
        if (!ReadFile(f, buf + total_read, to_read, &got, NULL) || got == 0) {
            free(buf);
            CloseHandle(f);
            return 0;
        }
        total_read += got;
    }
    CloseHandle(f);

    *out_buf = buf;
    *out_size = sz;
    return 1;
}

static int file_exists_w(const wchar_t *path) {
    wchar_t long_path[2200];
    to_long_path(path, long_path, 2200);
    DWORD attr = GetFileAttributesW(long_path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int push_entry(ovl_wcx_handle_t *ctx, const char *ascii_name,
                       uint64_t size, int archive_index, uint64_t data_offset) {
    if (ctx->entry_count >= ctx->entry_capacity) {
        int new_cap = ctx->entry_capacity ? ctx->entry_capacity * 2 : 64;
        ovl_entry_t *n = (ovl_entry_t *)realloc(ctx->entries, (size_t)new_cap * sizeof(ovl_entry_t));
        if (!n) return 0;
        ctx->entries = n;
        ctx->entry_capacity = new_cap;
    }

    ovl_entry_t *e = &ctx->entries[ctx->entry_count++];
    memset(e, 0, sizeof(*e));

    size_t n = strlen(ascii_name);
    if (n > 590) n = 590;
    for (size_t i = 0; i < n; i++) {
        char c = ascii_name[i];
        e->name[i] = (wchar_t)(unsigned char)(c == '/' ? '\\' : c);
    }
    e->name[n] = L'\0';
    e->size = size;
    e->archive_index = archive_index;
    e->data_offset = data_offset;
    return 1;
}

/* Reproduces Python-style slice semantics (decomp[start:start+len]): start
   and end are clamped to total_size instead of rejected on overflow.
   Externally loaded OVS archives don't carry a complete buffer-size table,
   so a naive bounds check would reject data that a lenient slice operation
   would still resolve to "rest of the buffer". Reproducing that clamping
   behavior keeps output identical to what a Python-based reference
   implementation would produce for these edge cases. */
static void py_slice_bounds(uint64_t total_size, uint64_t start, uint64_t requested_len,
                             uint64_t *out_start, uint64_t *out_len) {
    uint64_t s = (start > total_size) ? total_size : start;
    uint64_t e = s + requested_len;
    if (e > total_size || e < s) e = total_size;
    *out_start = s;
    *out_len = e - s;
}

/* Processes the pool and buffer data of a decompressed archive and appends
   the corresponding entries. decomp/decomp_size belong to arc. */
static void build_entries_for_archive(ovl_wcx_handle_t *ctx, int arc_idx,
                                       const unsigned char *decomp, size_t decomp_size) {
    const ovl_archive_t *arc = &ctx->header.archives[arc_idx];
    int version = ctx->header.version;

    uint32_t set_data_size = arc->set_data_size;
    uint32_t pool_region_sz = (arc->pools_end >= arc->pools_start) ? (arc->pools_end - arc->pools_start) : 0;

    /* Pools */
    ovl_pool_t *pools = NULL;
    int pool_count = 0;
    if (ovl_parse_mempools(decomp, decomp_size, arc, version, &pools, &pool_count)) {
        int unknown_idx = 0;
        for (int i = 0; i < pool_count; i++) {
            uint64_t raw_start = (uint64_t)set_data_size + pools[i].offset;
            uint64_t start, sz;
            py_slice_bounds(decomp_size, raw_start, pools[i].size, &start, &sz);
            if (sz == 0) continue;

            const ovl_file_t *finfo = ovl_find_file_by_hash(&ctx->header, pools[i].file_hash);
            char full[400];

            if (finfo) {
                char name_buf[280];
                char ext_buf[100];
                strncpy(name_buf, finfo->name, sizeof(name_buf) - 1); name_buf[sizeof(name_buf) - 1] = '\0';
                ovl_sanitize(name_buf);
                strncpy(ext_buf, finfo->ext, sizeof(ext_buf) - 1); ext_buf[sizeof(ext_buf) - 1] = '\0';
                if (ext_buf[0] != '\0' && ext_buf[0] != '.') {
                    char tmp[100];
                    ovl_sanitize(ext_buf);
                    _snprintf(tmp, sizeof(tmp) - 1, ".%s", ext_buf);
                    tmp[sizeof(tmp) - 1] = '\0';
                    strncpy(ext_buf, tmp, sizeof(ext_buf) - 1); ext_buf[sizeof(ext_buf) - 1] = '\0';
                } else {
                    ovl_sanitize(ext_buf);
                }
                _snprintf(full, sizeof(full) - 1, "%s%s", name_buf, ext_buf);
                full[sizeof(full) - 1] = '\0';
            } else {
                const char *detected = ovl_detect_ext(decomp + start, (size_t)sz);
                const char *ext = detected ? detected : ".bin";
                _snprintf(full, sizeof(full) - 1, "unknown-%04d%s", unknown_idx++, ext);
                full[sizeof(full) - 1] = '\0';
            }

            push_entry(ctx, full, sz, arc_idx, start);
        }
    }
    free(pools);

    /* Buffers (bulk data: texture mips, model vertices, ...) */
    if (arc->num_buffers > 0) {
        uint32_t *sizes = NULL;
        int sizes_count = 0;
        int parse_ok = ovl_parse_buffer_sizes(decomp, decomp_size, arc, version, &sizes, &sizes_count);
        DBG("  buf-parse: ok=%d count=%d pool_region_sz=%u set_data_size=%u decomp_size=%zu\n",
            parse_ok, sizes_count, pool_region_sz, set_data_size, decomp_size);
        if (parse_ok) {
            uint64_t pos = (uint64_t)set_data_size + pool_region_sz;
            for (int i = 0; i < sizes_count; i++) {
                uint64_t raw_bsz = sizes[i];
                uint64_t start, sz;
                py_slice_bounds(decomp_size, pos, raw_bsz, &start, &sz);
                DBG("    buf[%d] raw_size=%u pos=%llu -> sz=%llu\n", i, sizes[i], (unsigned long long)pos, (unsigned long long)sz);
                if (sz > 0) {
                    const char *detected = ovl_detect_ext(decomp + start, (size_t)sz);
                    const char *ext = detected ? detected : ".bin";
                    char full[160];
                    _snprintf(full, sizeof(full) - 1, "%s_buf%03d%s", arc->name, i, ext);
                    full[sizeof(full) - 1] = '\0';
                    push_entry(ctx, full, sz, arc_idx, start);
                }
                pos += raw_bsz; /* unclamped advance, see comment above on slice semantics */
            }
        }
        free(sizes);
    }
}

/* Decompresses/loads a single archive (STATIC or OVS) and appends its
   entries. Errors are silently skipped -- the archive then simply
   contributes no entries instead of aborting the whole open operation. */
static void process_archive(ovl_wcx_handle_t *ctx, int arc_idx,
                             const wchar_t *dir_w, const wchar_t *stem_w) {
    const ovl_archive_t *arc = &ctx->header.archives[arc_idx];
    unsigned char *decomp = NULL;
    size_t decomp_size = 0;
    int owns = 0;

    DBG("[archive %d] name=%s pools=%u bufs=%u set_data=%u compressed_size=%u uncompressed_size=%llu compression=%d\n",
        arc_idx, arc->name, arc->num_pools, arc->num_buffers, arc->set_data_size,
        arc->compressed_size, (unsigned long long)arc->uncompressed_size, ctx->header.compression);

    if (_stricmp(arc->name, "STATIC") == 0) {
        uint32_t cs = arc->compressed_size;
        if (cs == 0) { DBG("  -> STATIC is empty (compressed_size=0)\n"); return; }
        if ((uint64_t)ctx->header.data_start + cs > ctx->raw_size) {
            DBG("  -> STATIC data outside file bounds (data_start=%u + cs=%u > raw_size=%zu)\n",
                ctx->header.data_start, cs, ctx->raw_size);
            return;
        }

        const unsigned char *compressed = ctx->raw + ctx->header.data_start;

        if (ctx->header.compression == OVL_COMPRESSION_OODLE) {
            if (!ctx->oodle_tried) {
                ctx->oodle_tried = 1;
                /* oo2core_*.dll usually sits in the game's install root, while
                   .ovl files can be nested very deep (e.g. 9 levels below the
                   root in JWE2) -- so search generously in parent directories
                   too. */
                ctx->oodle_ok = ovl_oodle_load_upward(dir_w, 16, &ctx->oodle);
            }
            if (!ctx->oodle_ok) { DBG("  -> Oodle DLL not found (also not in parent directories of %ls)\n", dir_w); return; }

            uint64_t out_size64 = arc->uncompressed_size;
            if (out_size64 == 0 || out_size64 > 0xFFFFFFFFull) { DBG("  -> invalid uncompressed_size\n"); return; }
            decomp = (unsigned char *)malloc((size_t)out_size64);
            if (!decomp) return;
            if (!ovl_oodle_decompress(&ctx->oodle, compressed, cs, decomp, (size_t)out_size64)) {
                DBG("  -> Oodle decompression failed\n");
                free(decomp);
                return;
            }
            decomp_size = (size_t)out_size64;
            owns = 1;
        } else if (ctx->header.compression == OVL_COMPRESSION_ZLIB) {
            if (cs < 2) return;
            uint64_t out_size64 = arc->uncompressed_size;
            if (out_size64 == 0 || out_size64 > 0xFFFFFFFFull) { DBG("  -> invalid uncompressed_size\n"); return; }
            decomp = (unsigned char *)malloc((size_t)out_size64);
            if (!decomp) return;
            if (!ovl_zlib_inflate_raw(compressed + 2, cs - 2, decomp, (size_t)out_size64)) {
                DBG("  -> ZLIB decompression failed (cs=%u out=%llu)\n", cs, (unsigned long long)out_size64);
                free(decomp);
                return;
            }
            decomp_size = (size_t)out_size64;
            owns = 1;
        } else {
            /* NONE / unknown: use the raw bytes directly */
            decomp = (unsigned char *)compressed;
            decomp_size = cs;
            owns = 0;
        }
    } else {
        char arcname_lower[64];
        strncpy(arcname_lower, arc->name, sizeof(arcname_lower) - 1);
        arcname_lower[sizeof(arcname_lower) - 1] = '\0';
        for (char *p = arcname_lower; *p; p++) *p = (char)tolower((unsigned char)*p);

        wchar_t arcname_lower_w[64];
        ascii_to_wide(arcname_lower, arcname_lower_w, 64);

        wchar_t cand1[1024], cand2[1024];
        _snwprintf(cand1, 1023, L"%s%s.ovs.%s", dir_w, stem_w, arcname_lower_w); cand1[1023] = 0;
        _snwprintf(cand2, 1023, L"%s%s.ovs", dir_w, stem_w); cand2[1023] = 0;

        const wchar_t *chosen = NULL;
        if (file_exists_w(cand1)) chosen = cand1;
        else if (file_exists_w(cand2)) chosen = cand2;
        if (!chosen) {
            DBG("  -> OVS not found, checked: %ls | %ls\n", cand1, cand2);
            return;
        }

        DBG("  -> OVS found: %ls\n", chosen);
        if (!read_whole_file_w(chosen, &decomp, &decomp_size)) { DBG("  -> failed to read OVS\n"); return; }
        owns = 1;
    }

    ctx->decomp_bufs[arc_idx] = decomp;
    ctx->decomp_sizes[arc_idx] = decomp_size;
    ctx->owns_buf[arc_idx] = owns;

    int before = ctx->entry_count;
    build_entries_for_archive(ctx, arc_idx, decomp, decomp_size);
    DBG("  -> decomp_size=%zu new entries=%d\n", decomp_size, ctx->entry_count - before);
}

static void ensure_directories_w(const wchar_t *path) {
    wchar_t buf[2200];
    to_long_path(path, buf, 2200);

    size_t start = 4; /* skip "\\?\" */
    /* skip drive letter ("C:\") if present */
    if (wcslen(buf) > start + 2 && buf[start + 1] == L':' &&
        (buf[start + 2] == L'\\' || buf[start + 2] == L'/')) {
        start += 3;
    }

    for (size_t i = start; buf[i]; i++) {
        if (buf[i] == L'\\' || buf[i] == L'/') {
            wchar_t saved = buf[i];
            buf[i] = L'\0';
            CreateDirectoryW(buf, NULL);
            buf[i] = saved;
        }
    }
}

/* ---------------------------------------------------------------------------
 * WCX interface
 * ------------------------------------------------------------------------- */

HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW *ArchiveData) {
    ArchiveData->OpenResult = 0;

    unsigned char *raw = NULL;
    size_t raw_size = 0;
    if (!read_whole_file_w(ArchiveData->ArcName, &raw, &raw_size)) {
        ArchiveData->OpenResult = E_EOPEN;
        return NULL;
    }

    ovl_header_t header;
    char errbuf[256];
    if (!ovl_parse_header(raw, raw_size, &header, errbuf, sizeof(errbuf))) {
        DBG("ovl_parse_header failed: %s\n", errbuf);
        free(raw);
        ArchiveData->OpenResult = E_UNKNOWN_FORMAT;
        return NULL;
    }
    DBG("Header OK: version=%d compression=%d files=%d archives=%d data_start=%u raw_size=%zu\n",
        header.version, header.compression, header.num_files, header.num_archives,
        header.data_start, raw_size);

    ovl_wcx_handle_t *ctx = (ovl_wcx_handle_t *)calloc(1, sizeof(ovl_wcx_handle_t));
    if (!ctx) {
        ovl_free_header(&header);
        free(raw);
        ArchiveData->OpenResult = E_NO_MEMORY;
        return NULL;
    }

    ctx->raw = raw;
    ctx->raw_size = raw_size;
    ctx->header = header;
    wcsncpy(ctx->arc_path, ArchiveData->ArcName, 1023);
    ctx->arc_path[1023] = L'\0';

    ctx->decomp_bufs = (unsigned char **)calloc(header.num_archives ? header.num_archives : 1, sizeof(unsigned char *));
    ctx->decomp_sizes = (size_t *)calloc(header.num_archives ? header.num_archives : 1, sizeof(size_t));
    ctx->owns_buf = (int *)calloc(header.num_archives ? header.num_archives : 1, sizeof(int));
    if (!ctx->decomp_bufs || !ctx->decomp_sizes || !ctx->owns_buf) {
        free(ctx->decomp_bufs); free(ctx->decomp_sizes); free(ctx->owns_buf);
        ovl_free_header(&ctx->header);
        free(ctx->raw);
        free(ctx);
        ArchiveData->OpenResult = E_NO_MEMORY;
        return NULL;
    }

    /* Use the .ovl file's own mtime for all entries (the format itself
       carries no per-file timestamps). */
    wchar_t ovl_long_path[2200];
    to_long_path(ArchiveData->ArcName, ovl_long_path, 2200);
    HANDLE fh = CreateFileW(ovl_long_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    FILETIME ft_local = {0};
    if (fh != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        if (GetFileTime(fh, NULL, NULL, &ft)) {
            FileTimeToLocalFileTime(&ft, &ft_local);
        }
        CloseHandle(fh);
    }
    WORD fatdate = 0, fattime = 0;
    FileTimeToDosDateTime(&ft_local, &fatdate, &fattime);
    ctx->dos_time = ((int)fatdate << 16) | fattime;

    wchar_t drive[8] = {0}, dir_part[1024] = {0}, fname[512] = {0}, ext_part[64] = {0};
    _wsplitpath_s(ArchiveData->ArcName, drive, 8, dir_part, 1024, fname, 512, ext_part, 64);
    wchar_t dir_w[1024];
    _snwprintf(dir_w, 1023, L"%s%s", drive, dir_part); dir_w[1023] = 0;
    wchar_t stem_w[512];
    wcsncpy(stem_w, fname, 511); stem_w[511] = 0;

    for (int i = 0; i < header.num_archives; i++) {
        process_archive(ctx, i, dir_w, stem_w);
    }

    return (HANDLE)ctx;
}

/* ---------------------------------------------------------------------------
 * ANSI dummy exports
 *
 * Total Commander only recognizes a wcx64 DLL as a valid packer plugin if
 * the classic ANSI functions are exported alongside the *W functions, as a
 * kind of "signature" -- even for a pure Unicode plugin. Without these
 * exports, TC reports "Error, could not load plugin!" right at load time,
 * before OpenArchiveW is ever called. These functions are never actually
 * invoked by TC while operating in Unicode mode.
 * ------------------------------------------------------------------------- */

HANDLE __stdcall OpenArchive(tOpenArchiveData *ArchiveData) {
    ArchiveData->OpenResult = E_NOT_SUPPORTED;
    return NULL;
}

int __stdcall ReadHeader(HANDLE hArcData, tHeaderData *HeaderData) {
    (void)hArcData; (void)HeaderData;
    return E_END_ARCHIVE;
}

int __stdcall ReadHeaderEx(HANDLE hArcData, tHeaderDataEx *HeaderDataEx) {
    (void)hArcData; (void)HeaderDataEx;
    return E_END_ARCHIVE;
}

int __stdcall ProcessFile(HANDLE hArcData, int Operation, char *DestPath, char *DestName) {
    (void)hArcData; (void)Operation; (void)DestPath; (void)DestName;
    return E_NOT_SUPPORTED;
}

void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVolProc1) {
    (void)hArcData; (void)pChangeVolProc1;
}

int __stdcall PackFiles(char *PackedFile, char *SubPath, char *SrcPath, char *AddList, int Flags) {
    (void)PackedFile; (void)SubPath; (void)SrcPath; (void)AddList; (void)Flags;
    return E_NOT_SUPPORTED;
}

int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW *HeaderDataEx) {
    ovl_wcx_handle_t *ctx = (ovl_wcx_handle_t *)hArcData;
    if (ctx->cur_index >= ctx->entry_count) return E_END_ARCHIVE;

    ovl_entry_t *e = &ctx->entries[ctx->cur_index];
    memset(HeaderDataEx, 0, sizeof(*HeaderDataEx));

    wcsncpy(HeaderDataEx->FileName, e->name, 1023);
    wcsncpy(HeaderDataEx->ArcName, ctx->arc_path, 1023);
    HeaderDataEx->PackSize = (unsigned int)(e->size & 0xFFFFFFFFu);
    HeaderDataEx->PackSizeHigh = (unsigned int)(e->size >> 32);
    HeaderDataEx->UnpSize = HeaderDataEx->PackSize;
    HeaderDataEx->UnpSizeHigh = HeaderDataEx->PackSizeHigh;
    HeaderDataEx->FileTime = ctx->dos_time;
    HeaderDataEx->FileAttr = FILE_ATTRIBUTE_ARCHIVE;
    HeaderDataEx->HostOS = 0;

    ctx->cur_index++;
    return 0;
}

int __stdcall ProcessFileW(HANDLE hArcData, int Operation, wchar_t *DestPath, wchar_t *DestName) {
    ovl_wcx_handle_t *ctx = (ovl_wcx_handle_t *)hArcData;
    if (ctx->cur_index <= 0 || ctx->cur_index > ctx->entry_count) return E_BAD_ARCHIVE;

    ovl_entry_t *e = &ctx->entries[ctx->cur_index - 1];

    if (Operation == PK_SKIP || Operation == PK_TEST) {
        return 0;
    }
    if (Operation != PK_EXTRACT) {
        return 0;
    }

    wchar_t full_path[2048];
    if (DestPath && DestPath[0]) {
        _snwprintf(full_path, 2047, L"%s\\%s", DestPath, DestName);
    } else {
        wcsncpy(full_path, DestName, 2047);
    }
    full_path[2047] = L'\0';

    wchar_t long_path[2200];
    to_long_path(full_path, long_path, 2200);

    ensure_directories_w(full_path);

    HANDLE f = CreateFileW(long_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return E_ECREATE;

    unsigned char *buf = ctx->decomp_bufs[e->archive_index];
    size_t buf_size = ctx->decomp_sizes[e->archive_index];
    if (!buf || e->data_offset + e->size > buf_size) {
        CloseHandle(f);
        return E_BAD_DATA;
    }

    const unsigned char *src = buf + e->data_offset;
    uint64_t remaining = e->size;
    const DWORD CHUNK = 1u << 20;
    int aborted = 0;

    while (remaining > 0) {
        DWORD to_write = (DWORD)((remaining < CHUNK) ? remaining : CHUNK);
        DWORD written = 0;
        if (!WriteFile(f, src, to_write, &written, NULL) || written != to_write) {
            CloseHandle(f);
            DeleteFileW(long_path);
            return E_EWRITE;
        }
        src += to_write;
        remaining -= to_write;

        if (ctx->process_data_proc) {
            if (ctx->process_data_proc(DestName, (int)to_write) == 0) {
                aborted = 1;
                break;
            }
        }
    }
    CloseHandle(f);

    if (aborted) {
        DeleteFileW(long_path);
        return E_EABORTED;
    }

    HANDLE f2 = CreateFileW(long_path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f2 != INVALID_HANDLE_VALUE) {
        FILETIME ft_local;
        WORD fatdate = (WORD)(ctx->dos_time >> 16), fattime = (WORD)(ctx->dos_time & 0xFFFF);
        if (DosDateTimeToFileTime(fatdate, fattime, &ft_local)) {
            FILETIME ft_utc;
            LocalFileTimeToFileTime(&ft_local, &ft_utc);
            SetFileTime(f2, &ft_utc, &ft_utc, &ft_utc);
        }
        CloseHandle(f2);
    }

    return 0;
}

int __stdcall CloseArchive(HANDLE hArcData) {
    ovl_wcx_handle_t *ctx = (ovl_wcx_handle_t *)hArcData;
    if (!ctx) return 0;

    if (ctx->decomp_bufs) {
        for (int i = 0; i < ctx->header.num_archives; i++) {
            if (ctx->owns_buf && ctx->owns_buf[i] && ctx->decomp_bufs[i]) free(ctx->decomp_bufs[i]);
        }
    }
    free(ctx->decomp_bufs);
    free(ctx->decomp_sizes);
    free(ctx->owns_buf);
    free(ctx->entries);
    ovl_free_header(&ctx->header);
    free(ctx->raw);
    if (ctx->oodle_ok) ovl_oodle_unload(&ctx->oodle);
    free(ctx);
    return 0;
}

void __stdcall SetChangeVolProcW(HANDLE hArcData, tChangeVolProcW pChangeVolProc1) {
    ovl_wcx_handle_t *ctx = (ovl_wcx_handle_t *)hArcData;
    ctx->change_vol_proc = pChangeVolProc1;
}

void __stdcall SetProcessDataProcW(HANDLE hArcData, tProcessDataProcW pProcessDataProc) {
    ovl_wcx_handle_t *ctx = (ovl_wcx_handle_t *)hArcData;
    ctx->process_data_proc = pProcessDataProc;
}

int __stdcall GetPackerCaps(void) {
    return 0; /* read-only: no NEW/MODIFY/DELETE/ENCRYPT */
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
