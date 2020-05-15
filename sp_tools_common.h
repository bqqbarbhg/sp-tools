#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	SP_COMPRESSION_NONE = 0,
	SP_COMPRESSION_ZSTD = 1,

	SP_COMPRESSION_FORCE_U32 = 0x7fffffff,
} sp_compression_type;

size_t sp_get_compression_bound(sp_compression_type type, size_t src_size);
size_t sp_compress_buffer(sp_compression_type type, void *dst, size_t dst_size, const void *src, size_t src_size, int level);
size_t sp_decompress_buffer(sp_compression_type type, void *dst, size_t dst_size, const void *src, size_t src_size);

#ifdef __cplusplus
}
#endif
