#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	SP_COMPRESSION_NONE = 0,
	SP_COMPRESSION_ZSTD = 1,

	SP_COMPRESSION_FORCE_U32 = 0x7fffffff,
} sp_compression_type;

typedef enum sp_magic {
	SP_MAGIC_SPTEX = 0x78747073, // 'sptx'
	SP_MAGIC_SPMDL = 0x646d7073, // 'spmd'

	SP_MAGIC_FORCE_U32 = 0x7fffffff,
} sp_magic;

typedef enum sp_format {

	SP_FORMAT_UNKNOWN,

	// Basic 8 bits per component
	SP_FORMAT_R8_UNORM, SP_FORMAT_R8_SNORM, SP_FORMAT_R8_UINT, SP_FORMAT_R8_SINT,
	SP_FORMAT_RG8_UNORM, SP_FORMAT_RG8_SNORM, SP_FORMAT_RG8_UINT, SP_FORMAT_RG8_SINT,
	SP_FORMAT_RGB8_UNORM, SP_FORMAT_RGB8_SNORM, SP_FORMAT_RGB8_UINT, SP_FORMAT_RGB8_SINT, SP_FORMAT_RGB8_SRGB,
	SP_FORMAT_RGBA8_UNORM, SP_FORMAT_RGBA8_SNORM, SP_FORMAT_RGBA8_UINT, SP_FORMAT_RGBA8_SINT, SP_FORMAT_RGBA8_SRGB,

	// Basic 16 bits per component
	SP_FORMAT_R16_UNORM, SP_FORMAT_R16_SNORM, SP_FORMAT_R16_UINT, SP_FORMAT_R16_SINT, SP_FORMAT_R16_FLOAT,
	SP_FORMAT_RG16_UNORM, SP_FORMAT_RG16_SNORM, SP_FORMAT_RG16_UINT, SP_FORMAT_RG16_SINT, SP_FORMAT_RG16_FLOAT,
	SP_FORMAT_RGB16_UNORM, SP_FORMAT_RGB16_SNORM, SP_FORMAT_RGB16_UINT, SP_FORMAT_RGB16_SINT, SP_FORMAT_RGB16_FLOAT,
	SP_FORMAT_RGBA16_UNORM, SP_FORMAT_RGBA16_SNORM, SP_FORMAT_RGBA16_UINT, SP_FORMAT_RGBA16_SINT, SP_FORMAT_RGBA16_FLOAT,

	// Basic 32 bits per component
	SP_FORMAT_R32_UNORM, SP_FORMAT_R32_SNORM, SP_FORMAT_R32_UINT, SP_FORMAT_R32_SINT, SP_FORMAT_R32_FLOAT,
	SP_FORMAT_RG32_UNORM, SP_FORMAT_RG32_SNORM, SP_FORMAT_RG32_UINT, SP_FORMAT_RG32_SINT, SP_FORMAT_RG32_FLOAT,
	SP_FORMAT_RGB32_UNORM, SP_FORMAT_RGB32_SNORM, SP_FORMAT_RGB32_UINT, SP_FORMAT_RGB32_SINT, SP_FORMAT_RGB32_FLOAT,
	SP_FORMAT_RGBA32_UNORM, SP_FORMAT_RGBA32_SNORM, SP_FORMAT_RGBA32_UINT, SP_FORMAT_RGBA32_SINT, SP_FORMAT_RGBA32_FLOAT,

	// Depth buffer
	SP_FORMAT_D16_UNORM,
	SP_FORMAT_D24S8_UNORM,
	SP_FORMAT_D32_FLOAT,
	SP_FORMAT_D32S8_FLOAT,

	// Bit-packing
	SP_FORMAT_RGB10A2_UNORM, SP_FORMAT_RGB10A2_UINT,
	SP_FORMAT_R11G11B10_FLOAT,

	// BCn compression
	SP_FORMAT_BC1_UNORM, SP_FORMAT_BC1_SRGB,
	SP_FORMAT_BC2_UNORM, SP_FORMAT_BC2_SRGB,
	SP_FORMAT_BC3_UNORM, SP_FORMAT_BC3_SRGB,
	SP_FORMAT_BC4_UNORM, SP_FORMAT_BC4_SNORM,
	SP_FORMAT_BC5_UNORM, SP_FORMAT_BC5_SNORM,
	SP_FORMAT_BC6_UFLOAT, SP_FORMAT_BC6_SFLOAT,
	SP_FORMAT_BC7_UNORM, SP_FORMAT_BC7_SRGB,

	// ASTC compression (2D)
	SP_FORMAT_ASTC4X4_UNORM, SP_FORMAT_ASTC4X4_SRGB,
	SP_FORMAT_ASTC5X4_UNORM, SP_FORMAT_ASTC5X4_SRGB,
	SP_FORMAT_ASTC5X5_UNORM, SP_FORMAT_ASTC5X5_SRGB,
	SP_FORMAT_ASTC6X5_UNORM, SP_FORMAT_ASTC6X5_SRGB,
	SP_FORMAT_ASTC6X6_UNORM, SP_FORMAT_ASTC6X6_SRGB,
	SP_FORMAT_ASTC8X5_UNORM, SP_FORMAT_ASTC8X5_SRGB,
	SP_FORMAT_ASTC8X6_UNORM, SP_FORMAT_ASTC8X6_SRGB,
	SP_FORMAT_ASTC10X5_UNORM, SP_FORMAT_ASTC10X5_SRGB,
	SP_FORMAT_ASTC10X6_UNORM, SP_FORMAT_ASTC10X6_SRGB,
	SP_FORMAT_ASTC8X8_UNORM, SP_FORMAT_ASTC8X8_SRGB,
	SP_FORMAT_ASTC10X8_UNORM, SP_FORMAT_ASTC10X8_SRGB,
	SP_FORMAT_ASTC10X10_UNORM, SP_FORMAT_ASTC10X10_SRGB,
	SP_FORMAT_ASTC12X10_UNORM, SP_FORMAT_ASTC12X10_SRGB,
	SP_FORMAT_ASTC12X12_UNORM, SP_FORMAT_ASTC12X12_SRGB,

	// Special footer
	SP_FORMAT_COUNT,
	SP_FORMAT_FORCE_U32 = 0x7fffffff,

} sp_format;

typedef enum sp_vertex_attrib {
	SP_VERTEX_ATTRIB_POSITION,
	SP_VERTEX_ATTRIB_NORMAL,
	SP_VERTEX_ATTRIB_UV,
	SP_VERTEX_ATTRIB_COLOR,
	SP_VERTEX_ATTRIB_BONE_INDEX,
	SP_VERTEX_ATTRIB_BONE_WEIGHT,

	SP_VERTEX_ATTRIB_COUNT,
	SP_VERTEX_ATTRIB_FORCE_U32 = 0x7fffffff,
} sp_vertex_attrib;

typedef enum sp_format_flags {
	SP_FORMAT_FLAG_INTEGER = 0x1,
	SP_FORMAT_FLAG_FLOAT = 0x2,
	SP_FORMAT_FLAG_NORMALIZED = 0x4,
	SP_FORMAT_FLAG_SIGNED = 0x8,
	SP_FORMAT_FLAG_SRGB = 0x10,
	SP_FORMAT_FLAG_COMPRESSED = 0x20,
	SP_FORMAT_FLAG_DEPTH = 0x40,
	SP_FORMAT_FLAG_STENCIL = 0x80,
	SP_FORMAT_FLAG_BASIC = 0x100,
} sp_format_flags;

typedef struct sp_format_info {
	sp_format format;
	const char *enum_name;
	const char *short_name;
	uint32_t num_components;
	uint32_t block_size;
	uint32_t block_x, block_y;
	uint32_t flags;
} sp_format_info;

sp_format sp_find_format(uint32_t num_components, uint32_t component_size, sp_format_flags flags);

extern const sp_format_info sp_format_infos[SP_FORMAT_COUNT];

size_t sp_get_compression_bound(sp_compression_type type, size_t src_size);
size_t sp_compress_buffer(sp_compression_type type, void *dst, size_t dst_size, const void *src, size_t src_size, int level);
size_t sp_decompress_buffer(sp_compression_type type, void *dst, size_t dst_size, const void *src, size_t src_size);

typedef struct sptex_mip {
	uint32_t width, height;
	uint32_t compressed_data_offset;
	uint32_t compressed_data_size;
	uint32_t uncompressed_data_size;
	sp_compression_type compression_type;
} sptex_mip;

typedef struct sptex_header {
	sp_magic magic; // = SP_MAGIC_SPTEX
	uint32_t file_version;
	sp_format format;
	uint32_t file_size;
	uint32_t width, height;
	uint32_t uncropped_width, uncropped_height;
	uint32_t crop_min_x, crop_min_y;
	uint32_t crop_max_x, crop_max_y;
	uint32_t num_mips;
	sptex_mip mips[16];
} sptex_header;

#define SPMDL_MAX_VERTEX_BUFFERS 4
#define SPMDL_MAX_VERTEX_ATTRIBS 16

typedef struct spmdl_vec3
{
	float x, y, z;
} spmdl_vec3;

typedef struct spmdl_vec4
{
	float x, y, z, w;
} spmdl_vec4;

typedef struct spmdl_matrix
{
	spmdl_vec3 columns[4];
} spmdl_matrix;

typedef struct spmdl_string
{
	uint32_t offset;
	uint32_t length;
} spmdl_string;

typedef struct spmdl_header
{
	sp_magic magic; // = SP_MAGIC_SPMD
	uint32_t version;
	uint32_t num_nodes;
	uint32_t num_meshes;
	uint32_t num_bones;
	uint32_t num_materials;
	uint32_t string_data_size;
	uint32_t geometry_data_size;
} spmdl_header;

typedef struct spmdl_node
{
	uint32_t parent;
	spmdl_string name;
	spmdl_vec3 translation;
	spmdl_vec4 rotation;
	spmdl_vec3 scale;
	spmdl_matrix self_to_parent;
	spmdl_matrix self_to_root;
} spmdl_node;

typedef struct spmdl_bone
{
	uint32_t node;
	spmdl_matrix mesh_to_bone;
	uint32_t bone_offset;
	uint32_t num_bones;
} spmdl_bone;

typedef struct spmdl_buffer
{
	sp_compression_type compression;
	uint32_t flags;
	uint32_t data_offset;
	uint32_t compressed_size;
	uint32_t encoded_size;
	uint32_t uncompressed_size;
	uint32_t stride;
} spmdl_buffer;

typedef struct spmdl_attrib
{
	sp_vertex_attrib attrib;
	sp_format format;
	uint32_t stream;
	uint32_t offset;
} spmdl_attrib;

typedef struct spmdl_material
{
	spmdl_string name;
} spmdl_material;

typedef struct spmdl_mesh
{
	uint32_t node;
	uint32_t material;
	uint32_t num_indices;
	uint32_t num_vertices;
	uint32_t num_vertex_buffers;
	uint32_t num_elements;
	spmdl_buffer vertex_buffers[SPMDL_MAX_VERTEX_BUFFERS];
	spmdl_buffer index_buffer;
	spmdl_attrib attribs[SPMDL_MAX_VERTEX_ATTRIBS];
} spmdl_mesh;

#ifdef __cplusplus
}
#endif
