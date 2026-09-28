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

/* This DLL's own module handle, set in DllMain. Used to locate the plugin's
   own directory as a fallback place to look for the Oodle DLL. */
static HMODULE g_hinst = NULL;

/* ---------------------------------------------------------------------------
 * Data structures
 * ------------------------------------------------------------------------- */

typedef struct {
    wchar_t name[600];   /* display name including subfolders (with '\\') */
    uint64_t size;
    int archive_index;   /* index into ctx->decomp_bufs / ctx->synth_bufs */
    uint64_t data_offset;
    int is_synth;        /* 1 if data_offset/size refer to ctx->synth_bufs[archive_index]
                             instead of ctx->decomp_bufs[archive_index] (reconstructed
                             "structured data" content, e.g. resolved .assetpkg XML);
                             ENTRY_INFO if they refer to ctx->info_buf (generated text,
                             e.g. the "Ref - x.ovl.txt" include notes) */
} ovl_entry_t;

#define ENTRY_INFO 2

typedef struct {
    unsigned char *raw;
    size_t raw_size;

    ovl_header_t header;

    unsigned char **decomp_bufs;  /* parallel to header.archives */
    size_t *decomp_sizes;
    int *owns_buf;                /* 1 if decomp_bufs[i] was allocated separately */

    unsigned char **synth_bufs;   /* parallel to header.archives; reconstructed
                                     "structured data" content (always owned) */
    size_t *synth_sizes;

    unsigned char *info_buf;      /* generated text entries (ENTRY_INFO), owned */
    size_t info_size;

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

/* Returns the directory this DLL itself lives in (no trailing backslash).
   Used as a fallback location for the Oodle DLL: dropping oo2core_*.dll
   next to the plugin lets it decompress Oodle archives even when the .ovl
   being opened isn't inside a full game installation (e.g. a standalone
   test file). Returns 0 on failure. */
static int get_plugin_dir(wchar_t *out, size_t out_count) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_hinst, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 0;

    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    *slash = L'\0';

    wcsncpy(out, path, out_count - 1);
    out[out_count - 1] = L'\0';
    return 1;
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
    e->is_synth = 0;
    return 1;
}

/* Same as push_entry, but the data lives in ctx->synth_bufs[archive_index]
   (reconstructed content) instead of ctx->decomp_bufs[archive_index]. */
static int push_synth_entry(ovl_wcx_handle_t *ctx, const char *ascii_name,
                             uint64_t size, int archive_index, uint64_t data_offset) {
    if (!push_entry(ctx, ascii_name, size, archive_index, data_offset)) return 0;
    ctx->entries[ctx->entry_count - 1].is_synth = 1;
    return 1;
}

/* 1 if an entry with this name was already pushed (case-insensitive, like
   the file system TC extracts to). */
static int entry_name_taken(const ovl_wcx_handle_t *ctx, const char *ascii_name) {
    wchar_t w[600];
    ascii_to_wide(ascii_name, w, sizeof(w) / sizeof(w[0]));
    for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
    for (int i = 0; i < ctx->entry_count; i++) {
        if (_wcsicmp(ctx->entries[i].name, w) == 0) return 1;
    }
    return 0;
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

/* Resource types whose real content is not a plain contiguous byte range at
   the RootEntry's data_offset, but requires the engine's separate "structured
   data"/symbol-relocation deserialization system (fResOverlayData::
   RequestStructuredData). Without a dedicated resolver the naive offset only
   resolves to a meaningless index stub (often just a few zero bytes), not the
   real content, so such entries are skipped entirely rather than written out
   as garbage. assetpkg/world/xmlconfig now have dedicated resolvers (see
   resolve_assetpkg/resolve_worlddesc/resolve_xmlconfig above) and are handled
   before this fallback is ever reached; kept as a safety net for any other
   structured type encountered in the future -- currently none are known, so
   this always returns 0. */
static int is_structured_data_ext(const char *ext) {
    if (!ext) return 0;
    char lower[100];
    strncpy(lower, ext, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
    (void)lower;
    return 0;
}

/* Name of the game these files come from, used verbatim in the "game" XML
   attribute of resolved structured-data types (matches the reference
   exporter's output for this preset). */
#define GAME_NAME "Jurassic World Evolution 2"

/* Fragments sorted by (link_pool, link_offset) for binary search: each
   Fragment says "the pointer field at (link_pool, link_offset) resolves to
   the data at (struct_pool, struct_offset)". */
static int cmp_fragment_by_link(const void *a, const void *b) {
    const ovl_fragment_t *fa = (const ovl_fragment_t *)a;
    const ovl_fragment_t *fb = (const ovl_fragment_t *)b;
    if (fa->link_pool != fb->link_pool) return (fa->link_pool > fb->link_pool) - (fa->link_pool < fb->link_pool);
    return (fa->link_offset > fb->link_offset) - (fa->link_offset < fb->link_offset);
}

static const ovl_fragment_t *find_link(const ovl_fragment_t *sorted, int count,
                                        int32_t pool_index, uint32_t offset) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const ovl_fragment_t *f = &sorted[mid];
        int cmp = (f->link_pool != pool_index) ? (f->link_pool > pool_index ? 1 : -1)
                                                : (f->link_offset > offset) - (f->link_offset < offset);
        if (cmp == 0) return f;
        if (cmp < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* Reconstructs a Casino:AssetPackageRes:assetpkg file's real XML content.
   See unpack_ovl.py's _resolve_assetpkg for the full explanation: on disk,
   an AssetpkgRoot MemStruct is just an 8-byte Pointer (to a NUL-terminated
   asset path string) followed by an 8-byte 'shared' Uint64 (usually 0).
   Verified byte-for-byte against a known-good reference extraction of
   test/Init.ovl's 'acu' entry. Returns a malloc'd buffer (caller frees) or
   NULL if the pointer couldn't be resolved. */
static unsigned char *resolve_assetpkg(const unsigned char *decomp, size_t decomp_size,
                                        const ovl_pool_t *pools, int pool_count,
                                        uint32_t pool_region_start,
                                        const ovl_fragment_t *frags_sorted, int frag_count,
                                        int32_t pool_index, uint32_t data_offset,
                                        size_t *out_size) {
    const ovl_fragment_t *link = find_link(frags_sorted, frag_count, pool_index, data_offset);
    if (!link) return NULL;
    int32_t spool = link->struct_pool;
    if (spool < 0 || spool >= pool_count) return NULL;
    uint64_t str_start = (uint64_t)pool_region_start + pools[spool].offset + link->struct_offset;
    if (str_start >= decomp_size) return NULL;
    size_t max_len = decomp_size - (size_t)str_start;
    if (max_len > 4096) max_len = 4096;
    const unsigned char *str_begin = decomp + str_start;
    size_t str_len = 0;
    while (str_len < max_len && str_begin[str_len] != 0) str_len++;
    if (str_len == max_len) return NULL; /* no NUL terminator found within bound */

    /* 'shared' Uint64 follows the asset_path pointer (offset +8); only
       rendered as an XML attribute when non-zero. */
    uint64_t shared_val = 0;
    if (pool_index >= 0 && pool_index < pool_count) {
        uint64_t shared_off = (uint64_t)pool_region_start + pools[pool_index].offset + data_offset + 8;
        if (shared_off + 8 <= decomp_size) memcpy(&shared_val, decomp + shared_off, 8);
    }

    char shared_attr[48];
    shared_attr[0] = '\0';
    if (shared_val != 0) _snprintf(shared_attr, sizeof(shared_attr) - 1, " shared=\"%llu\"", (unsigned long long)shared_val);

    size_t cap = str_len + sizeof(shared_attr) + 128;
    unsigned char *out = (unsigned char *)malloc(cap);
    if (!out) return NULL;
    int n = _snprintf((char *)out, cap - 1, "<AssetpkgRoot%s game=\"%s\">\n\t<asset_path>%.*s</asset_path>\n</AssetpkgRoot>\n",
                       shared_attr, GAME_NAME, (int)str_len, (const char *)str_begin);
    if (n < 0) { free(out); return NULL; }
    *out_size = (size_t)n;
    return out;
}

/* Reads a NUL-terminated string at (pool_index, offset), resolved via the
   Fragment link table (like resolve_assetpkg's target lookup, but taking an
   already-known target location directly instead of looking up a pointer
   field). Returns a pointer into decomp and sets *out_len; NULL if no NUL
   terminator is found within a sane bound. */
static const char *read_zstring_at(const unsigned char *decomp, size_t decomp_size,
                                    const ovl_pool_t *pools, int pool_count,
                                    uint32_t pool_region_start, int32_t pool_index, uint32_t offset,
                                    size_t *out_len) {
    if (pool_index < 0 || pool_index >= pool_count) return NULL;
    uint64_t start = (uint64_t)pool_region_start + pools[pool_index].offset + offset;
    if (start >= decomp_size) return NULL;
    size_t max_len = decomp_size - (size_t)start;
    if (max_len > 4096) max_len = 4096;
    const char *begin = (const char *)(decomp + start);
    size_t len = 0;
    while (len < max_len && begin[len] != 0) len++;
    if (len == max_len) return NULL;
    *out_len = len;
    return begin;
}

/* WorldHeader ZStringList sub-elements (asset_pkgs/prefabs) always carry this
   pool_type attribute for this game preset (derived from a MIME constant tied
   to the resource type, not stored per-pool on disk; verified against
   test/Init.ovl -- hardcoding avoids implementing the whole MIME/pool-type
   lookup system for a single constant). */
#define WORLD_ZSTRINGLIST_POOL_TYPE "4"

/* Small growable append-only buffer, used to collect reconstructed
   "structured data" content (e.g. resolved .assetpkg/.world XML) for one
   archive. Its final buffer is handed to
   ctx->synth_bufs[arc_idx]/synth_sizes[arc_idx] once build_entries_for_archive
   is done, since decomp itself may not be a private, reallocatable buffer
   (e.g. uncompressed STATIC data points directly into the read-only mmapped
   .ovl file). */
typedef struct { unsigned char *data; size_t size; size_t cap; } growbuf_t;

static uint64_t growbuf_append(growbuf_t *gb, const unsigned char *bytes, size_t n) {
    if (gb->size + n > gb->cap) {
        size_t new_cap = gb->cap ? gb->cap * 2 : 4096;
        while (new_cap < gb->size + n) new_cap *= 2;
        unsigned char *p = (unsigned char *)realloc(gb->data, new_cap);
        if (!p) return (uint64_t)-1;
        gb->data = p;
        gb->cap = new_cap;
    }
    memcpy(gb->data + gb->size, bytes, n);
    uint64_t off = gb->size;
    gb->size += n;
    return off;
}

/* Appends "\t\t<pointer>{name}</pointer>\n" for each resolved ZStringList
   element to gb. Returns 0 on failure (unresolvable pointer). */
static int append_zstringlist_items(growbuf_t *gb, const unsigned char *decomp, size_t decomp_size,
                                     const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                     const ovl_fragment_t *frags_sorted, int frag_count,
                                     int32_t array_pool, uint32_t array_offset, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        const ovl_fragment_t *elem = find_link(frags_sorted, frag_count, array_pool, (uint32_t)(array_offset + i * 8));
        if (!elem) return 0;
        size_t slen;
        const char *s = read_zstring_at(decomp, decomp_size, pools, pool_count, pool_region_start,
                                         elem->struct_pool, elem->struct_offset, &slen);
        if (!s) return 0;
        char line[4200];
        int n = _snprintf(line, sizeof(line) - 1, "\t\t<pointer>%.*s</pointer>\n", (int)slen, s);
        if (n < 0) return 0;
        if (growbuf_append(gb, (const unsigned char *)line, (size_t)n) == (uint64_t)-1) return 0;
    }
    return 1;
}

/* Reconstructs a Casino:WorldDesc:world file's real XML content. See
   unpack_ovl.py's _resolve_worlddesc for the full field-layout explanation
   (80-byte fixed WorldHeader MemStruct, verified against test/Init.ovl's
   'classic_03_tropical' entry). Appends the result directly to `out` (a
   per-archive growbuf) since its size isn't known upfront (variable number of
   asset_pkgs/prefabs pointers); returns the byte offset of the start of the
   written content within `out`, or (uint64_t)-1 on failure. */
static uint64_t resolve_worlddesc(growbuf_t *out, const unsigned char *decomp, size_t decomp_size,
                                   const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                   const ovl_fragment_t *frags_sorted, int frag_count,
                                   int32_t pool_index, uint32_t data_offset, size_t *out_size) {
    if (pool_index < 0 || pool_index >= pool_count) return (uint64_t)-1;
    uint64_t struct_start = (uint64_t)pool_region_start + pools[pool_index].offset + data_offset;
    if (struct_start + 80 > decomp_size) return (uint64_t)-1;
    const unsigned char *raw = decomp + struct_start;

    uint64_t world_type, asset_pkgs_count, prefabs_count;
    memcpy(&world_type, raw + 0, 8);
    memcpy(&asset_pkgs_count, raw + 16, 8);
    memcpy(&prefabs_count, raw + 64, 8);

    const ovl_fragment_t *lua_link = find_link(frags_sorted, frag_count, pool_index, data_offset + 24);
    if (!lua_link) return (uint64_t)-1;
    size_t lua_len;
    const char *lua_name = read_zstring_at(decomp, decomp_size, pools, pool_count, pool_region_start,
                                            lua_link->struct_pool, lua_link->struct_offset, &lua_len);
    if (!lua_name) return (uint64_t)-1;

    uint64_t start_off = out->size;
    char head[128];
    int n = _snprintf(head, sizeof(head) - 1, "<WorldHeader world_type=\"%llu\" game=\"%s\">\n",
                       (unsigned long long)world_type, GAME_NAME);
    if (n < 0 || growbuf_append(out, (const unsigned char *)head, (size_t)n) == (uint64_t)-1) return (uint64_t)-1;

    if (asset_pkgs_count > 0) {
        const ovl_fragment_t *arr = find_link(frags_sorted, frag_count, pool_index, data_offset + 8);
        if (!arr) return (uint64_t)-1;
        const char *tag_open = "\t<asset_pkgs pool_type=\"" WORLD_ZSTRINGLIST_POOL_TYPE "\">\n";
        growbuf_append(out, (const unsigned char *)tag_open, strlen(tag_open));
        if (!append_zstringlist_items(out, decomp, decomp_size, pools, pool_count, pool_region_start,
                                       frags_sorted, frag_count, arr->struct_pool, arr->struct_offset, asset_pkgs_count))
            return (uint64_t)-1;
        const char *tag_close = "\t</asset_pkgs>\n";
        growbuf_append(out, (const unsigned char *)tag_close, strlen(tag_close));
    }

    char lua_elem[4200];
    n = _snprintf(lua_elem, sizeof(lua_elem) - 1, "\t<lua_name>%.*s</lua_name>\n", (int)lua_len, lua_name);
    if (n < 0 || growbuf_append(out, (const unsigned char *)lua_elem, (size_t)n) == (uint64_t)-1) return (uint64_t)-1;

    if (prefabs_count > 0) {
        const ovl_fragment_t *arr = find_link(frags_sorted, frag_count, pool_index, data_offset + 48);
        if (!arr) return (uint64_t)-1;
        const char *tag_open = "\t<prefabs pool_type=\"" WORLD_ZSTRINGLIST_POOL_TYPE "\">\n";
        growbuf_append(out, (const unsigned char *)tag_open, strlen(tag_open));
        if (!append_zstringlist_items(out, decomp, decomp_size, pools, pool_count, pool_region_start,
                                       frags_sorted, frag_count, arr->struct_pool, arr->struct_offset, prefabs_count))
            return (uint64_t)-1;
        const char *tag_close = "\t</prefabs>\n";
        growbuf_append(out, (const unsigned char *)tag_close, strlen(tag_close));
    }

    const char *tail = "</WorldHeader>\n";
    growbuf_append(out, (const unsigned char *)tail, strlen(tail));
    *out_size = (size_t)(out->size - start_off);
    return start_off;
}

/* ---------------------------------------------------------------------------
 * Minimal XML parser + tab-indenting serializer (for Casino:XMLConfig:xmlconfig)
 *
 * XmlconfigRoot's on-disk content is a single raw XML string (see
 * resolve_xmlconfig below), which the reference exporter nests under a
 * synthetic <xml_string> element inside <XmlconfigRoot game="...">, then
 * re-serializes the WHOLE tree with a tab-based pretty-printer (see
 * unpack_ovl.py's _xml_indent for the exact recipe this mirrors). This is a
 * small hand-rolled parser/printer covering exactly what real game config
 * files use (nested elements, attributes, leaf text) -- no comments/CDATA/
 * namespaces/processing instructions beyond the leading '<?xml ?>' prolog.
 * ------------------------------------------------------------------------- */

typedef struct { char *name; char *value; } xml_attr_t;
typedef struct xml_node {
    char *tag;
    xml_attr_t *attrs; int attr_count;
    char *text;                          /* NULL if this node has children instead */
    struct xml_node **children; int child_count;
} xml_node_t;

static xml_node_t *xml_node_new(void) { return (xml_node_t *)calloc(1, sizeof(xml_node_t)); }

static void xml_node_free(xml_node_t *n) {
    if (!n) return;
    free(n->tag);
    for (int i = 0; i < n->attr_count; i++) { free(n->attrs[i].name); free(n->attrs[i].value); }
    free(n->attrs);
    free(n->text);
    for (int i = 0; i < n->child_count; i++) xml_node_free(n->children[i]);
    free(n->children);
    free(n);
}

static char *xml_unescape(const char *s, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < len;) {
        if (s[i] == '&') {
            if (i + 4 <= len && strncmp(s + i, "&lt;", 4) == 0) { out[oi++] = '<'; i += 4; continue; }
            if (i + 4 <= len && strncmp(s + i, "&gt;", 4) == 0) { out[oi++] = '>'; i += 4; continue; }
            if (i + 5 <= len && strncmp(s + i, "&amp;", 5) == 0) { out[oi++] = '&'; i += 5; continue; }
            if (i + 6 <= len && strncmp(s + i, "&quot;", 6) == 0) { out[oi++] = '"'; i += 6; continue; }
            if (i + 6 <= len && strncmp(s + i, "&apos;", 6) == 0) { out[oi++] = '\''; i += 6; continue; }
            if (i + 1 < len && s[i + 1] == '#') {
                size_t j = i + 2; int hex = 0;
                if (j < len && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
                long val = 0; size_t k = j;
                while (k < len && s[k] != ';') {
                    char c = s[k];
                    int d = hex ? (isdigit((unsigned char)c) ? c - '0' : (isxdigit((unsigned char)c) ? tolower((unsigned char)c) - 'a' + 10 : -1))
                                : (isdigit((unsigned char)c) ? c - '0' : -1);
                    if (d < 0) break;
                    val = val * (hex ? 16 : 10) + d;
                    k++;
                }
                if (k < len && s[k] == ';') { out[oi++] = (char)val; i = k + 1; continue; }
            }
        }
        out[oi++] = s[i++];
    }
    out[oi] = '\0';
    return out;
}

static void xml_skip_ws(const char **p) { while (isspace((unsigned char)**p)) (*p)++; }

static char *xml_read_name(const char **p) {
    const char *start = *p;
    while (isalnum((unsigned char)**p) || **p == '_' || **p == ':' || **p == '-' || **p == '.') (*p)++;
    size_t n = (size_t)(*p - start);
    char *out = (char *)malloc(n + 1);
    if (out) { memcpy(out, start, n); out[n] = '\0'; }
    return out;
}

static void xml_add_child(xml_node_t *parent, xml_node_t *child) {
    xml_node_t **n = (xml_node_t **)realloc(parent->children, sizeof(xml_node_t *) * (size_t)(parent->child_count + 1));
    if (!n) { xml_node_free(child); return; }
    parent->children = n;
    parent->children[parent->child_count++] = child;
}

static void xml_add_attr(xml_node_t *node, char *name, char *value) {
    xml_attr_t *n = (xml_attr_t *)realloc(node->attrs, sizeof(xml_attr_t) * (size_t)(node->attr_count + 1));
    if (!n) { free(name); free(value); return; }
    node->attrs = n;
    node->attrs[node->attr_count].name = name;
    node->attrs[node->attr_count].value = value;
    node->attr_count++;
}

static xml_node_t *xml_parse_element(const char **p) {
    xml_skip_ws(p);
    if (**p != '<') return NULL;
    (*p)++;
    xml_node_t *node = xml_node_new();
    if (!node) return NULL;
    node->tag = xml_read_name(p);

    for (;;) {
        xml_skip_ws(p);
        if ((*p)[0] == '/' && (*p)[1] == '>') { *p += 2; return node; } /* self-closing, no text/children */
        if (**p == '>') { (*p)++; break; }
        if (**p == '\0') return node; /* malformed input, bail out gracefully */
        char *aname = xml_read_name(p);
        xml_skip_ws(p);
        if (**p == '=') {
            (*p)++;
            xml_skip_ws(p);
            char quote = **p;
            if (quote == '"' || quote == '\'') {
                (*p)++;
                const char *vstart = *p;
                while (**p && **p != quote) (*p)++;
                char *aval = xml_unescape(vstart, (size_t)(*p - vstart));
                if (**p == quote) (*p)++;
                xml_add_attr(node, aname, aval);
            } else {
                free(aname); /* malformed attribute, skip */
            }
        } else {
            free(aname);
        }
    }

    for (;;) {
        if ((*p)[0] == '<' && (*p)[1] == '/') {
            *p += 2;
            char *closename = xml_read_name(p);
            free(closename);
            xml_skip_ws(p);
            if (**p == '>') (*p)++;
            break;
        }
        if (**p == '<') {
            xml_node_t *child = xml_parse_element(p);
            if (child) xml_add_child(node, child);
            else break;
            continue;
        }
        if (**p == '\0') break;
        const char *tstart = *p;
        while (**p && **p != '<') (*p)++;
        if (node->child_count == 0) {
            free(node->text);
            node->text = xml_unescape(tstart, (size_t)(*p - tstart));
        }
    }
    return node;
}

static void xml_append_str(growbuf_t *out, const char *s) { growbuf_append(out, (const unsigned char *)s, strlen(s)); }

static void xml_append_escaped(growbuf_t *out, const char *s, int is_attr) {
    for (; *s; s++) {
        switch (*s) {
            case '&': xml_append_str(out, "&amp;"); break;
            case '<': xml_append_str(out, "&lt;"); break;
            case '>': xml_append_str(out, "&gt;"); break;
            case '"': if (is_attr) xml_append_str(out, "&quot;"); else growbuf_append(out, (const unsigned char *)s, 1); break;
            default: growbuf_append(out, (const unsigned char *)s, 1);
        }
    }
}

static void xml_append_tabs(growbuf_t *out, int level) {
    growbuf_append(out, (const unsigned char *)"\n", 1);
    for (int i = 0; i < level; i++) growbuf_append(out, (const unsigned char *)"\t", 1);
}

/* Tab-indented serialization matching the reference exporter byte-for-byte
   (see unpack_ovl.py's _xml_indent for the equivalent recipe, and the
   comment there for why printing "\n+(level+1)tabs" before each child and
   "\n+level tabs" once after the loop is mathematically equivalent to that
   tail-mutation recipe). */
static void xml_print_node(growbuf_t *out, const xml_node_t *n, int level) {
    xml_append_str(out, "<");
    xml_append_str(out, n->tag);
    for (int i = 0; i < n->attr_count; i++) {
        xml_append_str(out, " ");
        xml_append_str(out, n->attrs[i].name);
        xml_append_str(out, "=\"");
        xml_append_escaped(out, n->attrs[i].value, 1);
        xml_append_str(out, "\"");
    }
    /* No children AND no text (i.e. was self-closing "<tag/>" in the source,
       distinct from an explicit empty "<tag></tag>") -> ElementTree renders
       this as a self-closing "<tag ... />" (with a space before "/>"). */
    if (n->child_count == 0 && !n->text) {
        xml_append_str(out, " />");
        return;
    }
    xml_append_str(out, ">");
    if (n->child_count > 0) {
        for (int i = 0; i < n->child_count; i++) {
            xml_append_tabs(out, level + 1);
            xml_print_node(out, n->children[i], level + 1);
        }
        xml_append_tabs(out, level);
    } else {
        xml_append_escaped(out, n->text, 0);
    }
    xml_append_str(out, "</");
    xml_append_str(out, n->tag);
    xml_append_str(out, ">");
}

/* Reconstructs a Casino:XMLConfig:xmlconfig file's real XML content. See
   unpack_ovl.py's _resolve_xmlconfig for the full explanation: XmlconfigRoot
   is a single 8-byte Pointer field (xml_string) at offset 0, pointing to a
   NUL-terminated raw XML string with a leading '<?xml ...?>' declaration that
   gets stripped before parsing. The parsed tree is nested under a synthetic
   <xml_string> element inside <XmlconfigRoot game="...">, and the whole tree
   is re-indented -- so the original formatting of the embedded config is
   discarded. Output verified byte-for-byte against a known-good reference
   extraction for all 32 entries in test/Config.ovl. Appends to `out` like
   resolve_worlddesc;
   returns the start offset within `out`, or (uint64_t)-1 on failure. */
static uint64_t resolve_xmlconfig(growbuf_t *out, const unsigned char *decomp, size_t decomp_size,
                                   const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                   const ovl_fragment_t *frags_sorted, int frag_count,
                                   int32_t pool_index, uint32_t data_offset, size_t *out_size) {
    const ovl_fragment_t *link = find_link(frags_sorted, frag_count, pool_index, data_offset);
    if (!link) return (uint64_t)-1;
    size_t raw_len;
    const char *raw = read_zstring_at(decomp, decomp_size, pools, pool_count, pool_region_start,
                                       link->struct_pool, link->struct_offset, &raw_len);
    if (!raw) return (uint64_t)-1;

    const char *text = raw;
    size_t text_len = raw_len;
    if (text_len >= 5 && strncmp(text, "<?xml", 5) == 0) {
        const char *end = NULL;
        for (size_t i = 0; i + 1 < text_len; i++) {
            if (text[i] == '?' && text[i + 1] == '>') { end = text + i + 2; break; }
        }
        if (end) {
            while (end < text + text_len && (*end == '\r' || *end == '\n')) end++;
            text_len -= (size_t)(end - text);
            text = end;
        }
    }
    /* xml_parse_element needs a NUL-terminated, mutable-scan buffer */
    char *buf = (char *)malloc(text_len + 1);
    if (!buf) return (uint64_t)-1;
    memcpy(buf, text, text_len);
    buf[text_len] = '\0';
    const char *cursor = buf;
    xml_node_t *inner = xml_parse_element(&cursor);
    if (!inner) { free(buf); return (uint64_t)-1; }

    xml_node_t *root = xml_node_new();
    xml_node_t *xml_string_elem = xml_node_new();
    if (!root || !xml_string_elem) { xml_node_free(root); xml_node_free(xml_string_elem); xml_node_free(inner); free(buf); return (uint64_t)-1; }
    root->tag = _strdup("XmlconfigRoot");
    xml_add_attr(root, _strdup("game"), _strdup(GAME_NAME));
    xml_string_elem->tag = _strdup("xml_string");
    xml_add_child(xml_string_elem, inner);
    xml_add_child(root, xml_string_elem);

    uint64_t start_off = out->size;
    xml_print_node(out, root, 0);
    xml_append_str(out, "\n"); /* root's own trailing tail, per the tab-indent recipe */
    *out_size = (size_t)(out->size - start_off);

    xml_node_free(root);
    free(buf);
    return start_off;
}

/* ---------------------------------------------------------------------------
 * Generic best-effort dump for unknown structured-data resource types
 *
 * For resource types with no known field schema (e.g. presets/games other
 * than the JWE2 "Casino:*" set this plugin has dedicated resolvers for),
 * there is no way to reproduce an official/exact XML shape. Instead of a
 * meaningless raw-byte stub, the pointer graph is resolved generically via
 * the same Fragment table used by the dedicated resolvers above, and
 * rendered as readable, self-invented pseudo-XML -- no claim of matching any
 * real engine schema, just best-effort readability (see unpack_ovl.py's
 * resolve_generic_structured for the equivalent Python implementation).
 * ------------------------------------------------------------------------- */

typedef struct { int32_t pool; uint32_t offset; } pool_boundary_t;

static int cmp_pool_boundary(const void *a, const void *b) {
    const pool_boundary_t *ba = (const pool_boundary_t *)a;
    const pool_boundary_t *bb = (const pool_boundary_t *)b;
    if (ba->pool != bb->pool) return (ba->pool > bb->pool) - (ba->pool < bb->pool);
    return (ba->offset > bb->offset) - (ba->offset < bb->offset);
}

/* Sorted list of every known offset (from RootEntry.data_offset AND
   Fragment.struct_offset) per pool -- lets struct_size_at_generic determine
   the size of ANY struct location reached via a pointer chain, not just
   root-level entries (generalizes the same boundary-diff idea used
   throughout this file). */
static pool_boundary_t *build_pool_boundaries(const ovl_root_entry_t *root_entries, int root_count,
                                               const ovl_fragment_t *fragments, int frag_count,
                                               int *out_count) {
    int cap = root_count + frag_count;
    if (cap <= 0) { *out_count = 0; return NULL; }
    pool_boundary_t *arr = (pool_boundary_t *)malloc(sizeof(pool_boundary_t) * (size_t)cap);
    if (!arr) { *out_count = 0; return NULL; }
    int n = 0;
    for (int i = 0; i < root_count; i++) { arr[n].pool = root_entries[i].pool_index; arr[n].offset = root_entries[i].data_offset; n++; }
    for (int i = 0; i < frag_count; i++) { arr[n].pool = fragments[i].struct_pool; arr[n].offset = fragments[i].struct_offset; n++; }
    qsort(arr, (size_t)n, sizeof(pool_boundary_t), cmp_pool_boundary);
    *out_count = n;
    return arr;
}

static uint32_t struct_size_at_generic(const pool_boundary_t *bounds, int bounds_count,
                                        const ovl_pool_t *pools, int pool_count,
                                        int32_t pool_index, uint32_t offset) {
    if (pool_index < 0 || pool_index >= pool_count) return 0;
    uint32_t pool_size = pools[pool_index].size;
    pool_boundary_t target = { pool_index, offset };
    int lo = 0, hi = bounds_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (cmp_pool_boundary(&bounds[mid], &target) <= 0) lo = mid + 1; else hi = mid;
    }
    uint32_t next_off = pool_size;
    if (lo < bounds_count && bounds[lo].pool == pool_index && bounds[lo].offset < next_off) next_off = bounds[lo].offset;
    return (next_off > offset) ? (next_off - offset) : 0;
}

#define GENERIC_MAX_DEPTH 4
#define GENERIC_MAX_ARRAY 256
#define GENERIC_MAX_VISITED 512

typedef enum { GV_RAW, GV_STR, GV_ARRAY, GV_STRUCT } generic_kind_t;
typedef struct generic_val {
    generic_kind_t kind;
    uint32_t raw;
    char *str;                     /* GV_STR: malloc'd */
    struct generic_val *children;  /* GV_ARRAY/GV_STRUCT: malloc'd array */
    int child_count;
} generic_val_t;

static void generic_val_free(generic_val_t *v) {
    if (!v) return;
    free(v->str);
    for (int i = 0; i < v->child_count; i++) generic_val_free(&v->children[i]);
    free(v->children);
}

static int visited_contains(const pool_boundary_t *v, int n, int32_t pool, uint32_t off) {
    for (int i = 0; i < n; i++) if (v[i].pool == pool && v[i].offset == off) return 1;
    return 0;
}

static int looks_like_text(const char *s, size_t len) {
    if (len == 0 || len > 512) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 32 && c < 127) || c == 9 || c == 10 || c == 13)) return 0;
    }
    return 1;
}

static char *generic_try_string(const unsigned char *decomp, size_t decomp_size,
                                 const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                 int32_t pool_index, uint32_t offset) {
    size_t len;
    const char *s = read_zstring_at(decomp, decomp_size, pools, pool_count, pool_region_start, pool_index, offset, &len);
    if (!s || !looks_like_text(s, len)) return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void generic_push_child(generic_val_t **arr, int *count, generic_val_t v) {
    generic_val_t *n = (generic_val_t *)realloc(*arr, sizeof(generic_val_t) * (size_t)(*count + 1));
    if (!n) { generic_val_free(&v); return; }
    *arr = n;
    (*arr)[*count] = v;
    (*count)++;
}

static generic_val_t generic_resolve_pointer(const unsigned char *decomp, size_t decomp_size,
                                              const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                              const ovl_fragment_t *frags_sorted, int frag_count,
                                              const pool_boundary_t *bounds, int bounds_count,
                                              int32_t pool_index, uint32_t offset,
                                              int depth, pool_boundary_t *visited, int visited_count);

/* Reads a struct slot-by-slot: 8-byte slots with a Fragment link resolve as a
   pointer (string / array / nested struct); everything else is read in
   4-byte steps as an unsigned integer. */
static void generic_resolve_struct(const unsigned char *decomp, size_t decomp_size,
                                    const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                    const ovl_fragment_t *frags_sorted, int frag_count,
                                    const pool_boundary_t *bounds, int bounds_count,
                                    int32_t pool_index, uint32_t offset, uint32_t size,
                                    int depth, pool_boundary_t *visited, int visited_count,
                                    generic_val_t **out_children, int *out_count) {
    *out_children = NULL;
    *out_count = 0;
    if (depth > GENERIC_MAX_DEPTH || size == 0 || pool_index < 0 || pool_index >= pool_count) return;
    if (visited_contains(visited, visited_count, pool_index, offset)) return;
    if (visited_count < GENERIC_MAX_VISITED) { visited[visited_count].pool = pool_index; visited[visited_count].offset = offset; visited_count++; }

    uint32_t pool_start = pool_region_start + pools[pool_index].offset;
    uint32_t end = offset + size;
    uint32_t pos = offset;
    while (pos < end) {
        const ovl_fragment_t *link = (pos + 8 <= end) ? find_link(frags_sorted, frag_count, pool_index, pos) : NULL;
        if (link) {
            int32_t tpool = link->struct_pool;
            uint32_t toff = link->struct_offset;
            char *s = generic_try_string(decomp, decomp_size, pools, pool_count, pool_region_start, tpool, toff);
            if (s) {
                generic_val_t v = {0}; v.kind = GV_STR; v.str = s;
                generic_push_child(out_children, out_count, v);
            } else {
                int run = 0;
                uint32_t p = toff;
                while (run < GENERIC_MAX_ARRAY && find_link(frags_sorted, frag_count, tpool, p)) { run++; p += 8; }
                if (run >= 2) {
                    generic_val_t v = {0}; v.kind = GV_ARRAY;
                    for (int i = 0; i < run; i++) {
                        const ovl_fragment_t *e = find_link(frags_sorted, frag_count, tpool, toff + (uint32_t)i * 8);
                        generic_val_t item = generic_resolve_pointer(decomp, decomp_size, pools, pool_count, pool_region_start,
                                                                      frags_sorted, frag_count, bounds, bounds_count,
                                                                      e->struct_pool, e->struct_offset, depth + 1, visited, visited_count);
                        generic_push_child(&v.children, &v.child_count, item);
                    }
                    generic_push_child(out_children, out_count, v);
                } else {
                    uint32_t tsize = struct_size_at_generic(bounds, bounds_count, pools, pool_count, tpool, toff);
                    generic_val_t v = {0}; v.kind = GV_STRUCT;
                    generic_resolve_struct(decomp, decomp_size, pools, pool_count, pool_region_start,
                                            frags_sorted, frag_count, bounds, bounds_count,
                                            tpool, toff, tsize, depth + 1, visited, visited_count,
                                            &v.children, &v.child_count);
                    generic_push_child(out_children, out_count, v);
                }
            }
            pos += 8;
        } else if (pos + 4 <= end) {
            uint32_t val; memcpy(&val, decomp + pool_start + pos, 4);
            generic_val_t v = {0}; v.kind = GV_RAW; v.raw = val;
            generic_push_child(out_children, out_count, v);
            pos += 4;
        } else {
            pos = end;
        }
    }
}

static generic_val_t generic_resolve_pointer(const unsigned char *decomp, size_t decomp_size,
                                              const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                              const ovl_fragment_t *frags_sorted, int frag_count,
                                              const pool_boundary_t *bounds, int bounds_count,
                                              int32_t pool_index, uint32_t offset,
                                              int depth, pool_boundary_t *visited, int visited_count) {
    char *s = generic_try_string(decomp, decomp_size, pools, pool_count, pool_region_start, pool_index, offset);
    if (s) { generic_val_t v = {0}; v.kind = GV_STR; v.str = s; return v; }
    uint32_t size = struct_size_at_generic(bounds, bounds_count, pools, pool_count, pool_index, offset);
    generic_val_t v = {0}; v.kind = GV_STRUCT;
    generic_resolve_struct(decomp, decomp_size, pools, pool_count, pool_region_start,
                            frags_sorted, frag_count, bounds, bounds_count,
                            pool_index, offset, size, depth, visited, visited_count,
                            &v.children, &v.child_count);
    return v;
}

static void generic_render_slots(growbuf_t *out, const generic_val_t *slots, int count, const char *indent);

static void generic_render_item(growbuf_t *out, const generic_val_t *item, const char *indent) {
    char child_indent[64];
    _snprintf(child_indent, sizeof(child_indent) - 1, "%s\t", indent);
    if (item->kind == GV_STR) {
        xml_append_str(out, indent); xml_append_str(out, "<item>");
        xml_append_escaped(out, item->str, 0);
        xml_append_str(out, "</item>\n");
        return;
    }
    /* GV_STRUCT with exactly one string field and the rest raw ints ->
       render compactly as attributes + text, matching the top-level
       renderer's philosophy of keeping simple records on one line. */
    int str_count = 0, other_count = 0, all_raw = 1;
    const generic_val_t *the_str = NULL;
    for (int i = 0; i < item->child_count; i++) {
        if (item->children[i].kind == GV_STR) { str_count++; the_str = &item->children[i]; }
        else { other_count++; if (item->children[i].kind != GV_RAW) all_raw = 0; }
    }
    if (item->kind == GV_STRUCT && str_count == 1 && all_raw) {
        xml_append_str(out, indent); xml_append_str(out, "<item");
        int attr_i = 0;
        for (int i = 0; i < item->child_count; i++) {
            if (item->children[i].kind != GV_RAW) continue;
            attr_i++;
            char buf[48];
            _snprintf(buf, sizeof(buf) - 1, " attr%d=\"%u\"", attr_i, item->children[i].raw);
            xml_append_str(out, buf);
        }
        xml_append_str(out, ">");
        xml_append_escaped(out, the_str->str, 0);
        xml_append_str(out, "</item>\n");
        return;
    }
    xml_append_str(out, indent); xml_append_str(out, "<item>\n");
    generic_render_slots(out, item->children, item->child_count, child_indent);
    xml_append_str(out, indent); xml_append_str(out, "</item>\n");
}

static void generic_render_slots(growbuf_t *out, const generic_val_t *slots, int count, const char *indent) {
    char child_indent[64];
    _snprintf(child_indent, sizeof(child_indent) - 1, "%s\t", indent);
    for (int i = 0; i < count; i++) {
        const generic_val_t *v = &slots[i];
        char tag[24];
        _snprintf(tag, sizeof(tag) - 1, "field%d", i + 1);
        switch (v->kind) {
            case GV_RAW: {
                char buf[64];
                _snprintf(buf, sizeof(buf) - 1, "%s<%s>%u</%s>\n", indent, tag, v->raw, tag);
                xml_append_str(out, buf);
                break;
            }
            case GV_STR:
                xml_append_str(out, indent); xml_append_str(out, "<"); xml_append_str(out, tag); xml_append_str(out, ">");
                xml_append_escaped(out, v->str, 0);
                xml_append_str(out, "</"); xml_append_str(out, tag); xml_append_str(out, ">\n");
                break;
            case GV_ARRAY: {
                char buf[48];
                _snprintf(buf, sizeof(buf) - 1, "%s<items count=\"%d\">\n", indent, v->child_count);
                xml_append_str(out, buf);
                for (int j = 0; j < v->child_count; j++) generic_render_item(out, &v->children[j], child_indent);
                xml_append_str(out, indent); xml_append_str(out, "</items>\n");
                break;
            }
            case GV_STRUCT:
                xml_append_str(out, indent); xml_append_str(out, "<"); xml_append_str(out, tag); xml_append_str(out, ">\n");
                generic_render_slots(out, v->children, v->child_count, child_indent);
                xml_append_str(out, indent); xml_append_str(out, "</"); xml_append_str(out, tag); xml_append_str(out, ">\n");
                break;
        }
    }
}

/* Best-effort dump for an unknown structured-data type (see module comment
   above). type_name is the middle segment of the type string (e.g.
   "ControlGroup" from "Project:ControlGroup:controls"). Appends to `out`
   like resolve_worlddesc/resolve_xmlconfig; returns the start offset within
   `out`, or (uint64_t)-1 on failure. */
static uint64_t resolve_generic_structured(growbuf_t *out, const unsigned char *decomp, size_t decomp_size,
                                            const ovl_pool_t *pools, int pool_count, uint32_t pool_region_start,
                                            const ovl_fragment_t *frags_sorted, int frag_count,
                                            const pool_boundary_t *bounds, int bounds_count,
                                            int32_t pool_index, uint32_t data_offset, uint32_t size,
                                            const char *type_name, const char *file_name, size_t *out_size) {
    pool_boundary_t visited[GENERIC_MAX_VISITED];
    generic_val_t *slots = NULL; int slot_count = 0;
    generic_resolve_struct(decomp, decomp_size, pools, pool_count, pool_region_start,
                            frags_sorted, frag_count, bounds, bounds_count,
                            pool_index, data_offset, size, 0, visited, 0, &slots, &slot_count);

    uint64_t start_off = out->size;
    xml_append_str(out, "<"); xml_append_str(out, type_name); xml_append_str(out, " name=\"");
    xml_append_escaped(out, file_name, 1);
    xml_append_str(out, "\">\n");
    generic_render_slots(out, slots, slot_count, "\t");
    xml_append_str(out, "</"); xml_append_str(out, type_name); xml_append_str(out, ">\n");
    *out_size = (size_t)(out->size - start_off);

    for (int i = 0; i < slot_count; i++) generic_val_free(&slots[i]);
    free(slots);
    return start_off;
}

/* "name.ext" of a file-table entry: sanitized name plus the short extension
   (the part after the last ':' of the type string). */
static void file_entry_name(const ovl_file_t *finfo, char *out, size_t out_size) {
    char name_buf[280];
    char ext_buf[100];
    strncpy(name_buf, finfo->name, sizeof(name_buf) - 1); name_buf[sizeof(name_buf) - 1] = '\0';
    ovl_sanitize(name_buf);
    /* finfo->ext is a full type string "Namespace:Class:extension" (e.g.
       "Casino:WorldDesc:world") -- only the part after the last ':' is
       the actual file extension (e.g. just ".world"). */
    const char *ext_src = finfo->ext;
    const char *last_colon = strrchr(ext_src, ':');
    if (last_colon) ext_src = last_colon + 1;
    strncpy(ext_buf, ext_src, sizeof(ext_buf) - 1); ext_buf[sizeof(ext_buf) - 1] = '\0';
    if (ext_buf[0] != '\0' && ext_buf[0] != '.') {
        char tmp[100];
        ovl_sanitize(ext_buf);
        _snprintf(tmp, sizeof(tmp) - 1, ".%s", ext_buf);
        tmp[sizeof(tmp) - 1] = '\0';
        strncpy(ext_buf, tmp, sizeof(ext_buf) - 1); ext_buf[sizeof(ext_buf) - 1] = '\0';
    } else {
        ovl_sanitize(ext_buf);
    }
    _snprintf(out, out_size - 1, "%s%s", name_buf, ext_buf);
    out[out_size - 1] = '\0';
}

/* Builds the final entry name: known name+ext from the file table when
   has_hash and a match is found, otherwise signature-detected extension with
   a running fallback index (mirrors unpack_ovl.py's _name_for_hash).
   ext_hash (0 = unknown) picks the right one among same-named files of
   different types, see ovl_find_file_by_hash_ext. */
static void name_for_hash(const ovl_header_t *header, int has_hash, uint32_t file_hash,
                           uint32_t ext_hash,
                           const unsigned char *data, size_t data_size,
                           const char *fallback_prefix, int *fallback_idx,
                           char *out, size_t out_size) {
    const ovl_file_t *finfo = has_hash ? ovl_find_file_by_hash_ext(header, file_hash, ext_hash) : NULL;
    if (finfo) {
        file_entry_name(finfo, out, out_size);
        return;
    }
    const char *detected = ovl_detect_ext(data, data_size);
    const char *ext = detected ? detected : ".bin";
    _snprintf(out, out_size - 1, "%s-%04d%s", fallback_prefix, (*fallback_idx)++, ext);
    out[out_size - 1] = '\0';
}

/* Lists one file's buffers as a single entry "name.ext": the n buffers in
   bufs[] (all owned by the same DataEntry, already ordered by slot)
   concatenated. Contiguous buffers -- always the case for v19 -- are
   referenced in place; otherwise (v20 BufferGroups) they are copied into the
   archive's synth buffer. Empty files are skipped; if the plain name is
   already taken (e.g. by a pool entry), ".buffers" is appended. */
static void push_owner_buffers(ovl_wcx_handle_t *ctx, int arc_idx, const ovl_file_t *finfo,
                               const int *bufs, int n,
                               const uint64_t *buf_start, const uint64_t *buf_len,
                               const unsigned char *decomp, growbuf_t *synth) {
    uint64_t total = 0;
    int contiguous = 1;
    for (int j = 0; j < n; j++) {
        total += buf_len[bufs[j]];
        if (j > 0 && buf_start[bufs[j]] != buf_start[bufs[j - 1]] + buf_len[bufs[j - 1]]) contiguous = 0;
    }
    if (total == 0) return;

    char full[400];
    file_entry_name(finfo, full, sizeof(full) - 16);
    if (entry_name_taken(ctx, full)) strcat(full, ".buffers");

    if (contiguous) {
        push_entry(ctx, full, total, arc_idx, buf_start[bufs[0]]);
        return;
    }
    uint64_t off = (uint64_t)-1;
    for (int j = 0; j < n; j++) {
        uint64_t o = growbuf_append(synth, decomp + buf_start[bufs[j]], (size_t)buf_len[bufs[j]]);
        if (o == (uint64_t)-1) return;
        if (j == 0) off = o;
    }
    push_synth_entry(ctx, full, total, arc_idx, off);
}

/* Processes the pool and buffer data of a decompressed archive and appends
   the corresponding entries. decomp/decomp_size belong to arc.

   Layout:
   [pool_groups][pools][data_entries][buffer_entries][buffer_groups]
   [root_entries][fragments][set_header] -> then the pools' raw data, then
   the buffers' raw data. When num_root_entries>0 a pool may bundle several
   named resources; their boundaries are resolved via sorted RootEntry/
   Fragment offsets. When num_datas>0, buffers are named via DataEntry/
   BufferGroup instead of being anonymous "<arc>_bufNNN.bin".

   STRUCTURED DATA: many resource types are not simple contiguous byte blobs
   -- their RootEntry points only to a small MemStruct header whose pointer
   fields are all zero placeholders on disk; the real data is elsewhere and
   only reachable by resolving each pointer via the Fragment table. Known
   types (Casino:AssetPackageRes:assetpkg, Casino:WorldDesc:world,
   Casino:XMLConfig:xmlconfig) get exact, byte-verified resolvers (see
   resolve_assetpkg/resolve_worlddesc/resolve_xmlconfig above). Any other
   type whose struct contains at least one resolvable pointer field falls
   back to resolve_generic_structured: a self-invented, best-effort pseudo-
   XML dump of the resolved pointer graph (no claim of matching a real
   engine schema) -- still far more useful than the meaningless raw stub.
   Regular buffer-backed assets (textures, models, audio, Lua modules) are
   unaffected either way. */
static void build_entries_for_archive(ovl_wcx_handle_t *ctx, int arc_idx,
                                       const unsigned char *decomp, size_t decomp_size) {
    const ovl_archive_t *arc = &ctx->header.archives[arc_idx];
    int version = ctx->header.version;

    uint32_t pool_region_start = ovl_compute_pool_region_start(decomp, decomp_size, arc, version);
    uint32_t pool_region_sz = (arc->pools_end >= arc->pools_start) ? (arc->pools_end - arc->pools_start) : 0;

    ovl_pool_t *pools = NULL;
    int pool_count = 0;
    ovl_parse_mempools(decomp, decomp_size, arc, version, &pools, &pool_count);

    /* DataEntries read upfront: files whose real content lives in a buffer
       must not also be emitted (with a wrong/incomplete reference stub)
       from their pool entry. */
    ovl_data_entry_t *data_entries = NULL;
    int data_count = 0;
    if (arc->num_datas > 0) {
        ovl_parse_data_entries(decomp, decomp_size, arc, version, &data_entries, &data_count);
    }

    int unknown_idx = 0;
    growbuf_t synth = {0};  /* reconstructed content, handed to ctx->synth_bufs at the end */

    if (arc->num_root_entries > 0 && pools) {
        ovl_root_entry_t *root_entries = NULL; int root_count = 0;
        ovl_fragment_t *fragments = NULL; int frag_count = 0;
        ovl_parse_root_entries(decomp, decomp_size, arc, version, &root_entries, &root_count);
        ovl_parse_fragments(decomp, decomp_size, arc, version, &fragments, &frag_count);

        ovl_fragment_t *frags_sorted = NULL;
        if (frag_count > 0) {
            frags_sorted = (ovl_fragment_t *)malloc((size_t)frag_count * sizeof(ovl_fragment_t));
            if (frags_sorted) {
                memcpy(frags_sorted, fragments, (size_t)frag_count * sizeof(ovl_fragment_t));
                qsort(frags_sorted, (size_t)frag_count, sizeof(ovl_fragment_t), cmp_fragment_by_link);
            }
        }

        int bounds_count = 0;
        pool_boundary_t *bounds = build_pool_boundaries(root_entries, root_count, fragments, frag_count, &bounds_count);

        ovl_sub_file_t *subs = NULL; int sub_count = 0;
        if (ovl_resolve_pool_sub_files(pools, pool_count, root_entries, root_count,
                                        fragments, frag_count, &subs, &sub_count)) {
            for (int i = 0; i < sub_count; i++) {
                /* Same-named files of different types share the file_hash
                   (e.g. "x.ms2" with a DataEntry next to a structured "x.xyz"
                   without one) -- compare ext_hash too where both have it. */
                int in_data_entries = 0;
                for (int d = 0; d < data_count; d++) {
                    if (data_entries[d].file_hash == subs[i].file_hash &&
                        (data_entries[d].ext_hash == 0 || subs[i].ext_hash == 0 ||
                         data_entries[d].ext_hash == subs[i].ext_hash)) {
                        in_data_entries = 1;
                        break;
                    }
                }
                if (in_data_entries) continue; /* real content comes from a buffer, see below */

                const ovl_file_t *sub_finfo = ovl_find_file_by_hash_ext(&ctx->header, subs[i].file_hash,
                                                                        subs[i].ext_hash);

                if (sub_finfo && strcmp(sub_finfo->ext, "Casino:AssetPackageRes:assetpkg") == 0 && frags_sorted) {
                    /* pools[subs[i].pool_index] + subs[i].offset is exactly this
                       occurrence's RootEntry (pool_index, data_offset) -- using it
                       directly (instead of a file_hash->RootEntry map) avoids picking
                       the wrong RootEntry when the same file_hash appears multiple
                       times with different ext (distinct resource facades of the same
                       object, e.g. an asset that is both ...assetpkg and ...lua). */
                    size_t out_size = 0;
                    unsigned char *resolved = resolve_assetpkg(decomp, decomp_size, pools, pool_count,
                                                                pool_region_start, frags_sorted, frag_count,
                                                                subs[i].pool_index, (uint32_t)subs[i].offset, &out_size);
                    if (resolved) {
                        uint64_t off = growbuf_append(&synth, resolved, out_size);
                        free(resolved);
                        if (off != (uint64_t)-1) {
                            char full[400];
                            name_for_hash(&ctx->header, 1, subs[i].file_hash, subs[i].ext_hash,
                                          synth.data + off, out_size, "unknown", &unknown_idx, full, sizeof(full));
                            push_synth_entry(ctx, full, out_size, arc_idx, off);
                            continue;
                        }
                    }
                    /* Resolution failed (no matching Fragment) -- skip rather than
                       write the meaningless raw stub. */
                    continue;
                }
                if (sub_finfo && strcmp(sub_finfo->ext, "Casino:WorldDesc:world") == 0 && frags_sorted) {
                    size_t out_size = 0;
                    uint64_t off = resolve_worlddesc(&synth, decomp, decomp_size, pools, pool_count,
                                                      pool_region_start, frags_sorted, frag_count,
                                                      subs[i].pool_index, (uint32_t)subs[i].offset, &out_size);
                    if (off != (uint64_t)-1) {
                        char full[400];
                        name_for_hash(&ctx->header, 1, subs[i].file_hash, subs[i].ext_hash,
                                      synth.data + off, out_size, "unknown", &unknown_idx, full, sizeof(full));
                        push_synth_entry(ctx, full, out_size, arc_idx, off);
                    }
                    continue; /* on failure: skip rather than write the wrong raw stub */
                }
                if (sub_finfo && strcmp(sub_finfo->ext, "Casino:XMLConfig:xmlconfig") == 0 && frags_sorted) {
                    size_t out_size = 0;
                    uint64_t off = resolve_xmlconfig(&synth, decomp, decomp_size, pools, pool_count,
                                                      pool_region_start, frags_sorted, frag_count,
                                                      subs[i].pool_index, (uint32_t)subs[i].offset, &out_size);
                    if (off != (uint64_t)-1) {
                        char full[400];
                        name_for_hash(&ctx->header, 1, subs[i].file_hash, subs[i].ext_hash,
                                      synth.data + off, out_size, "unknown", &unknown_idx, full, sizeof(full));
                        push_synth_entry(ctx, full, out_size, arc_idx, off);
                    }
                    continue; /* on failure: skip rather than write the wrong raw stub */
                }
                if (sub_finfo && is_structured_data_ext(sub_finfo->ext)) continue;

                /* Unknown type (no dedicated resolver above) -- check whether
                   any 8-byte slot in this range has a Fragment link. If so,
                   it's another MemStruct-style object with pointer fields
                   (like assetpkg/world/xmlconfig), just without a known
                   schema -- generic best-effort dump instead of the
                   meaningless raw stub (see resolve_generic_structured). */
                int has_pointer = 0;
                if (frags_sorted) {
                    for (uint64_t o = 0; o + 8 <= subs[i].size; o += 8) {
                        if (find_link(frags_sorted, frag_count, subs[i].pool_index, (uint32_t)(subs[i].offset + o))) {
                            has_pointer = 1;
                            break;
                        }
                    }
                }
                if (has_pointer) {
                    const char *type_name = "Unknown";
                    if (sub_finfo) {
                        const char *c1 = strchr(sub_finfo->ext, ':');
                        if (c1) type_name = c1 + 1; /* still "Class:ext" here, trimmed below */
                    }
                    char type_buf[100];
                    if (type_name != NULL) {
                        strncpy(type_buf, type_name, sizeof(type_buf) - 1); type_buf[sizeof(type_buf) - 1] = '\0';
                        char *c2 = strchr(type_buf, ':');
                        if (c2) *c2 = '\0';
                    } else {
                        strcpy(type_buf, "Unknown");
                    }
                    const char *file_name = sub_finfo ? sub_finfo->name : "unknown";
                    size_t out_size = 0;
                    uint64_t off = resolve_generic_structured(&synth, decomp, decomp_size, pools, pool_count,
                                                               pool_region_start, frags_sorted, frag_count,
                                                               bounds, bounds_count, subs[i].pool_index,
                                                               (uint32_t)subs[i].offset, (uint32_t)subs[i].size,
                                                               type_buf, file_name, &out_size);
                    char full[400];
                    name_for_hash(&ctx->header, 1, subs[i].file_hash, subs[i].ext_hash,
                                  synth.data + off, out_size, "unknown", &unknown_idx, full, sizeof(full));
                    push_synth_entry(ctx, full, out_size, arc_idx, off);
                    continue;
                }

                int pidx = subs[i].pool_index;
                uint64_t raw_start = (uint64_t)pool_region_start + pools[pidx].offset + subs[i].offset;
                uint64_t start, sz;
                py_slice_bounds(decomp_size, raw_start, subs[i].size, &start, &sz);
                if (sz == 0) continue;

                char full[400];
                name_for_hash(&ctx->header, 1, subs[i].file_hash, subs[i].ext_hash,
                              decomp + start, (size_t)sz, "unknown", &unknown_idx, full, sizeof(full));
                push_entry(ctx, full, sz, arc_idx, start);
            }
        }
        free(bounds);
        free(frags_sorted);
        free(subs);
        free(root_entries);
        free(fragments);
    } else {
        /* Simple case: one pool = one file (unchanged from before). */
        for (int i = 0; i < pool_count; i++) {
            uint64_t raw_start = (uint64_t)pool_region_start + pools[i].offset;
            uint64_t start, sz;
            py_slice_bounds(decomp_size, raw_start, pools[i].size, &start, &sz);
            if (sz == 0) continue;
            char full[400];
            name_for_hash(&ctx->header, 1, pools[i].file_hash, 0, decomp + start, (size_t)sz,
                          "unknown", &unknown_idx, full, sizeof(full));
            push_entry(ctx, full, sz, arc_idx, start);
        }
    }
    free(pools);

    /* Buffers (bulk data: texture mips, model vertices, Lua modules, ...) */
    if (arc->num_buffers > 0) {
        uint32_t *sizes = NULL;
        int sizes_count = 0;
        int parse_ok = ovl_parse_buffer_sizes(decomp, decomp_size, arc, version, &sizes, &sizes_count);
        DBG("  buf-parse: ok=%d count=%d pool_region_sz=%u pool_region_start=%u decomp_size=%zu\n",
            parse_ok, sizes_count, pool_region_sz, pool_region_start, decomp_size);
        if (parse_ok) {
            int *buffer_owner = NULL;
            int *buffer_sub = NULL;
            if (arc->num_datas > 0 && sizes_count > 0) {
                buffer_owner = (int *)calloc((size_t)sizes_count, sizeof(int));
                buffer_sub = (int *)calloc((size_t)sizes_count, sizeof(int));
                ovl_buffer_group_t *groups = NULL;
                int group_count = 0;
                ovl_parse_buffer_groups(decomp, decomp_size, arc, version, &groups, &group_count);
                if (buffer_owner && buffer_sub) {
                    ovl_resolve_buffer_owners(data_entries, data_count, groups, group_count,
                                              sizes_count, buffer_owner, buffer_sub);
                }
                free(groups);
            }

            /* Byte range of every buffer first (Python-style slice clamping). */
            uint64_t *buf_start = (uint64_t *)calloc((size_t)sizes_count, sizeof(uint64_t));
            uint64_t *buf_len = (uint64_t *)calloc((size_t)sizes_count, sizeof(uint64_t));
            uint64_t pos = (uint64_t)pool_region_start + pool_region_sz;
            for (int i = 0; buf_start && buf_len && i < sizes_count; i++) {
                py_slice_bounds(decomp_size, pos, sizes[i], &buf_start[i], &buf_len[i]);
                DBG("    buf[%d] raw_size=%u pos=%llu -> sz=%llu\n", i, sizes[i], (unsigned long long)pos,
                    (unsigned long long)buf_len[i]);
                pos += sizes[i]; /* unclamped advance, see comment above on slice semantics */
            }

            /* Buffers grouped by owning DataEntry, each group ordered by slot:
               a file owning buffers is listed once, as "name.ext" with all its
               buffers concatenated (see push_owner_buffers). */
            int *grp_first = NULL, *grp_bufs = NULL, *grp_fill = NULL;
            if (buffer_owner && buffer_sub && data_count > 0) {
                grp_first = (int *)calloc((size_t)data_count + 1, sizeof(int));
                grp_fill = (int *)calloc((size_t)data_count, sizeof(int));
                grp_bufs = (int *)calloc((size_t)sizes_count, sizeof(int));
            }
            if (grp_first && grp_fill && grp_bufs) {
                for (int i = 0; i < sizes_count; i++)
                    if (buffer_owner[i] >= 0) grp_first[buffer_owner[i] + 1]++;
                for (int k = 0; k < data_count; k++) grp_first[k + 1] += grp_first[k];
                for (int i = 0; i < sizes_count; i++) {
                    int k = buffer_owner[i];
                    if (k < 0) continue;
                    int *g = grp_bufs + grp_first[k];
                    int j = grp_fill[k]++;
                    while (j > 0 && buffer_sub[g[j - 1]] > buffer_sub[i]) { g[j] = g[j - 1]; j--; }
                    g[j] = i;
                }
            }

            int buf_unknown_idx = 0;
            for (int i = 0; buf_start && buf_len && i < sizes_count; i++) {
                int k = (grp_first && grp_fill && grp_bufs) ? buffer_owner[i] : -1;
                const ovl_file_t *finfo = NULL;
                if (k >= 0) {
                    finfo = ovl_find_file_by_hash_ext(&ctx->header, data_entries[k].file_hash,
                                                      data_entries[k].ext_hash);
                }
                if (finfo) {
                    /* listed once, at the owner's first buffer in table order */
                    if (grp_fill[k] >= 0) {
                        push_owner_buffers(ctx, arc_idx, finfo, grp_bufs + grp_first[k],
                                           grp_first[k + 1] - grp_first[k], buf_start, buf_len,
                                           decomp, &synth);
                        grp_fill[k] = -1;
                    }
                    continue;
                }
                if (buf_len[i] == 0) continue;
                char full[400];
                const char *detected = ovl_detect_ext(decomp + buf_start[i], (size_t)buf_len[i]);
                const char *ext = detected ? detected : ".bin";
                if (k >= 0) {
                    _snprintf(full, sizeof(full) - 1, "%s_unknown-%04d%s", arc->name, buf_unknown_idx++, ext);
                } else {
                    _snprintf(full, sizeof(full) - 1, "%s_buf%03d%s", arc->name, i, ext);
                }
                full[sizeof(full) - 1] = '\0';
                push_entry(ctx, full, buf_len[i], arc_idx, buf_start[i]);
            }
            free(grp_first);
            free(grp_fill);
            free(grp_bufs);
            free(buf_start);
            free(buf_len);
            free(buffer_owner);
            free(buffer_sub);
        }
        free(sizes);
    }
    free(data_entries);
    ctx->synth_bufs[arc_idx] = synth.data;
    ctx->synth_sizes[arc_idx] = synth.size;
}

/* Ensures the plugin's Oodle handle is loaded (searching upward from the
   .ovl's directory, then next to the plugin DLL itself as a fallback). */
static int ensure_oodle_loaded(ovl_wcx_handle_t *ctx, const wchar_t *dir_w) {
    if (!ctx->oodle_tried) {
        ctx->oodle_tried = 1;
        /* oo2core_*.dll usually sits in the game's install root, while .ovl
           files can be nested very deep (e.g. 9 levels below the root in
           JWE2) -- so search generously in parent directories too. */
        ctx->oodle_ok = ovl_oodle_load_upward(dir_w, 16, &ctx->oodle);

        /* Fallback: next to the plugin DLL itself. Lets a copy of
           oo2core_*.dll dropped into the plugin folder work for any .ovl,
           even outside a full game installation (e.g. a standalone test
           file). */
        if (!ctx->oodle_ok) {
            wchar_t plugin_dir[MAX_PATH];
            if (get_plugin_dir(plugin_dir, MAX_PATH)) {
                ctx->oodle_ok = ovl_oodle_load(plugin_dir, &ctx->oodle);
                if (ctx->oodle_ok) DBG("  -> Oodle DLL found next to the plugin: %ls\n", plugin_dir);
            }
        }
    }
    return ctx->oodle_ok;
}

/* Decompresses a compressed blob (STATIC data, or a compressed OVS batch
   file) according to the archive's compression flag. Returns a malloc'd
   buffer (caller frees) on success, NULL on failure. */
static unsigned char *decompress_blob(ovl_wcx_handle_t *ctx, const wchar_t *dir_w,
                                       const unsigned char *compressed, uint32_t cs,
                                       uint64_t uncompressed_size, size_t *out_size) {
    *out_size = 0;
    if (ctx->header.compression == OVL_COMPRESSION_OODLE) {
        if (!ensure_oodle_loaded(ctx, dir_w)) {
            DBG("  -> Oodle DLL not found (also not in parent directories of %ls, nor next to the plugin)\n", dir_w);
            return NULL;
        }
        if (uncompressed_size == 0 || uncompressed_size > 0xFFFFFFFFull) { DBG("  -> invalid uncompressed_size\n"); return NULL; }
        unsigned char *decomp = (unsigned char *)malloc((size_t)uncompressed_size);
        if (!decomp) return NULL;
        if (!ovl_oodle_decompress(&ctx->oodle, compressed, cs, decomp, (size_t)uncompressed_size)) {
            DBG("  -> Oodle decompression failed\n");
            free(decomp);
            return NULL;
        }
        *out_size = (size_t)uncompressed_size;
        return decomp;
    } else if (ctx->header.compression == OVL_COMPRESSION_ZLIB) {
        if (cs < 2) return NULL;
        if (uncompressed_size == 0 || uncompressed_size > 0xFFFFFFFFull) { DBG("  -> invalid uncompressed_size\n"); return NULL; }
        unsigned char *decomp = (unsigned char *)malloc((size_t)uncompressed_size);
        if (!decomp) return NULL;
        if (!ovl_zlib_inflate_raw(compressed + 2, cs - 2, decomp, (size_t)uncompressed_size)) {
            DBG("  -> ZLIB decompression failed (cs=%u out=%llu)\n", cs, (unsigned long long)uncompressed_size);
            free(decomp);
            return NULL;
        }
        *out_size = (size_t)uncompressed_size;
        return decomp;
    }
    return NULL; /* NONE/unknown: caller should use the raw bytes directly */
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

        if (ctx->header.compression == OVL_COMPRESSION_NONE) {
            decomp = (unsigned char *)compressed;
            decomp_size = cs;
            owns = 0;
        } else {
            decomp = decompress_blob(ctx, dir_w, compressed, cs, arc->uncompressed_size, &decomp_size);
            if (!decomp) return;
            owns = 1;
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
        unsigned char *raw_ovs = NULL;
        size_t raw_ovs_size = 0;
        if (!read_whole_file_w(chosen, &raw_ovs, &raw_ovs_size)) { DBG("  -> failed to read OVS\n"); return; }

        /* OVS batch files are usually stored uncompressed, but some are
           still compressed (raw size matches compressed_size instead of
           uncompressed_size) -- detect and decompress accordingly. */
        if (raw_ovs_size == arc->compressed_size && arc->uncompressed_size != 0 &&
            arc->uncompressed_size != raw_ovs_size && ctx->header.compression != OVL_COMPRESSION_NONE) {
            decomp = decompress_blob(ctx, dir_w, raw_ovs, (uint32_t)raw_ovs_size, arc->uncompressed_size, &decomp_size);
            free(raw_ovs);
            if (!decomp) { DBG("  -> OVS was compressed but decompression failed\n"); return; }
            DBG("  -> OVS was compressed, decompressed to %zu bytes\n", decomp_size);
            owns = 1;
        } else {
            decomp = raw_ovs;
            decomp_size = raw_ovs_size;
            owns = 1;
        }
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

/* Lists one text entry "Ref - <name>.ovl.txt" per included OVL (see
   ovl_included_t), before the archive contents -- otherwise an OVL that only
   includes others looks empty. The text (UTF-8) names the target, its
   resolved path next to the .ovl and whether it exists; it lives in
   ctx->info_buf. */
static void push_include_refs(ovl_wcx_handle_t *ctx, const wchar_t *dir_w) {
    growbuf_t gb = {0};
    const wchar_t *arc_file = wcsrchr(ctx->arc_path, L'\\');
    arc_file = arc_file ? arc_file + 1 : ctx->arc_path;
    char arc_file_u8[1024];
    if (!WideCharToMultiByte(CP_UTF8, 0, arc_file, -1, arc_file_u8, sizeof(arc_file_u8), NULL, NULL))
        strcpy(arc_file_u8, "?");

    for (int i = 0; i < ctx->header.num_included; i++) {
        const char *inc = ctx->header.included[i].name;
        if (!inc[0]) continue;

        wchar_t target[1400];
        _snwprintf(target, 1399, L"%s%hs.ovl", dir_w, inc);
        target[1399] = L'\0';
        for (wchar_t *c = target; *c; c++) if (*c == L'/') *c = L'\\';
        wchar_t target_long[1500];
        to_long_path(target, target_long, 1500);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        int exists = GetFileAttributesExW(target_long, GetFileExInfoStandard, &fad) &&
                     !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

        char target_u8[4096];
        if (!WideCharToMultiByte(CP_UTF8, 0, target, -1, target_u8, sizeof(target_u8), NULL, NULL))
            strcpy(target_u8, "?");
        char status[64];
        if (exists) {
            _snprintf(status, sizeof(status) - 1, "vorhanden, %llu Byte",
                      ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
        } else {
            strcpy(status, "nicht gefunden");
        }
        status[sizeof(status) - 1] = '\0';

        char text[6000];
        int n = _snprintf(text, sizeof(text) - 1,
                          "\xEF\xBB\xBF"
                          "Referenz (Include) aus %s\r\n"
                          "Ziel:    %s.ovl\r\n"
                          "Pfad:    %s\r\n"
                          "Status:  %s\r\n"
                          "\r\n"
                          "Das Spiel l\xC3\xA4" "dt diese OVL zusammen mit %s.\r\n",
                          arc_file_u8, inc, target_u8, status, arc_file_u8);
        if (n <= 0) continue;
        uint64_t off = growbuf_append(&gb, (const unsigned char *)text, (size_t)n);
        if (off == (uint64_t)-1) break;

        char flat[240];
        strncpy(flat, inc, sizeof(flat) - 1);
        flat[sizeof(flat) - 1] = '\0';
        for (char *c = flat; *c; c++) if (*c == '\\' || *c == '/') *c = '_';
        ovl_sanitize(flat);
        char entry_name[300];
        _snprintf(entry_name, sizeof(entry_name) - 1, "Ref - %s.ovl.txt", flat);
        entry_name[sizeof(entry_name) - 1] = '\0';
        for (int dup = 2; entry_name_taken(ctx, entry_name); dup++) {
            _snprintf(entry_name, sizeof(entry_name) - 1, "Ref - %s (%d).ovl.txt", flat, dup);
            entry_name[sizeof(entry_name) - 1] = '\0';
        }
        if (push_entry(ctx, entry_name, (uint64_t)n, 0, off)) {
            ctx->entries[ctx->entry_count - 1].is_synth = ENTRY_INFO;
        }
    }
    ctx->info_buf = gb.data;
    ctx->info_size = gb.size;
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
    ctx->synth_bufs = (unsigned char **)calloc(header.num_archives ? header.num_archives : 1, sizeof(unsigned char *));
    ctx->synth_sizes = (size_t *)calloc(header.num_archives ? header.num_archives : 1, sizeof(size_t));
    if (!ctx->decomp_bufs || !ctx->decomp_sizes || !ctx->owns_buf || !ctx->synth_bufs || !ctx->synth_sizes) {
        free(ctx->decomp_bufs); free(ctx->decomp_sizes); free(ctx->owns_buf);
        free(ctx->synth_bufs); free(ctx->synth_sizes);
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

    push_include_refs(ctx, dir_w);

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

    unsigned char *buf;
    size_t buf_size;
    if (e->is_synth == ENTRY_INFO) {
        buf = ctx->info_buf;
        buf_size = ctx->info_size;
    } else {
        buf = e->is_synth ? ctx->synth_bufs[e->archive_index] : ctx->decomp_bufs[e->archive_index];
        buf_size = e->is_synth ? ctx->synth_sizes[e->archive_index] : ctx->decomp_sizes[e->archive_index];
    }
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
    if (ctx->synth_bufs) {
        for (int i = 0; i < ctx->header.num_archives; i++) {
            free(ctx->synth_bufs[i]);
        }
    }
    free(ctx->decomp_bufs);
    free(ctx->decomp_sizes);
    free(ctx->owns_buf);
    free(ctx->synth_bufs);
    free(ctx->synth_sizes);
    free(ctx->info_buf);
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
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
