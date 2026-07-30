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

/* RootEntry: one per declared file. pool_index == -1 means "no direct pool
   pointer" (e.g. resolved via the separate AssetEntry/SetEntry system, which
   this parser does not implement). */
typedef struct {
    uint32_t file_hash;
    uint32_t ext_hash;
    int32_t  pool_index;
    uint32_t data_offset;
} ovl_root_entry_t;

/* Fragment: internal pointer-patch instruction. struct_pool/struct_offset
   mark an additional boundary inside a pool -- a file's size is the distance
   to the next known offset in the same pool, whether that offset came from a
   RootEntry or a Fragment. link_pool/link_offset identify the pointer field
   itself, letting struct_pool/struct_offset be resolved as its target. */
typedef struct {
    int32_t  link_pool;
    uint32_t link_offset;
    int32_t  struct_pool;
    uint32_t struct_offset;
} ovl_fragment_t;

/* DataEntry: identifies which/how many buffers belong to a given file_hash. */
typedef struct {
    uint32_t file_hash;
    uint16_t buffer_count;
    uint64_t size_1;
    uint64_t size_2;
} ovl_data_entry_t;

/* BufferGroup: maps a contiguous range of DataEntries to a contiguous range
   of buffers (each DataEntry consumes buffer_count buffers in sequence). */
typedef struct {
    uint32_t buffer_offset;
    uint32_t buffer_count;
    uint32_t data_offset;
    uint32_t data_count;
} ovl_buffer_group_t;

/* One resolved sub-file inside a pool: byte range [offset, offset+size) is
   relative to that pool's own start. has_hash is 0 when the boundary came
   only from a Fragment (no associated file -- not emitted as an entry). */
typedef struct {
    int pool_index;
    uint64_t offset;
    uint64_t size;
    uint32_t file_hash;
    int has_hash;
} ovl_sub_file_t;

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

/* Byte offset (within the decompressed archive) where the pools' own raw
   data begins -- i.e. after ALL prefix tables (pool_groups, pools,
   data_entries, buffer_entries, buffer_groups, root_entries, fragments,
   set_header). Falls back to arc->set_data_size when none of the extended
   tables (num_datas/num_root_entries/num_fragments) are present, which is
   equivalent to the previously used simple formula. */
uint32_t ovl_compute_pool_region_start(const unsigned char *decomp, size_t decomp_size,
                                        const ovl_archive_t *arc, int version);

int ovl_parse_root_entries(const unsigned char *decomp, size_t decomp_size,
                            const ovl_archive_t *arc, int version,
                            ovl_root_entry_t **out_entries, int *out_count);

int ovl_parse_fragments(const unsigned char *decomp, size_t decomp_size,
                         const ovl_archive_t *arc, int version,
                         ovl_fragment_t **out_fragments, int *out_count);

int ovl_parse_data_entries(const unsigned char *decomp, size_t decomp_size,
                            const ovl_archive_t *arc, int version,
                            ovl_data_entry_t **out_entries, int *out_count);

int ovl_parse_buffer_groups(const unsigned char *decomp, size_t decomp_size,
                             const ovl_archive_t *arc, int version,
                             ovl_buffer_group_t **out_groups, int *out_count);

/* Resolves each pool's bundled sub-files via sorted offset differences
   (RootEntry + Fragment boundaries). Only boundaries with an associated
   file_hash (from a RootEntry) are returned as entries; Fragment-only
   boundaries merely refine segment sizes. Caller frees *out_entries. */
int ovl_resolve_pool_sub_files(const ovl_pool_t *pools, int pool_count,
                                const ovl_root_entry_t *root_entries, int root_count,
                                const ovl_fragment_t *fragments, int fragment_count,
                                ovl_sub_file_t **out_entries, int *out_count);

/* Maps buffer index -> file_hash via DataEntry+BufferGroup. buffer_hash_found[i]
   is 0 if buffer i has no known owner (both arrays must have num_buffers
   entries, allocated by the caller). */
void ovl_resolve_buffer_names(const ovl_data_entry_t *data_entries, int data_count,
                               const ovl_buffer_group_t *buffer_groups, int group_count,
                               int num_buffers,
                               uint32_t *buffer_hash, int *buffer_hash_found);

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
