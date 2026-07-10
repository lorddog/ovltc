/**
 * ovlTC — A native Total Commander packer plugin (WCX, 64-bit) for .ovl files.
 *
 * Copyright (c) 2026 LordDog
 * Open source — feel free to use, modify, and distribute.
 * Credits are appreciated but not required.
 *
 * Parser for the OVL/FRES format used by the Cobra Engine (JWE preset).
 */
#ifndef OVL_FORMAT_H
#define OVL_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVL_COMPRESSION_NONE  0
#define OVL_COMPRESSION_ZLIB  1
#define OVL_COMPRESSION_OODLE 4

typedef struct {
    char name[240];
    char ext[64];
    uint32_t file_hash;
} ovl_file_t;

typedef struct {
    char name[64];
} ovl_mime_t;

typedef struct {
    char name[64];
    uint32_t num_pools;
    uint16_t num_datas;
    uint16_t num_pool_groups;
    uint32_t num_buffer_groups;
    uint32_t num_buffers;
    uint32_t num_fragments;
    uint32_t num_root_entries;
    uint32_t read_start;
    uint32_t set_data_size;
    uint32_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t pools_start;
    uint32_t pools_end;
} ovl_archive_t;

typedef struct {
    int version;
    uint32_t user_version;
    int compression;            /* OVL_COMPRESSION_* */
    ovl_file_t *files;
    int num_files;
    ovl_mime_t *mimes;
    int num_mimes;
    ovl_archive_t *archives;
    int num_archives;
    uint32_t data_start;
    uint32_t num_stream_files;
} ovl_header_t;

typedef struct {
    uint32_t size;
    uint32_t offset;
    uint32_t file_hash;
} ovl_pool_t;

/* Parses the FRES header. Returns 1 on success, 0 on error (errbuf filled). */
int ovl_parse_header(const unsigned char *data, size_t data_size,
                      ovl_header_t *out_header, char *errbuf, size_t errbuf_size);

void ovl_free_header(ovl_header_t *h);

/* Looks up an ovl_file_t by file_hash (linear search, fine for typical sizes). */
const ovl_file_t *ovl_find_file_by_hash(const ovl_header_t *h, uint32_t file_hash);

int ovl_parse_mempools(const unsigned char *decomp, size_t decomp_size,
                        const ovl_archive_t *arc, int version,
                        ovl_pool_t **out_pools, int *out_count);

int ovl_parse_buffer_sizes(const unsigned char *decomp, size_t decomp_size,
                            const ovl_archive_t *arc, int version,
                            uint32_t **out_sizes, int *out_count);

/* Detects known file signatures (DDS, PNG, Lua, ...), returns the extension
   including the dot, or NULL if there's no match. */
const char *ovl_detect_ext(const unsigned char *data, size_t size);

/* Replaces Windows-invalid characters (:*?"<>|) with '_', in place. */
void ovl_sanitize(char *name);

/* Raw deflate decompression (zlib, wbits=-15); the caller is responsible for
   skipping the 2-byte game-specific header. Returns 1 on success. out must
   be out_size bytes. */
int ovl_zlib_inflate_raw(const unsigned char *src, size_t src_size,
                          unsigned char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* OVL_FORMAT_H */
