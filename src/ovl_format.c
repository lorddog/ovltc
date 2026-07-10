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
