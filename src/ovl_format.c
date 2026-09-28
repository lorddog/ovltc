/**
 * ovlTC — A native Total Commander packer plugin (WCX, 64-bit) for .ovl files.
 *
 * Copyright (c) 2026 LordDog
 * Open source — feel free to use, modify, and distribute.
 * Credits are appreciated but not required.
 *
 * Parser for the OVL/FRES format used by the Cobra Engine (JWE preset).
 */
#include "ovl_format.h"
#include "zlib/zlib.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Upper bound guarding against malicious/corrupt headers with huge counts
   (overflow protection). */
#define OVL_MAX_COUNT 2000000u

#define ARCHIVE_ENTRY_SZ 68
#define FILE_SZ          12
#define DEPENDENCY_SZ    20
#define AUX_SZ           12
#define STREAM_SZ        12
#define ARC_META_SZ       8

/* ---- Bounds-checked little-endian readers ----------------------------------- */

static int rd_u16(const unsigned char *buf, size_t buf_size, size_t off, uint16_t *out) {
    if (off + 2 > buf_size) return 0;
    *out = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
    return 1;
}

static int rd_u32(const unsigned char *buf, size_t buf_size, size_t off, uint32_t *out) {
    if (off + 4 > buf_size) return 0;
    *out = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
    return 1;
}

static int rd_u64(const unsigned char *buf, size_t buf_size, size_t off, uint64_t *out) {
    uint32_t lo, hi;
    if (!rd_u32(buf, buf_size, off, &lo)) return 0;
    if (!rd_u32(buf, buf_size, off + 4, &hi)) return 0;
    *out = ((uint64_t)hi << 32) | lo;
    return 1;
}

/* Reads a null-terminated string from buf[off..buf_size), copies it
   (clipped) into dest (dest_size bytes, always null-terminated). */
static void rd_cstr(const unsigned char *buf, size_t buf_size, size_t off,
                     char *dest, size_t dest_size) {
    size_t i = 0;
    if (dest_size == 0) return;
    if (off >= buf_size) { dest[0] = '\0'; return; }
    while (i < dest_size - 1 && (off + i) < buf_size && buf[off + i] != 0) {
        dest[i] = (char)buf[off + i];
        i++;
    }
    dest[i] = '\0';
}

/* ---- Sanitize / signature detection ----------------------------------------- */

void ovl_sanitize(char *name) {
    static const char invalid[] = ":*?\"<>|";
    for (char *p = name; *p; p++) {
        if (strchr(invalid, (unsigned char)*p)) *p = '_';
    }
}

typedef struct { const unsigned char *magic; size_t magic_len; const char *ext; } ovl_sig_t;

static const unsigned char SIG_PNG[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
static const unsigned char SIG_LUA[] = { 0x1b, 'L', 'u', 'a' };
static const unsigned char SIG_JPG[] = { 0xff, 0xd8, 0xff };

static const ovl_sig_t OVL_SIGS[] = {
    { (const unsigned char *)"DDS ",  4, ".dds" },
    { SIG_PNG,                        8, ".png" },
    { SIG_LUA,                        4, ".luac" },
    { (const unsigned char *)"OggS",  4, ".ogg" },
    { (const unsigned char *)"fLaC",  4, ".flac" },
    { (const unsigned char *)"RIFF",  4, ".riff" },
    { (const unsigned char *)"<?xml", 5, ".xml" },
    { (const unsigned char *)"<",     1, ".xml" },
    { SIG_JPG,                        3, ".jpg" },
    { (const unsigned char *)"PK\x03\x04", 4, ".zip" },
    { (const unsigned char *)"BM",    2, ".bmp" },
};
#define OVL_SIGS_COUNT (sizeof(OVL_SIGS) / sizeof(OVL_SIGS[0]))

const char *ovl_detect_ext(const unsigned char *data, size_t size) {
    for (size_t i = 0; i < OVL_SIGS_COUNT; i++) {
        const ovl_sig_t *s = &OVL_SIGS[i];
        if (size >= s->magic_len && memcmp(data, s->magic, s->magic_len) == 0) {
            return s->ext;
        }
    }
    return NULL;
}

/* ---- Header parser ----------------------------------------------------------- */

static int fail(char *errbuf, size_t errbuf_size, const char *msg) {
    if (errbuf && errbuf_size) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return 0;
}

int ovl_parse_header(const unsigned char *data, size_t data_size,
                      ovl_header_t *out, char *errbuf, size_t errbuf_size) {
    memset(out, 0, sizeof(*out));

    if (data_size < 0x90 || memcmp(data, "FRES", 4) != 0) {
        return fail(errbuf, errbuf_size, "No FRES magic found");
    }

    int version = data[5];
    uint32_t user_version;
    if (!rd_u32(data, data_size, 8, &user_version))
        return fail(errbuf, errbuf_size, "Header too short (user_version)");
    int compression = (int)((user_version & 0x380) >> 7);

    uint16_t num_mimes; uint32_t num_files, num_archives, len_names, len_archive_names;
    uint32_t num_triplets; uint16_t num_included_ovls; uint32_t num_dependencies;
    uint32_t num_aux_entries, num_stream_files;

    if (!rd_u16(data, data_size, 0x1E, &num_mimes) ||
        !rd_u32(data, data_size, 0x20, &num_files) ||
        !rd_u32(data, data_size, 0x2C, &num_archives) ||
        !rd_u32(data, data_size, 0x10, &len_names) ||
        !rd_u32(data, data_size, 0x50, &len_archive_names) ||
        !rd_u32(data, data_size, 0x5C, &num_triplets) ||
        !rd_u16(data, data_size, 0x1C, &num_included_ovls) ||
        !rd_u32(data, data_size, 0x28, &num_dependencies) ||
        !rd_u32(data, data_size, 0x18, &num_aux_entries) ||
        !rd_u32(data, data_size, 0x40, &num_stream_files)) {
        return fail(errbuf, errbuf_size, "Header too short (counts)");
    }

    if (num_mimes > OVL_MAX_COUNT || num_files > OVL_MAX_COUNT ||
        num_archives > OVL_MAX_COUNT || num_triplets > OVL_MAX_COUNT) {
        return fail(errbuf, errbuf_size, "Implausibly large counts in header (possibly corrupt file)");
    }

    size_t names_start = 0x90;
    if (names_start + len_names > data_size)
        return fail(errbuf, errbuf_size, "Names block outside the file");
    const unsigned char *names_buf = data + names_start;
    size_t names_buf_size = len_names;

    size_t mime_sz = (version >= 20) ? 32 : 24;
    size_t mimes_start = names_start + len_names;

    out->mimes = (ovl_mime_t *)calloc(num_mimes ? num_mimes : 1, sizeof(ovl_mime_t));
    out->num_mimes = (int)num_mimes;
    if (!out->mimes) return fail(errbuf, errbuf_size, "Out of memory (mimes)");

    for (uint32_t i = 0; i < num_mimes; i++) {
        size_t o = mimes_start + (size_t)i * mime_sz;
        uint32_t name_off;
        if (!rd_u32(data, data_size, o, &name_off))
            return fail(errbuf, errbuf_size, "Mime entry outside the file");
        rd_cstr(names_buf, names_buf_size, name_off, out->mimes[i].name, sizeof(out->mimes[i].name));
    }

    size_t triplets_sz = 0;
    if (version >= 20) {
        triplets_sz = (size_t)num_triplets * 3;
        if (triplets_sz % 4) triplets_sz += 4 - (triplets_sz % 4);
    }

    size_t files_start = mimes_start + (size_t)num_mimes * mime_sz + triplets_sz;
    out->files = (ovl_file_t *)calloc(num_files ? num_files : 1, sizeof(ovl_file_t));
    out->num_files = (int)num_files;
    if (!out->files) return fail(errbuf, errbuf_size, "Out of memory (files)");

    for (uint32_t i = 0; i < num_files; i++) {
        size_t o = files_start + (size_t)i * FILE_SZ;
        uint32_t name_off, fhash; uint16_t ext_idx;
        if (!rd_u32(data, data_size, o, &name_off) ||
            !rd_u32(data, data_size, o + 4, &fhash) ||
            !rd_u16(data, data_size, o + 10, &ext_idx)) {
            return fail(errbuf, errbuf_size, "File entry outside the file");
        }
        rd_cstr(names_buf, names_buf_size, name_off, out->files[i].name, sizeof(out->files[i].name));
        out->files[i].file_hash = fhash;
        if (ext_idx < num_mimes) {
            strncpy(out->files[i].ext, out->mimes[ext_idx].name, sizeof(out->files[i].ext) - 1);
        }
    }

    size_t arc_names_start = files_start + (size_t)num_files * FILE_SZ;
    if (arc_names_start + len_archive_names > data_size)
        return fail(errbuf, errbuf_size, "ArchiveNames block outside the file");
    const unsigned char *arc_names_buf = data + arc_names_start;
    size_t arc_names_buf_size = len_archive_names;

    size_t arcs_start = arc_names_start + len_archive_names;
    out->archives = (ovl_archive_t *)calloc(num_archives ? num_archives : 1, sizeof(ovl_archive_t));
    out->num_archives = (int)num_archives;
    if (!out->archives) return fail(errbuf, errbuf_size, "Out of memory (archives)");

    for (uint32_t i = 0; i < num_archives; i++) {
        size_t o = arcs_start + (size_t)i * ARCHIVE_ENTRY_SZ;
        uint32_t name_off;
        ovl_archive_t *a = &out->archives[i];
        if (!rd_u32(data, data_size, o, &name_off) ||
            !rd_u32(data, data_size, o + 12, &a->num_pools) ||
            !rd_u16(data, data_size, o + 16, &a->num_datas) ||
            !rd_u16(data, data_size, o + 18, &a->num_pool_groups) ||
            !rd_u32(data, data_size, o + 20, &a->num_buffer_groups) ||
            !rd_u32(data, data_size, o + 24, &a->num_buffers) ||
            !rd_u32(data, data_size, o + 28, &a->num_fragments) ||
            !rd_u32(data, data_size, o + 32, &a->num_root_entries) ||
            !rd_u32(data, data_size, o + 36, &a->read_start) ||
            !rd_u32(data, data_size, o + 40, &a->set_data_size) ||
            !rd_u32(data, data_size, o + 44, &a->compressed_size) ||
            !rd_u64(data, data_size, o + 48, &a->uncompressed_size) ||
            !rd_u32(data, data_size, o + 56, &a->pools_start) ||
            !rd_u32(data, data_size, o + 60, &a->pools_end)) {
            return fail(errbuf, errbuf_size, "Archive entry outside the file");
        }
        rd_cstr(arc_names_buf, arc_names_buf_size, name_off, a->name, sizeof(a->name));
    }

    /* Included OVLs: one name offset (u32, into the names block) each, right
       after the archive entries. Lenient: stops at the end of the file. */
    size_t inc_start = arcs_start + (size_t)num_archives * ARCHIVE_ENTRY_SZ;
    out->included = (ovl_included_t *)calloc(num_included_ovls ? num_included_ovls : 1, sizeof(ovl_included_t));
    if (!out->included) return fail(errbuf, errbuf_size, "Out of memory (included OVLs)");
    for (uint32_t i = 0; i < num_included_ovls; i++) {
        uint32_t name_off;
        if (!rd_u32(data, data_size, inc_start + (size_t)i * 4, &name_off)) break;
        rd_cstr(names_buf, names_buf_size, name_off, out->included[i].name, sizeof(out->included[i].name));
        out->num_included = (int)i + 1;
    }

    size_t data_start = arcs_start
        + (size_t)num_archives * ARCHIVE_ENTRY_SZ
        + (size_t)num_included_ovls * 4
        + (size_t)num_dependencies * DEPENDENCY_SZ
        + (size_t)num_aux_entries * AUX_SZ
        + (size_t)num_stream_files * STREAM_SZ
        + (size_t)num_archives * ARC_META_SZ;

    out->version = version;
    out->user_version = user_version;
    out->compression = compression;
    out->data_start = (uint32_t)data_start;
    out->num_stream_files = num_stream_files;

    return 1;
}

void ovl_free_header(ovl_header_t *h) {
    if (!h) return;
    free(h->files);
    free(h->mimes);
    free(h->archives);
    free(h->included);
    memset(h, 0, sizeof(*h));
}

const ovl_file_t *ovl_find_file_by_hash(const ovl_header_t *h, uint32_t file_hash) {
    /* Iterate backwards: on duplicate hashes, the last entry in file order
       wins (matches a "last one wins" dict-style lookup semantics). */
    for (int i = h->num_files - 1; i >= 0; i--) {
        if (h->files[i].file_hash == file_hash) return &h->files[i];
    }
    return NULL;
}

uint32_t ovl_djb2(const char *s) {
    uint32_t hash = 5381;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        hash = (hash << 5) + hash + c;
    }
    return hash;
}

const ovl_file_t *ovl_find_file_by_hash_ext(const ovl_header_t *h, uint32_t file_hash,
                                            uint32_t ext_hash) {
    if (ext_hash != 0) {
        for (int i = h->num_files - 1; i >= 0; i--) {
            if (h->files[i].file_hash != file_hash) continue;
            const char *short_ext = strrchr(h->files[i].ext, ':');
            short_ext = short_ext ? short_ext + 1 : h->files[i].ext;
            if (ovl_djb2(short_ext) == ext_hash) return &h->files[i];
        }
    }
    return ovl_find_file_by_hash(h, file_hash);
}

/* ---- Mempool / buffer parsing ------------------------------------------------ */

static void struct_sizes(int version, size_t *pg_sz, size_t *mp_sz, size_t *de_sz, size_t *be_sz) {
    *pg_sz = 4;
    *mp_sz = (version >= 17) ? 32 : 16;
    *de_sz = (version >= 19) ? 32 : 24;
    *be_sz = 8;
}

int ovl_parse_mempools(const unsigned char *decomp, size_t decomp_size,
                        const ovl_archive_t *arc, int version,
                        ovl_pool_t **out_pools, int *out_count) {
    size_t pg_sz, mp_sz, de_sz, be_sz;
    struct_sizes(version, &pg_sz, &mp_sz, &de_sz, &be_sz);

    *out_pools = NULL;
    *out_count = 0;
    if (arc->num_pools == 0) return 1;
    if (arc->num_pools > OVL_MAX_COUNT) return 0;

    ovl_pool_t *pools = (ovl_pool_t *)calloc(arc->num_pools, sizeof(ovl_pool_t));
    if (!pools) return 0;

    size_t base = (size_t)arc->num_pool_groups * pg_sz;
    for (uint32_t i = 0; i < arc->num_pools; i++) {
        size_t o = base + (size_t)i * mp_sz;
        uint32_t size_, offset_, file_hash_;
        int ok;
        if (version >= 17) {
            ok = rd_u32(decomp, decomp_size, o + 8, &size_) &&
                 rd_u32(decomp, decomp_size, o + 12, &offset_) &&
                 rd_u32(decomp, decomp_size, o + 16, &file_hash_);
        } else {
            ok = rd_u32(decomp, decomp_size, o, &size_) &&
                 rd_u32(decomp, decomp_size, o + 4, &offset_) &&
                 rd_u32(decomp, decomp_size, o + 8, &file_hash_);
        }
        if (!ok) { free(pools); return 0; }
        pools[i].size = size_;
        pools[i].offset = offset_;
        pools[i].file_hash = file_hash_;
    }

    *out_pools = pools;
    *out_count = (int)arc->num_pools;
    return 1;
}

int ovl_parse_buffer_sizes(const unsigned char *decomp, size_t decomp_size,
                            const ovl_archive_t *arc, int version,
                            uint32_t **out_sizes, int *out_count) {
    size_t pg_sz, mp_sz, de_sz, be_sz;
    struct_sizes(version, &pg_sz, &mp_sz, &de_sz, &be_sz);

    *out_sizes = NULL;
    *out_count = 0;
    if (arc->num_buffers == 0) return 1;
    if (arc->num_buffers > OVL_MAX_COUNT) return 0;

    uint32_t *sizes = (uint32_t *)calloc(arc->num_buffers, sizeof(uint32_t));
    if (!sizes) return 0;

    size_t base = (size_t)arc->num_pool_groups * pg_sz
                + (size_t)arc->num_pools * mp_sz
                + (size_t)arc->num_datas * de_sz;

    for (uint32_t i = 0; i < arc->num_buffers; i++) {
        size_t o = base + (size_t)i * be_sz;
        uint32_t sz;
        int ok = (version <= 19) ? rd_u32(decomp, decomp_size, o + 4, &sz)
                                  : rd_u32(decomp, decomp_size, o, &sz);
        if (!ok) { free(sizes); return 0; }
        sizes[i] = sz;
    }

    *out_sizes = sizes;
    *out_count = (int)arc->num_buffers;
    return 1;
}

/* ---- Extended layout: DataEntry / BufferGroup / RootEntry / Fragment -------- *
 *
 * Struct order inside the decompressed archive:
 *   [pool_groups][pools][data_entries][buffer_entries][buffer_groups]
 *   [root_entries][fragments][set_header]
 *   -> only then the pools' own raw data, followed by the buffers' raw data.
 *
 * For simple archives (num_datas=0, num_root_entries=0, num_fragments=0) this
 * whole prefix is tiny and coincides with set_data_size -- the original
 * simple pool/buffer logic remains unchanged for that case. When these
 * tables are populated (e.g. Init.ovl bundling hundreds of files per pool),
 * the full prefix must be accounted for or every offset points nowhere. */

typedef struct {
    size_t data_entries, buffer_entries, buffer_groups, root_entries, fragments, set_header;
    size_t re_sz, fr_sz, de_sz, bg_sz;
} ovl_table_offsets_t;

static void table_offsets(const ovl_archive_t *arc, int version, ovl_table_offsets_t *o) {
    size_t pg_sz, mp_sz, de_sz, be_sz;
    struct_sizes(version, &pg_sz, &mp_sz, &de_sz, &be_sz);
    o->bg_sz = 32;
    o->re_sz = (version >= 19) ? 16 : 12;
    o->fr_sz = 16;
    o->de_sz = de_sz;

    o->data_entries  = (size_t)arc->num_pool_groups * pg_sz + (size_t)arc->num_pools * mp_sz;
    o->buffer_entries = o->data_entries + (size_t)arc->num_datas * de_sz;
    o->buffer_groups   = o->buffer_entries + (size_t)arc->num_buffers * be_sz;
    o->root_entries     = o->buffer_groups + (size_t)arc->num_buffer_groups * o->bg_sz;
    o->fragments          = o->root_entries + (size_t)arc->num_root_entries * o->re_sz;
    o->set_header            = o->fragments + (size_t)arc->num_fragments * o->fr_sz;
}

uint32_t ovl_compute_pool_region_start(const unsigned char *decomp, size_t decomp_size,
                                        const ovl_archive_t *arc, int version) {
    if (arc->num_datas == 0 && arc->num_root_entries == 0 && arc->num_fragments == 0) {
        return arc->set_data_size;
    }

    ovl_table_offsets_t off;
    table_offsets(arc, version, &off);

    uint32_t set_count, asset_count;
    if (!rd_u32(decomp, decomp_size, off.set_header, &set_count) ||
        !rd_u32(decomp, decomp_size, off.set_header + 4, &asset_count)) {
        return arc->set_data_size;
    }
    size_t set_entry_sz   = (version >= 19) ? 12 : 8;
    size_t asset_entry_sz = (version >= 19) ? 24 : 16;
    size_t set_header_size = 16 + (size_t)set_count * set_entry_sz + (size_t)asset_count * asset_entry_sz;
    size_t result = off.set_header + set_header_size;

    /* Sanity/safety net: a corrupt or unexpectedly-laid-out archive (e.g. a
       compressed OVS batch mistakenly treated as already decompressed) could
       otherwise produce a wild offset here. All callers bounds-check against
       decomp_size before dereferencing, but falling back cleanly avoids
       building an unusable (and confusing) pool_region_start in the first
       place. */
    if (result > decomp_size || result > 0xFFFFFFFFull) {
        return arc->set_data_size;
    }
    return (uint32_t)result;
}

int ovl_parse_root_entries(const unsigned char *decomp, size_t decomp_size,
                            const ovl_archive_t *arc, int version,
                            ovl_root_entry_t **out_entries, int *out_count) {
    *out_entries = NULL;
    *out_count = 0;
    if (arc->num_root_entries == 0) return 1;
    if (arc->num_root_entries > OVL_MAX_COUNT) return 0;

    ovl_table_offsets_t off;
    table_offsets(arc, version, &off);

    ovl_root_entry_t *entries = (ovl_root_entry_t *)calloc(arc->num_root_entries, sizeof(ovl_root_entry_t));
    if (!entries) return 0;

    for (uint32_t i = 0; i < arc->num_root_entries; i++) {
        size_t o = off.root_entries + (size_t)i * off.re_sz;
        uint32_t file_hash = 0, ext_hash = 0, data_offset = 0;
        int32_t pool_index = -1;
        int ok;
        if (version >= 19) {
            uint32_t raw_pool_index;
            ok = rd_u32(decomp, decomp_size, o, &file_hash) &&
                 rd_u32(decomp, decomp_size, o + 4, &ext_hash) &&
                 rd_u32(decomp, decomp_size, o + 8, &raw_pool_index) &&
                 rd_u32(decomp, decomp_size, o + 12, &data_offset);
            pool_index = (int32_t)raw_pool_index;
        } else {
            uint32_t raw_pool_index;
            ok = rd_u32(decomp, decomp_size, o, &file_hash) &&
                 rd_u32(decomp, decomp_size, o + 4, &raw_pool_index) &&
                 rd_u32(decomp, decomp_size, o + 8, &data_offset);
            pool_index = (int32_t)raw_pool_index;
        }
        if (!ok) { free(entries); return 0; }
        entries[i].file_hash = file_hash;
        entries[i].ext_hash = ext_hash;
        entries[i].pool_index = pool_index;
        entries[i].data_offset = data_offset;
    }

    *out_entries = entries;
    *out_count = (int)arc->num_root_entries;
    return 1;
}

int ovl_parse_fragments(const unsigned char *decomp, size_t decomp_size,
                         const ovl_archive_t *arc, int version,
                         ovl_fragment_t **out_fragments, int *out_count) {
    *out_fragments = NULL;
    *out_count = 0;
    if (arc->num_fragments == 0) return 1;
    if (arc->num_fragments > OVL_MAX_COUNT) return 0;

    ovl_table_offsets_t off;
    table_offsets(arc, version, &off);

    ovl_fragment_t *fragments = (ovl_fragment_t *)calloc(arc->num_fragments, sizeof(ovl_fragment_t));
    if (!fragments) return 0;

    for (uint32_t i = 0; i < arc->num_fragments; i++) {
        size_t o = off.fragments + (size_t)i * off.fr_sz;
        uint32_t link_pool, link_offset, struct_pool, struct_offset;
        if (!rd_u32(decomp, decomp_size, o, &link_pool) ||
            !rd_u32(decomp, decomp_size, o + 4, &link_offset) ||
            !rd_u32(decomp, decomp_size, o + 8, &struct_pool) ||
            !rd_u32(decomp, decomp_size, o + 12, &struct_offset)) {
            free(fragments);
            return 0;
        }
        fragments[i].link_pool = (int32_t)link_pool;
        fragments[i].link_offset = link_offset;
        fragments[i].struct_pool = (int32_t)struct_pool;
        fragments[i].struct_offset = struct_offset;
    }

    *out_fragments = fragments;
    *out_count = (int)arc->num_fragments;
    return 1;
}

int ovl_parse_data_entries(const unsigned char *decomp, size_t decomp_size,
                            const ovl_archive_t *arc, int version,
                            ovl_data_entry_t **out_entries, int *out_count) {
    *out_entries = NULL;
    *out_count = 0;
    if (arc->num_datas == 0) return 1;
    if (arc->num_datas > OVL_MAX_COUNT) return 0;

    ovl_table_offsets_t off;
    table_offsets(arc, version, &off);

    ovl_data_entry_t *entries = (ovl_data_entry_t *)calloc(arc->num_datas, sizeof(ovl_data_entry_t));
    if (!entries) return 0;

    for (uint32_t i = 0; i < arc->num_datas; i++) {
        size_t o = off.data_entries + (size_t)i * off.de_sz;
        uint32_t file_hash, ext_hash = 0;
        uint16_t buffer_count;
        uint64_t size_1, size_2;
        int ok;
        if (version >= 19) {
            uint16_t set_index;
            ok = rd_u32(decomp, decomp_size, o, &file_hash) &&
                 rd_u32(decomp, decomp_size, o + 4, &ext_hash) &&
                 rd_u16(decomp, decomp_size, o + 8, &set_index) &&
                 rd_u16(decomp, decomp_size, o + 10, &buffer_count) &&
                 rd_u64(decomp, decomp_size, o + 16, &size_1) &&
                 rd_u64(decomp, decomp_size, o + 24, &size_2);
        } else {
            uint16_t set_index;
            ok = rd_u32(decomp, decomp_size, o, &file_hash) &&
                 rd_u16(decomp, decomp_size, o + 4, &set_index) &&
                 rd_u16(decomp, decomp_size, o + 6, &buffer_count) &&
                 rd_u64(decomp, decomp_size, o + 8, &size_1) &&
                 rd_u64(decomp, decomp_size, o + 16, &size_2);
        }
        if (!ok) { free(entries); return 0; }
        entries[i].file_hash = file_hash;
        entries[i].ext_hash = ext_hash;
        entries[i].buffer_count = buffer_count;
        entries[i].size_1 = size_1;
        entries[i].size_2 = size_2;
    }

    *out_entries = entries;
    *out_count = (int)arc->num_datas;
    return 1;
}

int ovl_parse_buffer_groups(const unsigned char *decomp, size_t decomp_size,
                             const ovl_archive_t *arc, int version,
                             ovl_buffer_group_t **out_groups, int *out_count) {
    *out_groups = NULL;
    *out_count = 0;
    if (arc->num_buffer_groups == 0) return 1;
    if (arc->num_buffer_groups > OVL_MAX_COUNT) return 0;

    ovl_table_offsets_t off;
    table_offsets(arc, version, &off);

    ovl_buffer_group_t *groups = (ovl_buffer_group_t *)calloc(arc->num_buffer_groups, sizeof(ovl_buffer_group_t));
    if (!groups) return 0;

    for (uint32_t i = 0; i < arc->num_buffer_groups; i++) {
        size_t o = off.buffer_groups + (size_t)i * off.bg_sz;
        uint32_t buffer_offset, buffer_count, ext_index, buffer_index, data_offset, data_count;
        uint64_t size;
        if (!rd_u32(decomp, decomp_size, o, &buffer_offset) ||
            !rd_u32(decomp, decomp_size, o + 4, &buffer_count) ||
            !rd_u32(decomp, decomp_size, o + 8, &ext_index) ||
            !rd_u32(decomp, decomp_size, o + 12, &buffer_index) ||
            !rd_u64(decomp, decomp_size, o + 16, &size) ||
            !rd_u32(decomp, decomp_size, o + 24, &data_offset) ||
            !rd_u32(decomp, decomp_size, o + 28, &data_count)) {
            free(groups);
            return 0;
        }
        groups[i].buffer_offset = buffer_offset;
        groups[i].buffer_count = buffer_count;
        groups[i].buffer_index = buffer_index;
        groups[i].data_offset = data_offset;
        groups[i].data_count = data_count;
    }

    *out_groups = groups;
    *out_count = (int)arc->num_buffer_groups;
    return 1;
}

/* Internal: dynamic set of unique uint64 offsets per pool (sorted insert). */
typedef struct {
    uint64_t *vals;
    int count, capacity;
} ovl_offset_set_t;

static void offset_set_add(ovl_offset_set_t *s, uint64_t v) {
    for (int i = 0; i < s->count; i++) if (s->vals[i] == v) return;
    if (s->count >= s->capacity) {
        int new_cap = s->capacity ? s->capacity * 2 : 8;
        uint64_t *n = (uint64_t *)realloc(s->vals, (size_t)new_cap * sizeof(uint64_t));
        if (!n) return;
        s->vals = n;
        s->capacity = new_cap;
    }
    s->vals[s->count++] = v;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int ovl_resolve_pool_sub_files(const ovl_pool_t *pools, int pool_count,
                                const ovl_root_entry_t *root_entries, int root_count,
                                const ovl_fragment_t *fragments, int fragment_count,
                                ovl_sub_file_t **out_entries, int *out_count) {
    *out_entries = NULL;
    *out_count = 0;
    if (pool_count <= 0) return 1;

    ovl_offset_set_t *sets = (ovl_offset_set_t *)calloc((size_t)pool_count, sizeof(ovl_offset_set_t));
    if (!sets) return 0;

    for (int i = 0; i < root_count; i++) {
        int32_t pidx = root_entries[i].pool_index;
        if (pidx >= 0 && pidx < pool_count) {
            offset_set_add(&sets[pidx], root_entries[i].data_offset);
        }
    }
    for (int i = 0; i < fragment_count; i++) {
        int32_t pidx = fragments[i].struct_pool;
        if (pidx >= 0 && pidx < pool_count && fragments[i].struct_offset != pools[pidx].size) {
            offset_set_add(&sets[pidx], fragments[i].struct_offset);
        }
    }

    int cap = 64, count = 0;
    ovl_sub_file_t *result = (ovl_sub_file_t *)calloc((size_t)cap, sizeof(ovl_sub_file_t));
    if (!result) { for (int i = 0; i < pool_count; i++) free(sets[i].vals); free(sets); return 0; }

    for (int p = 0; p < pool_count; p++) {
        if (sets[p].count == 0) continue;
        qsort(sets[p].vals, (size_t)sets[p].count, sizeof(uint64_t), cmp_u64);

        for (int j = 0; j < sets[p].count; j++) {
            uint64_t o = sets[p].vals[j];
            uint64_t next = (j + 1 < sets[p].count) ? sets[p].vals[j + 1] : pools[p].size;
            if (next <= o) continue;

            /* Only emit entries that have an associated file_hash (from a
               RootEntry) -- Fragment-only offsets just refine boundaries. */
            int found = 0;
            uint32_t fh = 0, eh = 0;
            for (int i = 0; i < root_count; i++) {
                if (root_entries[i].pool_index == p && root_entries[i].data_offset == o) {
                    found = 1;
                    fh = root_entries[i].file_hash;
                    eh = root_entries[i].ext_hash;
                    break;
                }
            }
            if (!found) continue;

            if (count >= cap) {
                cap *= 2;
                ovl_sub_file_t *n = (ovl_sub_file_t *)realloc(result, (size_t)cap * sizeof(ovl_sub_file_t));
                if (!n) { free(result); for (int i = 0; i < pool_count; i++) free(sets[i].vals); free(sets); return 0; }
                result = n;
            }
            result[count].pool_index = p;
            result[count].offset = o;
            result[count].size = next - o;
            result[count].file_hash = fh;
            result[count].ext_hash = eh;
            result[count].has_hash = 1;
            count++;
        }
    }

    for (int i = 0; i < pool_count; i++) free(sets[i].vals);
    free(sets);

    *out_entries = result;
    *out_count = count;
    return 1;
}

void ovl_resolve_buffer_owners(const ovl_data_entry_t *data_entries, int data_count,
                               const ovl_buffer_group_t *buffer_groups, int group_count,
                               int num_buffers, int *buffer_owner, int *buffer_sub) {
    for (int i = 0; i < num_buffers; i++) { buffer_owner[i] = -1; buffer_sub[i] = 0; }

    if (group_count == 0) {
        /* No BufferGroups (v19): the DataEntries claim the buffers in table
           order. Verified on 4,957 real v19 archives: the buffer_counts
           always add up to num_buffers, and each entry's buffer sizes sum to
           size_1 + size_2. Anything else is left unresolved. */
        uint64_t total = 0;
        for (int k = 0; k < data_count; k++) total += data_entries[k].buffer_count;
        if (total != (uint64_t)num_buffers) return;
        int buf_idx = 0;
        for (int k = 0; k < data_count; k++) {
            for (uint16_t n = 0; n < data_entries[k].buffer_count; n++, buf_idx++) {
                buffer_owner[buf_idx] = k;
                buffer_sub[buf_idx] = n;
            }
        }
        return;
    }

    /* One buffer per DataEntry and group, see ovl_buffer_group_t. */
    for (int g = 0; g < group_count; g++) {
        const ovl_buffer_group_t *bg = &buffer_groups[g];
        for (uint32_t j = 0; j < bg->data_count && j < bg->buffer_count; j++) {
            uint64_t k = (uint64_t)bg->data_offset + j;
            uint64_t b = (uint64_t)bg->buffer_offset + j;
            if (k >= (uint64_t)data_count || b >= (uint64_t)num_buffers) break;
            buffer_owner[b] = (int)k;
            buffer_sub[b] = (int)bg->buffer_index;
        }
    }
}

/* ---- ZLIB raw deflate decompression ------------------------------------------ */

int ovl_zlib_inflate_raw(const unsigned char *src, size_t src_size,
                          unsigned char *out, size_t out_size) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return 0;

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_size;
    strm.next_out = (Bytef *)out;
    strm.avail_out = (uInt)out_size;

    int ret = inflate(&strm, Z_FINISH);
    size_t produced = out_size - strm.avail_out;
    inflateEnd(&strm);

    if (ret != Z_STREAM_END || produced != out_size) return 0;
    return 1;
}
