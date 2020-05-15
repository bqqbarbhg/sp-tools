#include "sp_tools_common.h"
#include "ext/zstd.h"
#include <assert.h>

size_t sp_get_compression_bound(sp_compression_type type, size_t src_size)
{
	switch (type)
	{
	case SP_COMPRESSION_NONE: return src_size;
	case SP_COMPRESSION_ZSTD: return ZSTD_compressBound(src_size);
	default: return 0;
	}
}

size_t sp_compress_buffer(sp_compression_type type, void *dst, size_t dst_size, const void *src, size_t src_size, int level)
{
	if (level < 1) level = 1;
	if (level > 20) level = 20;
	switch (type)
	{
	case SP_COMPRESSION_NONE:
		assert(dst_size >= src_size);
		memcpy(dst, src, src_size);
		return src_size;
	case SP_COMPRESSION_ZSTD:
		return ZSTD_compress(dst, dst_size, src, src_size, level - 1);
	default: return 0;
	}
}

size_t sp_decompress_buffer(sp_compression_type type, void *dst, size_t dst_size, const void *src, size_t src_size)
{
	switch (type)
	{
	case SP_COMPRESSION_NONE:
		assert(dst_size >= src_size);
		memcpy(dst, src, src_size);
		return src_size;
	case SP_COMPRESSION_ZSTD:
		return ZSTD_decompress(dst, dst_size, src, src_size);
	default: return 0;
	}
}
