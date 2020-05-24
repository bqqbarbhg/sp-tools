#define _CRT_SECURE_NO_WARNINGS

#include "../sp_tools_common.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <limits>
#include "ufbx.h"
#include "meshoptimizer/meshoptimizer.h"
#include "acl/core/ansi_allocator.h"
#include "acl/algorithm/uniformly_sampled/encoder.h"
#include "acl/compression/animation_clip.h"
#include "acl/compression/compress.h"
#include "rh_hash.h"
#include "json_output.h"
#include "mikktspace.h"

bool g_verbose;

void failf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	vfprintf(stderr, fmt, args);
	putc('\n', stderr);

	va_end(args);

	exit(1);
}

struct attrib_info {
	const char *code;
	const char *name;
	sp_vertex_attrib type;
};

attrib_info attrib_list[] = {
	{ "pos", "position", SP_VERTEX_ATTRIB_POSITION },
	{ "nrm", "normal", SP_VERTEX_ATTRIB_NORMAL },
	{ "tan", "tangent", SP_VERTEX_ATTRIB_TANGENT },
	{ "tsgn", "tangent-sign", SP_VERTEX_ATTRIB_TANGENT_SIGN },
	{ "uv", "uv", SP_VERTEX_ATTRIB_UV },
	{ "col", "color", SP_VERTEX_ATTRIB_COLOR },
	{ "bonei", "bone-index", SP_VERTEX_ATTRIB_BONE_INDEX },
	{ "bonew", "bone-weight", SP_VERTEX_ATTRIB_BONE_WEIGHT },
	{ "pad", "padding", SP_VERTEX_ATTRIB_PADDING },
};

struct vertex_format {
	uint32_t num_attribs;
	uint32_t num_streams;
	spmdl_attrib attribs[SPMDL_MAX_VERTEX_ATTRIBS];
	uint32_t stream_stride[SPMDL_MAX_VERTEX_BUFFERS];
};

static sp_vertex_attrib find_attrib(const char *name)
{
	for (attrib_info &info : attrib_list) {
		if (!strcmp(info.code, name)) {
			return info.type;
		}
	}
	failf("Unknown attribute '%s'", name);
	return SP_VERTEX_ATTRIB_COUNT;
}

static sp_format find_format(const char *name)
{
	for (uint32_t i = 1; i < SP_FORMAT_COUNT; i++) {
		if (!strcmp(sp_format_infos[i].short_name, name)) {
			return (sp_format)i;
		}
	}
	failf("Unknown format '%s'", name);
	return SP_FORMAT_UNKNOWN;
}

vertex_format parse_attribs(const char *str)
{
	vertex_format fmt = { };
	fmt.num_streams = 1;

	uint32_t offset = 0;

	const char *ptr = str;
	for (;;) {
		char attrib_name[32];
		char format_name[64];
		size_t attrib_len = 0;
		size_t format_len = 0;

		if (++fmt.num_attribs > SPMDL_MAX_VERTEX_ATTRIBS) failf("Too many vertex attributes");

		while (*ptr != '_') {
			if (!*ptr) failf("Expected vertex attribute 'name_format' tuple, eg. 'pos_rgb3f'");
			if (format_len + 1 >= sizeof(attrib_name)) failf("Attribute name too long");
			attrib_name[attrib_len++] = *ptr++;
		}
		attrib_name[attrib_len] = '\0';
		ptr++;

		while (*ptr != ';' && *ptr != ',' && *ptr != '\0') {
			if (format_len + 1 >= sizeof(format_name)) failf("Format name too long");
			format_name[format_len++] = *ptr++;
		}
		format_name[format_len] = '\0';

		spmdl_attrib &attrib = fmt.attribs[fmt.num_attribs - 1];
		attrib.attrib = find_attrib(attrib_name);
		attrib.format = find_format(format_name);
		attrib.stream = fmt.num_streams - 1;
		attrib.offset = offset;

		offset += sp_format_infos[attrib.format].block_size;
		if (*ptr == ';') {
			fmt.stream_stride[fmt.num_streams - 1] = offset;
			offset = 0;
			if (++fmt.num_streams > SPMDL_MAX_VERTEX_BUFFERS) failf("Too many vertex streams");
		} else if (*ptr == '\0') {
			break;
		}
		ptr++;
	}

	fmt.stream_stride[fmt.num_streams - 1] = offset;

	return fmt;
}

struct mesh_opts
{
	vertex_format format;
	bool transform_to_root = false;
};

struct mesh_data_format
{
	uint32_t weights_per_vertex = 0;
	uint32_t vertex_size_in_floats = 0;
	uint32_t position_offset_in_floats = ~0u;
	uint8_t attrib_size_in_floats[SPMDL_MAX_VERTEX_ATTRIBS] = { };
};

struct mesh_bone
{
	ufbx_node *node;
	ufbx_matrix mesh_to_bind;
};

struct bone_weight
{
	int32_t index;
	float weight;
};

struct mesh_part
{
	ufbx_mesh *mesh = nullptr;
	vertex_format format;
	mesh_data_format data_format;
	ufbx_material *material = nullptr;
	size_t num_indices = 0;
	size_t num_vertices = 0;
	ufbx_node *root_bone;
	rh::array<mesh_bone> bones;
	rh::array<float> vertex_data;
	rh::array<uint32_t> index_data;
};

struct mesh_part_source
{
	ufbx_vec3 *positions = nullptr;
	ufbx_vec3 *normals = nullptr;
	bone_weight *weights = nullptr;
	rh::array<uint32_t> indices;
	ufbx_node *root_bone = nullptr;
	rh::array<mesh_bone> bones;
};

struct split_part
{
	rh::hash_map<int32_t, uint32_t> bone_refs;
	rh::hash_map<uint32_t, uint32_t> vertex_refs;

	rh::array<uint32_t> vertex_indices;
	rh::array<uint32_t> bone_indices;
	rh::array<uint32_t> indices;
};

struct mesh_limits
{
	uint32_t max_vertices;
	uint32_t max_bones;
};

struct merge_key
{
	ufbx_material *material;
	vertex_format format;

	bool operator==(const merge_key &rhs) const {
		return material == rhs.material && !memcmp(&format, &rhs.format, sizeof(format));
	}
};

struct attrib_bounds
{
	float min[4];
	float max[4];
	uint32_t num_components;
};

static uint32_t hash(const merge_key &key) { return rh::hash_buffer_align4(&key, sizeof(key)); }

mesh_data_format create_mesh_data_format(const vertex_format &format)
{
	mesh_data_format fmt = { };

	for (uint32_t i = 0; i < format.num_attribs; i++) {
		const spmdl_attrib &attrib = format.attribs[i];

		uint32_t attrib_size_in_floats = 0;
		switch (attrib.attrib) {
		case SP_VERTEX_ATTRIB_POSITION:
			fmt.position_offset_in_floats = fmt.vertex_size_in_floats;
			attrib_size_in_floats = 3;
			break;
		case SP_VERTEX_ATTRIB_NORMAL: attrib_size_in_floats = 3; break;
		case SP_VERTEX_ATTRIB_TANGENT: attrib_size_in_floats = 4; break;
		case SP_VERTEX_ATTRIB_TANGENT_SIGN: attrib_size_in_floats = 1; break;
		case SP_VERTEX_ATTRIB_UV: attrib_size_in_floats = 2; break;
		case SP_VERTEX_ATTRIB_COLOR: attrib_size_in_floats = 4; break;
		case SP_VERTEX_ATTRIB_BONE_INDEX:
		case SP_VERTEX_ATTRIB_BONE_WEIGHT:
			attrib_size_in_floats = sp_format_infos[attrib.format].num_components;
			if (fmt.weights_per_vertex == 0) {
				fmt.weights_per_vertex = attrib_size_in_floats;
			} else if (fmt.weights_per_vertex != attrib_size_in_floats) {
				failf("Vertex attribute bone/weight size mismatch");
			}
			break;
		case SP_VERTEX_ATTRIB_PADDING: attrib_size_in_floats = 0; break;
		}

		fmt.attrib_size_in_floats[i] = (uint8_t)attrib_size_in_floats;
		fmt.vertex_size_in_floats += attrib_size_in_floats;
	}

	return fmt;
}

struct tangent_generator
{
	float *vertex_data = nullptr;
	uint32_t vertex_stride = ~0u;
	uint32_t num_triangles = ~0u;
	uint32_t position_offset = ~0u;
	uint32_t normal_offset = ~0u;
	uint32_t uv_offset = ~0u;
	uint32_t tangent_offset = ~0u;
	uint32_t tangent_sign_offset = ~0u;
};

int tangent_getNumFaces(const SMikkTSpaceContext * pContext)
{
	tangent_generator *tg = (tangent_generator*)pContext->m_pUserData;
	return tg->num_triangles;
}

int tangent_getNumVerticesOfFace(const SMikkTSpaceContext * pContext, const int iFace)
{
	return 3;
}

void tangent_getPosition(const SMikkTSpaceContext * pContext, float fvPosOut[], const int iFace, const int iVert)
{
	tangent_generator *tg = (tangent_generator*)pContext->m_pUserData;
	const float* v = tg->vertex_data + (iFace * 3 + iVert) * tg->vertex_stride + tg->position_offset;
	fvPosOut[0] = v[0]; fvPosOut[1] = v[1]; fvPosOut[2] = v[2];
}

void tangent_getNormal(const SMikkTSpaceContext * pContext, float fvNormOut[], const int iFace, const int iVert)
{
	tangent_generator *tg = (tangent_generator*)pContext->m_pUserData;
	const float* v = tg->vertex_data + (iFace * 3 + iVert) * tg->vertex_stride + tg->normal_offset;
	fvNormOut[0] = v[0]; fvNormOut[1] = v[1]; fvNormOut[2] = v[2];
}

void tangent_getTexCoord(const SMikkTSpaceContext * pContext, float fvTexcOut[], const int iFace, const int iVert)
{
	tangent_generator *tg = (tangent_generator*)pContext->m_pUserData;
	const float* v = tg->vertex_data + (iFace * 3 + iVert) * tg->vertex_stride + tg->uv_offset;
	fvTexcOut[0] = v[0]; fvTexcOut[1] = v[1];
}

void tangent_setTSpaceBasic(const SMikkTSpaceContext * pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert)
{
	tangent_generator *tg = (tangent_generator*)pContext->m_pUserData;
	float* dst = tg->vertex_data + (iFace * 3 + iVert) * tg->vertex_stride + tg->tangent_offset;
	dst[0] = fvTangent[0]; dst[1] = fvTangent[1]; dst[2] = fvTangent[2]; dst[3] = fSign;

	if (tg->tangent_sign_offset != ~0u) {
		float* sdst = tg->vertex_data + (iFace * 3 + iVert) * tg->vertex_stride + tg->tangent_sign_offset;
		sdst[0] = fSign;
	}
}

SMikkTSpaceInterface tangent_interface = {
	&tangent_getNumFaces,
	&tangent_getNumVerticesOfFace,
	&tangent_getPosition,
	&tangent_getNormal,
	&tangent_getTexCoord,
	&tangent_setTSpaceBasic,
};

mesh_part process_mesh_part(ufbx_mesh *mesh, const mesh_data_format &fmt, const mesh_opts &opts, const mesh_part_source &src, uint32_t material_ix)
{
	mesh_part part;
	part.mesh = mesh;
	part.format = opts.format;
	part.data_format = fmt;
	part.root_bone = src.root_bone;

	if (mesh->materials.size) {
		part.material = mesh->materials.data[material_ix];
	} else {
		part.material = nullptr;
	}

	rh::array<float> vertex_data;
	vertex_data.resize(fmt.vertex_size_in_floats * src.indices.size());

	rh::array<int32_t> bone_remap;
	bone_remap.resize(src.bones.size(), -1);

	uint32_t normal_offset_in_floats = ~0u;
	uint32_t uv_offset_in_floats = ~0u;
	uint32_t tangent_offset_in_floats = ~0u;
	uint32_t tangent_sign_offset_in_floats = ~0u;
	size_t stride_in_floats = fmt.vertex_size_in_floats;

	uint32_t attrib_offset_in_floats = 0;
	for (uint32_t i = 0; i < opts.format.num_attribs; i++) {
		const spmdl_attrib &attrib = opts.format.attribs[i];

		float *dst = vertex_data.data() + attrib_offset_in_floats;
		switch (attrib.attrib) {

		case SP_VERTEX_ATTRIB_POSITION:
			if (src.positions) {
				for (size_t i : src.indices) {
					ufbx_vec3 v = src.positions[mesh->vertex_position.indices[i]];
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_NORMAL:
			normal_offset_in_floats = attrib_offset_in_floats;
			if (src.normals) {
				for (size_t i : src.indices) {
					ufbx_vec3 v = src.normals[mesh->vertex_normal.indices[i]];
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_TANGENT:
			tangent_offset_in_floats = attrib_offset_in_floats;
			break;

		case SP_VERTEX_ATTRIB_TANGENT_SIGN:
			tangent_sign_offset_in_floats = attrib_offset_in_floats;
			break;

		case SP_VERTEX_ATTRIB_UV:
			uv_offset_in_floats = attrib_offset_in_floats;
			if (mesh->vertex_uv.data) {
				for (size_t i : src.indices) {
					ufbx_vec2 v = ufbx_get_vertex_vec2(&mesh->vertex_uv, i);
					dst[0] = (float)v.x; dst[1] = (float)v.y;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_COLOR:
			if (mesh->vertex_color.data) {
				for (size_t i : src.indices) {
					ufbx_vec4 v = ufbx_get_vertex_vec4(&mesh->vertex_color, i);
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z; dst[3] = (float)v.w;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_BONE_INDEX:
			if (src.weights) {
				for (size_t i : src.indices) {
					bone_weight *w = src.weights + mesh->vertex_position.indices[i] * fmt.weights_per_vertex;
					for (size_t j = 0; j < fmt.weights_per_vertex; j++) {
						int32_t &ix = bone_remap[w[j].index];
						if (ix < 0) {
							ix = (int32_t)part.bones.size();
							part.bones.push_back(src.bones[w[j].index]);
						}
						dst[j] = (float)ix;
					}
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_BONE_WEIGHT:
			if (src.weights) {
				for (size_t i : src.indices) {
					bone_weight *w = src.weights + mesh->vertex_position.indices[i] * fmt.weights_per_vertex;
					for (size_t j = 0; j < fmt.weights_per_vertex; j++) {
						dst[j] = w[j].weight;
					}
					dst += stride_in_floats;
				}
			}
			break;
		}

		attrib_offset_in_floats += (uint32_t)fmt.attrib_size_in_floats[i];
	}

	if (normal_offset_in_floats != ~0u && uv_offset_in_floats != ~0u && tangent_offset_in_floats != ~0u) {
		tangent_generator tg;
		tg.vertex_data = vertex_data.data();
		tg.num_triangles = (uint32_t)src.indices.size() / 3;
		tg.vertex_stride = (uint32_t)stride_in_floats;
		tg.position_offset = fmt.position_offset_in_floats;
		tg.normal_offset = normal_offset_in_floats;
		tg.uv_offset = uv_offset_in_floats;
		tg.tangent_offset = tangent_offset_in_floats;
		tg.tangent_sign_offset = tangent_sign_offset_in_floats;

		SMikkTSpaceContext ctx;
		ctx.m_pInterface = &tangent_interface;
		ctx.m_pUserData = &tg;
		genTangSpaceDefault(&ctx);
	}

	if (src.indices.size() > 0 && fmt.vertex_size_in_floats > 0) {
		rh::array<uint32_t> remap;
		remap.resize_uninit(src.indices.size());

		size_t vertex_size = fmt.vertex_size_in_floats * sizeof(float);

		part.num_indices = src.indices.size();
		part.num_vertices = meshopt_generateVertexRemap(remap.data(), NULL, part.num_indices,
			vertex_data.data(), part.num_indices, vertex_size);

		part.index_data.resize_uninit(part.num_indices);
		part.vertex_data.resize_uninit(part.num_vertices * fmt.vertex_size_in_floats);

		meshopt_remapIndexBuffer(part.index_data.data(), NULL, part.num_indices, remap.data());
		meshopt_remapVertexBuffer(part.vertex_data.data(), vertex_data.data(), part.num_indices,
			vertex_size, remap.data());

		meshopt_optimizeVertexCache(part.index_data.data(), part.index_data.data(), part.num_indices, part.num_vertices);
	}

	return part;
}

void merge_mesh_part(mesh_part &dst, const mesh_part &src)
{
	rh::array<uint32_t> bone_remap;

	if (!src.bones.empty()) {
		bone_remap.reserve(src.bones.size());
		for (const mesh_bone &src_bone : src.bones) {
			bool found = false;
			uint32_t index = (uint32_t)dst.bones.size();
			if (dst.bones.size() < 512) {
				for (index = 0; index < dst.bones.size(); index++) {
					mesh_bone &dst_bone = dst.bones[index];
					if (src_bone.node == dst_bone.node && !memcmp(&src_bone.mesh_to_bind, &dst_bone.mesh_to_bind, sizeof(ufbx_matrix))) {
						break;
					}
				}
			}
			bone_remap.push_back(index);
			if (index == dst.bones.size()) {
				dst.bones.push_back(src_bone);
			}
		}
	}

	dst.index_data.reserve(dst.index_data.size() + src.index_data.size());
	dst.vertex_data.reserve(dst.vertex_data.size() + src.vertex_data.size());

	uint32_t index_offset = (uint32_t)dst.num_vertices;
	for (uint32_t ix : src.index_data) {
		dst.index_data.push_back(ix + index_offset);
	}

	dst.vertex_data.insert_back(src.vertex_data.data(), src.vertex_data.size());

	// Remap bone indices

	if (!src.bones.empty()) {
		float *src_vertex_begin = dst.vertex_data.data() + dst.data_format.vertex_size_in_floats * dst.num_vertices;
		uint32_t attrib_offset_in_floats = 0;
		for (uint32_t i = 0; i < dst.format.num_attribs; i++) {
			const spmdl_attrib &attrib = dst.format.attribs[i];

			size_t stride_in_floats = dst.data_format.vertex_size_in_floats;
			switch (attrib.attrib) {

			case SP_VERTEX_ATTRIB_BONE_INDEX:
				{
					float *dst_data = dst.vertex_data.data() + attrib_offset_in_floats + dst.num_vertices * stride_in_floats;
					for (size_t vi = 0; vi < src.num_vertices; vi++) {
						for (size_t j = 0; j < dst.data_format.weights_per_vertex; j++) {
							dst_data[j] = (float)bone_remap[(int32_t)dst_data[j]];
						}
						dst_data += stride_in_floats;
					}
				}
				break;

			}

			attrib_offset_in_floats += (uint32_t)dst.data_format.attrib_size_in_floats[i];
		}
	}

	dst.num_vertices += src.num_vertices;
	dst.num_indices += src.num_indices;
}

bool add_triangle(split_part &part, const uint32_t *indices, const int32_t *bone_indices, uint32_t weights_per_vertex, const mesh_limits &limits)
{
	size_t prev_num_vertices = part.vertex_indices.size();
	size_t prev_num_bones = part.bone_indices.size();

	for (uint32_t ci = 0; ci < 3; ci++) {
		uint32_t vertex_ix = indices[ci];

		{
			auto res = part.vertex_refs.emplace(vertex_ix, (uint32_t)part.vertex_indices.size());
			if (res.inserted) {
				part.vertex_indices.push_back(vertex_ix);
			}
			part.indices.push_back(res.entry->value);
		}

		const int32_t *bone_index_base = bone_indices + vertex_ix * weights_per_vertex;
		for (uint32_t bi = 0; bi < weights_per_vertex; bi++) {
			auto res = part.bone_refs.emplace(bone_index_base[bi], (uint32_t)part.bone_indices.size());
			if (res.inserted) {
				part.bone_indices.push_back(bone_index_base[bi]);
			}
		}
	}

	assert(part.vertex_refs.size() <= part.vertex_indices.size());
	assert(part.bone_refs.size() <= part.bone_indices.size());

	if (part.vertex_indices.size() <= limits.max_vertices && part.bone_indices.size() <= limits.max_bones) {
		return true;
	}

	for (size_t i = prev_num_vertices; i < part.vertex_indices.size(); i++) {
		part.vertex_refs.remove(part.vertex_indices[i]);
	}

	for (size_t i = prev_num_bones; i < part.bone_indices.size(); i++) {
		part.bone_refs.remove(part.bone_indices[i]);
	}

	assert(part.vertex_refs.size() <= limits.max_vertices);
	assert(part.bone_refs.size() <= limits.max_bones);

	part.indices.resize(part.indices.size() - 3);
	part.vertex_indices.resize(prev_num_vertices);
	part.bone_indices.resize(prev_num_bones);

	return false;
}

rh::array<mesh_part> split_mesh(mesh_part src, const mesh_limits &limits)
{
	rh::array<mesh_part> parts;

	if (src.num_vertices <= limits.max_vertices && src.bones.size() <= limits.max_bones) {
		parts.push_back(std::move(src));
		return parts;
	}

	rh::array<split_part> splits;
	splits.emplace_back();

	rh::array<int32_t> bone_indices;
	bone_indices.reserve(src.num_vertices * src.data_format.weights_per_vertex);

	{
		uint32_t attrib_offset_in_floats = 0;
		for (uint32_t i = 0; i < src.format.num_attribs; i++) {
			const spmdl_attrib &attrib = src.format.attribs[i];

			size_t stride_in_floats = src.data_format.vertex_size_in_floats;
			float *src_data = src.vertex_data.data() + attrib_offset_in_floats;
			switch (attrib.attrib) {

			case SP_VERTEX_ATTRIB_BONE_INDEX:
				{
					for (size_t vi = 0; vi < src.num_vertices; vi++) {
						for (size_t j = 0; j < src.data_format.weights_per_vertex; j++) {
							bone_indices.push_back((int32_t)src_data[j]);
						}
						src_data += stride_in_floats;
					}
				}
				break;

			}

			attrib_offset_in_floats += (uint32_t)src.data_format.attrib_size_in_floats[i];
		}
	}

	for (uint32_t tri_base = 0; tri_base < src.num_indices; tri_base += 3) {

		uint32_t *indices = src.index_data.data() + tri_base;
		bool inserted = false;
		for (split_part &split : splits) {
			if (add_triangle(split, indices, bone_indices.data(), src.data_format.weights_per_vertex, limits)) {
				inserted = true;
				break;
			}
		}

		if (!inserted) {
			splits.emplace_back();
			inserted = add_triangle(splits.back(), indices, bone_indices.data(), src.data_format.weights_per_vertex, limits);
			if (!inserted) failf("Failed to split mesh, limits too tight for a single triangle");
		}

	}

	parts.reserve(splits.size());

	for (split_part &split : splits) {
		mesh_part part;
		part.mesh = src.mesh;
		part.format = src.format;
		part.data_format = src.data_format;
		part.material = src.material;
		part.root_bone = src.root_bone;

		part.vertex_data.reserve(split.vertex_indices.size() * src.data_format.vertex_size_in_floats);
		part.bones.reserve(split.bone_indices.size());

		if (src.bones.size() > 0) {
			for (uint32_t bone_ix : split.bone_indices) {
				part.bones.push_back(src.bones[bone_ix]);
			}
		}

		for (uint32_t vertex_ix : split.vertex_indices) {
			const float *vert = src.vertex_data.data() + vertex_ix * src.data_format.vertex_size_in_floats;
			part.vertex_data.insert_back(vert, src.data_format.vertex_size_in_floats);
		}

		part.num_indices = split.indices.size();
		part.num_vertices = split.vertex_indices.size();

		part.index_data = std::move(split.indices);

		{
			uint32_t attrib_offset_in_floats = 0;
			for (uint32_t i = 0; i < src.format.num_attribs; i++) {
				const spmdl_attrib &attrib = src.format.attribs[i];

				size_t stride_in_floats = src.data_format.vertex_size_in_floats;
				float *part_data = part.vertex_data.data() + attrib_offset_in_floats;
				switch (attrib.attrib) {

				case SP_VERTEX_ATTRIB_BONE_INDEX:
					{
						for (size_t vi = 0; vi < part.num_vertices; vi++) {
							for (size_t j = 0; j < part.data_format.weights_per_vertex; j++) {
								auto it = split.bone_refs.find((int32_t)part_data[j]);
								assert(it != split.bone_refs.end());
								part_data[j] = (float)it->value;
							}
							part_data += stride_in_floats;
						}
					}
					break;

				}

				attrib_offset_in_floats += (uint32_t)src.data_format.attrib_size_in_floats[i];
			}
		}

		parts.push_back(std::move(part));
	}

	return parts;
}

rh::array<mesh_part> process_mesh(ufbx_mesh *mesh, const mesh_data_format &fmt, const mesh_opts &opts)
{
	rh::array<mesh_part> parts;

	rh::array<ufbx_vec3> transformed_positions;
	rh::array<ufbx_vec3> transformed_normals;
	rh::array<bone_weight> weights;

	mesh_part_source src;

	src.positions = mesh->vertex_position.data;
	src.normals = mesh->vertex_normal.data;

	if (opts.transform_to_root) {
		transformed_positions.reserve(mesh->vertex_position.num_elements);
		transformed_normals.reserve(mesh->vertex_normal.num_elements);

		if (src.positions) {
			for (size_t i = 0; i < mesh->vertex_position.num_elements; i++) {
				ufbx_vec3 v = ufbx_transform_position(&mesh->node.to_root, mesh->vertex_position.data[i]);
				transformed_positions.push_back(v);
			}
			src.positions = transformed_positions.data();
		}

		if (src.normals) {
			ufbx_matrix normal_to_root = ufbx_get_normal_matrix(&mesh->node.to_root);
			for (size_t i = 0; i < mesh->vertex_normal.num_elements; i++) {
				ufbx_vec3 v = ufbx_transform_direction(&normal_to_root, mesh->vertex_normal.data[i]);
				double len = sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
				v.x /= len; v.y /= len; v.z /= len;
				transformed_normals.push_back(v);
			}
			src.normals = transformed_normals.data();
		}
	}

	if (fmt.weights_per_vertex > 0 && mesh->skins.size > 0) {
		weights.resize(fmt.weights_per_vertex * mesh->num_vertices);

		int32_t skin_index = 0;
		for (ufbx_skin &skin : mesh->skins) {
			size_t num_weights = skin.num_weights;

			if (!src.root_bone) {
				ufbx_node *root = skin.bone;
				while (root->parent && root->parent->parent) {
					root = root->parent;
				}
				src.root_bone = root;
			}

			mesh_bone bone;
			bone.node = skin.bone;
			bone.mesh_to_bind = skin.mesh_to_bind;
			src.bones.push_back(bone);

			for (size_t i = 0; i < num_weights; i++) {
				bone_weight *dst = weights.data() + skin.indices[i] * fmt.weights_per_vertex;
				bone_weight w { skin_index, (float)skin.weights[i] };
				for (uint32_t c = 0; c < fmt.weights_per_vertex; c++) {
					if (w.weight > dst[c].weight) {
						std::swap(w, dst[c]);
					}
				}
			}

			skin_index++;
		}

		// Normalize weights and pad with last bone
		for (size_t i = 0; i < mesh->num_vertices; i++) {
			bone_weight *w = weights.data() + i * fmt.weights_per_vertex;
			float total = 0.0f;
			for (uint32_t j = 0; j < fmt.weights_per_vertex; j++) {
				total += w[j].weight;
			}
			if (total > 0.0f) {
				int32_t last_index = 0;
				for (uint32_t j = 0; j < fmt.weights_per_vertex; j++) {
					if (w[j].weight > 0.0f) {
						last_index = w[j].index;
						w[j].weight /= total;
					} else {
						w[j].index = last_index;
					}
				}
			}
		}

		src.weights = weights.data();
	}

	size_t max_indices = mesh->num_triangles * 3;

	src.indices.reserve(max_indices);

	uint32_t num_materials = (uint32_t)mesh->materials.size;
	if (num_materials == 0) num_materials = 1;

	int32_t *face_material = mesh->face_material;
	for (uint32_t mat_ix = 0; mat_ix < num_materials; mat_ix++) {

		src.indices.clear();

		for (size_t fi = 0; fi < mesh->num_faces; fi++) {
			if (face_material && face_material[fi] != mat_ix) continue;
			ufbx_face face = mesh->faces[fi];
			for (size_t i = 1; i + 2 <= face.num_indices; i++) {
				src.indices.push_back((uint32_t)(face.index_begin + 0));
				src.indices.push_back((uint32_t)(face.index_begin + i + 0));
				src.indices.push_back((uint32_t)(face.index_begin + i + 1));
			}
		}

		mesh_part part = process_mesh_part(mesh, fmt, opts, src, mat_ix);
		parts.push_back(std::move(part));
	}

	return parts;
}

struct optimize_opts
{
	float overdraw_threshold = 1.05f;
};

void optimize_mesh_part(mesh_part &part, const optimize_opts &opts)
{
	if (part.data_format.position_offset_in_floats != ~0u) {
		meshopt_spatialSortTriangles(part.index_data.data(), part.index_data.data(),
			part.num_indices, part.vertex_data.data() + part.data_format.position_offset_in_floats,
			part.num_vertices, part.data_format.vertex_size_in_floats * sizeof(float));
	}

	meshopt_optimizeVertexCache(part.index_data.data(), part.index_data.data(),
		part.num_indices, part.num_vertices);

	if (part.data_format.position_offset_in_floats != ~0u) {
		meshopt_optimizeOverdraw(part.index_data.data(), part.index_data.data(),
			part.num_indices, part.vertex_data.data() + part.data_format.position_offset_in_floats,
			part.num_vertices, part.data_format.vertex_size_in_floats * sizeof(float), opts.overdraw_threshold);
	}

	meshopt_optimizeVertexFetch(part.vertex_data.data(), part.index_data.data(),
		part.num_indices, part.vertex_data.data(), part.num_vertices,
		part.data_format.vertex_size_in_floats * sizeof(float));
}

template <typename T>
T clamp_float(float f)
{
	constexpr double min = (double)std::numeric_limits<T>().lowest();
	constexpr double max = (double)std::numeric_limits<T>().max();
	if ((double)f < min) return (T)min;
	if ((double)f > max) return (T)max;
	return (T)f;
}

template <typename T>
T clamp_float(float f, double min, double max)
{
	if ((double)f < min) return (T)min;
	if ((double)f > max) return (T)max;
	return (T)f;
}

template <typename T>
void normalize_quantized_values(void *dst, size_t num_components, size_t num_elements, size_t element_stride_in_bytes)
{
	char *ptr = (char*)dst, *end = ptr + num_elements * element_stride_in_bytes;
	for (; ptr != end; ptr += element_stride_in_bytes) {
		int64_t total = 0;
		T *t = (T*)ptr;
		for (size_t i = 0; i < num_components; i++) {
			total += (int64_t)t[i];
		}
		if (total == 0) continue;

		int64_t delta = (int64_t)std::numeric_limits<T>().max() - total;
		if (delta == 0) continue;

		for (size_t i = 0; i < num_components; i++) {
			if (delta > 0) {
				if (t[i] > 0) {
					t[i] += (T)delta;
					break;
				}
			} else {
				if ((int64_t)t[i] > -delta) {
					t[i] -= (T)-delta;
					break;
				} else {
					delta += (int64_t)t[i];
					t[i] = 0;
				}
			}
		}
	}
}

struct encode_args
{
	sp_format format;
	void *dst;
	size_t dst_stride_in_bytes;
	const float *src;
	size_t src_stride_in_floats;
	size_t src_components;
	size_t num_elements;
};

size_t min(size_t a, size_t b) { return a < b ? a : b; }

template <typename T>
void encode_unorm(const encode_args &args, size_t dst_components)
{
	size_t components = min(args.src_components, dst_components);
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	T *dst = (T*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	while (src != src_end) {
		for (size_t i = 0; i < components; i++) {
			dst[i] = meshopt_quantizeUnorm(src[i], sizeof(T) * 8);
		}

		src += src_stride_in_floats;
		dst = (T*)((char*)dst + dst_stride_in_bytes);
	}
}

template <typename T>
void encode_snorm(const encode_args &args, size_t dst_components)
{
	size_t components = min(args.src_components, dst_components);
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	T *dst = (T*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	while (src != src_end) {
		for (size_t i = 0; i < components; i++) {
			dst[i] = (T)meshopt_quantizeSnorm(src[i], sizeof(T) * 8);
		}

		src += src_stride_in_floats;
		dst = (T*)((char*)dst + dst_stride_in_bytes);
	}
}

template <typename T>
void encode_uint(const encode_args &args, size_t dst_components)
{
	size_t components = min(args.src_components, dst_components);
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	T *dst = (T*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	while (src != src_end) {
		for (size_t i = 0; i < components; i++) {
			dst[i] = clamp_float<T>(src[i]);
		}

		src += src_stride_in_floats;
		dst = (T*)((char*)dst + dst_stride_in_bytes);
	}
}

template <typename T>
void encode_sint(const encode_args &args, size_t dst_components)
{
	size_t components = min(args.src_components, dst_components);
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	T *dst = (T*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	while (src != src_end) {
		for (size_t i = 0; i < components; i++) {
			dst[i] = clamp_float<T>(src[i]);
		}

		src += src_stride_in_floats;
		dst = (T*)((char*)dst + dst_stride_in_bytes);
	}
}

void encode_half(const encode_args &args, size_t dst_components)
{
	size_t components = min(args.src_components, dst_components);
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	uint16_t *dst = (uint16_t*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	while (src != src_end) {
		for (size_t i = 0; i < components; i++) {
			dst[i] = (uint16_t)meshopt_quantizeHalf(src[i]);
		}

		src += src_stride_in_floats;
		dst = (uint16_t*)((char*)dst + dst_stride_in_bytes);
	}
}

void encode_float(const encode_args &args, size_t dst_components)
{
	size_t components = min(args.src_components, dst_components);
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	float *dst = (float*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	while (src != src_end) {
		for (size_t i = 0; i < components; i++) {
			// TODO: Float quantization
			dst[i] = src[i];
		}

		src += src_stride_in_floats;
		dst = (float*)((char*)dst + dst_stride_in_bytes);
	}
}

void encode_unorm10(const encode_args &args)
{
	size_t components = args.src_components;
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	uint32_t *dst = (uint32_t*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	float x = 0.0f, y = 0.0f, z = 0.0f;
	while (src != src_end) {
		x = src[0];
		if (components >= 3) {
			y = src[1]; z = src[2];
		} else if (components >= 2) {
			y = src[1];
		}

		*dst
			= meshopt_quantizeUnorm(x, 10) << 20
			| meshopt_quantizeUnorm(y, 10) << 10
			| meshopt_quantizeUnorm(z, 10);

		src += src_stride_in_floats;
		dst = (uint32_t*)((char*)dst + dst_stride_in_bytes);
	}
}

void encode_uint10(const encode_args &args)
{
	size_t components = args.src_components;
	size_t src_stride_in_floats = args.src_stride_in_floats;
	size_t dst_stride_in_bytes = args.dst_stride_in_bytes;
	uint32_t *dst = (uint32_t*)args.dst;
	const float *src = args.src, *src_end = src + args.src_stride_in_floats*args.num_elements;
	float x = 0.0f, y = 0.0f, z = 0.0f;
	while (src != src_end) {
		x = src[0];
		if (components >= 3) {
			y = src[1]; z = src[2];
		} else if (components >= 2) {
			y = src[1];
		}

		*dst
			= clamp_float<uint32_t>(x, 0.0, 1024.0) << 20
			| clamp_float<uint32_t>(y, 0.0, 1024.0) << 10
			| clamp_float<uint32_t>(z, 0.0, 1024.0);

		src += src_stride_in_floats;
		dst = (uint32_t*)((char*)dst + dst_stride_in_bytes);
	}
}

void encode_format(const encode_args &args)
{
	switch (args.format) {
	case SP_FORMAT_R8_UNORM: encode_unorm<uint8_t>(args, 1); break;
	case SP_FORMAT_R8_SNORM: encode_snorm<int8_t>(args, 1); break;
	case SP_FORMAT_R8_UINT: encode_uint<uint8_t>(args, 1); break;
	case SP_FORMAT_R8_SINT: encode_sint<int8_t>(args, 1); break;
	case SP_FORMAT_RG8_UNORM: encode_unorm<uint8_t>(args, 2); break;
	case SP_FORMAT_RG8_SNORM: encode_snorm<int8_t>(args, 2); break;
	case SP_FORMAT_RG8_UINT: encode_uint<uint8_t>(args, 2); break;
	case SP_FORMAT_RG8_SINT: encode_sint<int8_t>(args, 2); break;
	case SP_FORMAT_RGB8_UNORM: encode_unorm<uint8_t>(args, 3); break;
	case SP_FORMAT_RGB8_SNORM: encode_snorm<int8_t>(args, 3); break;
	case SP_FORMAT_RGB8_UINT: encode_uint<uint8_t>(args, 3); break;
	case SP_FORMAT_RGB8_SINT: encode_sint<int8_t>(args, 3); break;
	case SP_FORMAT_RGBA8_UNORM: encode_unorm<uint8_t>(args, 4); break;
	case SP_FORMAT_RGBA8_SNORM: encode_snorm<int8_t>(args, 4); break;
	case SP_FORMAT_RGBA8_UINT: encode_uint<uint8_t>(args, 4); break;
	case SP_FORMAT_RGBA8_SINT: encode_sint<int8_t>(args, 4); break;
	case SP_FORMAT_R16_UNORM: encode_unorm<uint16_t>(args, 1); break;
	case SP_FORMAT_R16_SNORM: encode_snorm<int16_t>(args, 1); break;
	case SP_FORMAT_R16_UINT: encode_uint<uint16_t>(args, 1); break;
	case SP_FORMAT_R16_SINT: encode_sint<int16_t>(args, 1); break;
	case SP_FORMAT_R16_FLOAT: encode_half(args, 1); break;
	case SP_FORMAT_RG16_UNORM: encode_unorm<uint16_t>(args, 2); break;
	case SP_FORMAT_RG16_SNORM: encode_snorm<int16_t>(args, 2); break;
	case SP_FORMAT_RG16_UINT: encode_uint<uint16_t>(args, 2); break;
	case SP_FORMAT_RG16_SINT: encode_sint<int16_t>(args, 2); break;
	case SP_FORMAT_RG16_FLOAT: encode_half(args, 2); break;
	case SP_FORMAT_RGB16_UNORM: encode_unorm<uint16_t>(args, 3); break;
	case SP_FORMAT_RGB16_SNORM: encode_snorm<int16_t>(args, 3); break;
	case SP_FORMAT_RGB16_UINT: encode_uint<uint16_t>(args, 3); break;
	case SP_FORMAT_RGB16_SINT: encode_sint<int16_t>(args, 3); break;
	case SP_FORMAT_RGB16_FLOAT: encode_half(args, 3); break;
	case SP_FORMAT_RGBA16_UNORM: encode_unorm<uint16_t>(args, 4); break;
	case SP_FORMAT_RGBA16_SNORM: encode_snorm<int16_t>(args, 4); break;
	case SP_FORMAT_RGBA16_UINT: encode_uint<uint16_t>(args, 4); break;
	case SP_FORMAT_RGBA16_SINT: encode_sint<int16_t>(args, 4); break;
	case SP_FORMAT_RGBA16_FLOAT: encode_half(args, 4); break;
	case SP_FORMAT_R32_UNORM: encode_unorm<uint32_t>(args, 1); break;
	case SP_FORMAT_R32_SNORM: encode_snorm<int32_t>(args, 1); break;
	case SP_FORMAT_R32_UINT: encode_uint<uint32_t>(args, 1); break;
	case SP_FORMAT_R32_SINT: encode_sint<int32_t>(args, 1); break;
	case SP_FORMAT_R32_FLOAT: encode_float(args, 1); break;
	case SP_FORMAT_RG32_UNORM: encode_unorm<uint32_t>(args, 2); break;
	case SP_FORMAT_RG32_SNORM: encode_snorm<int32_t>(args, 2); break;
	case SP_FORMAT_RG32_UINT: encode_uint<uint32_t>(args, 2); break;
	case SP_FORMAT_RG32_SINT: encode_sint<int32_t>(args, 2); break;
	case SP_FORMAT_RG32_FLOAT: encode_float(args, 2); break;
	case SP_FORMAT_RGB32_UNORM: encode_unorm<uint32_t>(args, 3); break;
	case SP_FORMAT_RGB32_SNORM: encode_snorm<int32_t>(args, 3); break;
	case SP_FORMAT_RGB32_UINT: encode_uint<uint32_t>(args, 3); break;
	case SP_FORMAT_RGB32_SINT: encode_sint<int32_t>(args, 3); break;
	case SP_FORMAT_RGB32_FLOAT: encode_float(args, 3); break;
	case SP_FORMAT_RGBA32_UNORM: encode_unorm<uint32_t>(args, 4); break;
	case SP_FORMAT_RGBA32_SNORM: encode_snorm<int32_t>(args, 4); break;
	case SP_FORMAT_RGBA32_UINT: encode_uint<uint32_t>(args, 4); break;
	case SP_FORMAT_RGBA32_SINT: encode_sint<int32_t>(args, 4); break;
	case SP_FORMAT_RGBA32_FLOAT: encode_float(args, 4); break;
	case SP_FORMAT_RGB10A2_UNORM: encode_unorm10(args); break;
	case SP_FORMAT_RGB10A2_UINT: encode_uint10(args); break;
	default: break;
	}
}

void encode_vertex_stream(rh::array<char> &dst, const mesh_part &part, size_t stream_ix)
{
	encode_args args;
	args.dst_stride_in_bytes = part.format.stream_stride[stream_ix];
	args.src_stride_in_floats = part.data_format.vertex_size_in_floats;
	args.num_elements = part.num_vertices;
	dst.resize_uninit(part.num_vertices * args.dst_stride_in_bytes);
	memset(dst.data(), 0, dst.size());

	const float *src = part.vertex_data.data();
	for (size_t attrib_ix = 0; attrib_ix < part.format.num_attribs; attrib_ix++) {
		const spmdl_attrib &attrib = part.format.attribs[attrib_ix];
		uint32_t src_components = part.data_format.attrib_size_in_floats[attrib_ix];

		if (attrib.stream == stream_ix) {
			args.format = attrib.format;
			args.src = src;
			args.dst = dst.data() + attrib.offset;
			args.src_components = src_components;
			encode_format(args);

			if (attrib.attrib == SP_VERTEX_ATTRIB_BONE_WEIGHT) {
				switch (attrib.format) {
				case SP_FORMAT_R8_UNORM: normalize_quantized_values<uint8_t>(args.dst, 1, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RG8_UNORM: normalize_quantized_values<uint8_t>(args.dst, 2, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RGB8_UNORM: normalize_quantized_values<uint8_t>(args.dst, 3, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RGBA8_UNORM: normalize_quantized_values<uint8_t>(args.dst, 4, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_R16_UNORM: normalize_quantized_values<uint16_t>(args.dst, 1, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RG16_UNORM: normalize_quantized_values<uint16_t>(args.dst, 2, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RGB16_UNORM: normalize_quantized_values<uint16_t>(args.dst, 3, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RGBA16_UNORM: normalize_quantized_values<uint16_t>(args.dst, 4, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_R32_UNORM: normalize_quantized_values<uint32_t>(args.dst, 1, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RG32_UNORM: normalize_quantized_values<uint32_t>(args.dst, 2, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RGB32_UNORM: normalize_quantized_values<uint32_t>(args.dst, 3, args.num_elements, args.dst_stride_in_bytes); break;
				case SP_FORMAT_RGBA32_UNORM: normalize_quantized_values<uint32_t>(args.dst, 4, args.num_elements, args.dst_stride_in_bytes); break;
				}
			}
		}

		src += src_components;
	}
}

attrib_bounds get_attrib_bounds(const mesh_part &part, size_t attrib_ix)
{
	const float *src = part.vertex_data.data();
	for (size_t ref_ix = 0; ref_ix < part.format.num_attribs; ref_ix++) {
		const spmdl_attrib &attrib = part.format.attribs[attrib_ix];
		uint32_t src_components = part.data_format.attrib_size_in_floats[attrib_ix];
		uint32_t stride = part.data_format.vertex_size_in_floats;

		if (ref_ix == attrib_ix) {
			attrib_bounds bounds;
			for (size_t i = 0; i < src_components; i++) {
				bounds.min[i] = +HUGE_VALF;
				bounds.max[i] = -HUGE_VALF;
			}
			for (size_t i = src_components; i < 4; i++) {
				bounds.min[i] = 0.0f;
				bounds.max[i] = 0.0f;
			}
			bounds.num_components = src_components;

			for (size_t vi = 0; vi < part.num_vertices; vi++) {
				for (size_t i = 0; i < src_components; i++) {
					float f = src[i];
					if (f < bounds.min[i]) bounds.min[i] = f;
					if (f > bounds.max[i]) bounds.max[i] = f;
				}

				src += part.data_format.vertex_size_in_floats;
			}

			return bounds;
		}

		src += src_components;
	}

	return { };
}

struct model_node
{
	ufbx_node *node;
	uint32_t parent_ix;
};

struct model
{
	rh::array<model_node> nodes;
	rh::hash_map<ufbx_node*, uint32_t> node_mapping;
};

uint32_t add_node(model &model, ufbx_node *node)
{
	if (!node) return ~0u;
	auto it = model.node_mapping.find(node);
	if (it) return it->value;
	uint32_t parent_ix = add_node(model, node->parent);
	model_node dst = { node, parent_ix };
	uint32_t ix = (uint32_t)model.nodes.size();
	model.node_mapping.emplace(node, ix);
	model.nodes.push_back(std::move(dst));
	return ix;
}

uint32_t find_node(const model &model, ufbx_node *node)
{
	auto it = model.node_mapping.find(node);
	assert(it);
	return it->value;
}

struct gltf_buffer_view
{
	uint32_t offset;
	uint32_t size;
	uint32_t stride;
	uint32_t target;
};

struct gltf_accessor
{
	uint32_t buffer_view;
	uint32_t component_type;
	uint32_t offset = 0;
	uint32_t count = 0;
	bool normalized = false;
	bool has_bounds = false;
	const char *type;
	attrib_bounds bounds;
};

struct gltf_attribute
{
	const char *name = nullptr;
	uint32_t accessor;
};

struct gltf_primitive
{
	rh::array<gltf_attribute> attributes;
	uint32_t index_accessor = ~0u;
};

struct gltf_mesh
{
	rh::array<gltf_primitive> primitives;
};

struct gltf_skin
{
	uint32_t ibm_accessor = ~0u;
	uint32_t skeleton = ~0u;
	rh::array<uint32_t> joints;
};

struct gltf_mesh_ref
{
	uint32_t mesh = ~0u;
	uint32_t skin = ~0u;
};

struct gltf_node
{
	ufbx_node *node;
	const char *name;
	rh::array<uint32_t> children;
	gltf_mesh_ref mesh_ref;
};

struct gltf_file
{
	rh::array<char> binary_data;
	rh::array<gltf_buffer_view> buffer_views;
	rh::array<gltf_accessor> accessors;
	rh::array<gltf_mesh> meshes;
	rh::array<gltf_skin> skins;
	rh::array<gltf_node> nodes;
};

uint32_t gltf_push_buffer(gltf_file &gfile, rh::slice<const char> data, size_t stride, uint32_t target)
{
	gltf_buffer_view gview;
	gview.offset = (uint32_t)gfile.binary_data.size();
	gview.size = (uint32_t)data.size;
	gview.stride = (uint32_t)stride;
	gview.target = target;
	gfile.binary_data.insert_back(data);
	gfile.buffer_views.push_back(std::move(gview));

	// Pad to 4 bytes
	while (gfile.binary_data.size() % 4 != 0) {
		gfile.binary_data.push_back('\0');
	}

	return (uint32_t)gfile.buffer_views.size() - 1;
}

uint32_t gltf_push_accessor(gltf_file &gfile, gltf_accessor gacc)
{
	gfile.accessors.push_back(std::move(gacc));
	return (uint32_t)gfile.accessors.size() - 1;
}

gltf_mesh_ref gltf_push_mesh(gltf_file &gfile, gltf_mesh gmesh, gltf_skin gskin)
{
	gltf_mesh_ref ref;
	gfile.meshes.push_back(std::move(gmesh));
	ref.mesh = (uint32_t)gfile.meshes.size() - 1;
	if (gskin.joints.size() > 0) {
		gfile.skins.push_back(std::move(gskin));
		ref.skin = (uint32_t)gfile.skins.size() - 1;
	} else {
		ref.skin = ~0u;
	}
	return ref;
}

struct HackVertex
{
	spmdl_vec3 position;
	spmdl_vec3 normal;
	uint8_t bone_ix[4];
	uint8_t bone_weight[4];
};

gltf_file convert_to_gltf(const model &mod, rh::slice<mesh_part> parts)
{
	gltf_file gfile;
	gfile.nodes.reserve(mod.nodes.size());

	uint32_t ix = 0;
	for (const model_node &node : mod.nodes) {
		gltf_node gnode;
		gnode.node = node.node;
		gnode.name = node.node->name.data;
		gfile.nodes.push_back(std::move(gnode));

		if (node.parent_ix != ~0u) {
			gfile.nodes[node.parent_ix].children.push_back(ix);
		}
		ix++;
	}

	rh::array<char> buffer;

	for (const mesh_part &part : parts) {
		auto it = mod.node_mapping.find(&part.mesh->node);
		assert(it);

		gltf_node &gnode = gfile.nodes[it->value];

		gltf_mesh gmesh;
		gltf_skin gskin;

		gltf_primitive gprim;

		{
			rh::array<uint16_t> indices16;
			rh::slice<const char> index_data;
			uint32_t component_type;

			if (part.num_vertices < UINT16_MAX) {
				indices16.reserve(part.index_data.size());
				for (uint32_t ix : part.index_data) {
					indices16.push_back((uint16_t)ix);
				}
				index_data.data = (const char*)indices16.data();
				index_data.size = indices16.size() * sizeof(uint16_t);
				component_type = 5123; // UNSIGNED_SHORT
			} else {
				index_data.data = (const char*)part.index_data.data();
				index_data.size = part.index_data.size() * sizeof(uint32_t);
				component_type = 5125; // UNSIGNED_BYTE
			}

			uint32_t buffer_view = gltf_push_buffer(gfile, index_data, 0, 34963); // ELEMENT_ARRAY_BUFFER

			gltf_accessor gacc;
			gacc.buffer_view = buffer_view;
			gacc.count = (uint32_t)part.num_indices;
			gacc.component_type = component_type;
			gacc.type = "SCALAR";
			gprim.index_accessor = gltf_push_accessor(gfile, std::move(gacc));
		}

		for (uint32_t si = 0; si < part.format.num_streams; si++) {
			encode_vertex_stream(buffer, part, si);

			HackVertex *verts = (HackVertex*)buffer.data();

			uint32_t buffer_view = gltf_push_buffer(gfile, buffer.slice(), part.format.stream_stride[si], 34962); // ARRAY_BUFFER

			for (uint32_t ai = 0; ai < part.format.num_attribs; ai++) {
				const spmdl_attrib &attrib = part.format.attribs[ai];
				if (attrib.stream != si) continue;

				gltf_accessor gacc;
				gacc.buffer_view = buffer_view;
				gacc.count = (uint32_t)part.num_vertices;
				gacc.offset = attrib.offset;

				if (attrib.attrib == SP_VERTEX_ATTRIB_POSITION) {
					gacc.has_bounds = true;
					gacc.bounds = get_attrib_bounds(part, ai);
				}

				const sp_format_info &info = sp_format_infos[attrib.format];
				assert(info.flags & SP_FORMAT_FLAG_BASIC);

				uint32_t component_size = info.block_size / info.num_components;
				if (info.flags & SP_FORMAT_FLAG_FLOAT) {
					assert(component_size == 4);
					gacc.component_type = 5126; // FLOAT
				} else {
					uint32_t unsigned_prefix = (info.flags & SP_FORMAT_FLAG_SIGNED) ? 0 : 1;
					switch (component_size) {
					case 1: gacc.component_type = 5120 + unsigned_prefix; break; // (UNSIGNED_)BYTE
					case 2: gacc.component_type = 5122 + unsigned_prefix; break; // (UNSIGNED_)SHORT
					case 4: gacc.component_type = 5124 + unsigned_prefix; break; // (UNSIGNED_)INT
					}

					if (info.flags & SP_FORMAT_FLAG_NORMALIZED) {
						gacc.normalized = true;
					}
				}

				switch (info.num_components) {
				case 1: gacc.type = "SCALAR"; break;
				case 2: gacc.type = "VEC2"; break;
				case 3: gacc.type = "VEC3"; break;
				case 4: gacc.type = "VEC4"; break;
				}

				gltf_attribute gattrib;
				gattrib.accessor = gltf_push_accessor(gfile, std::move(gacc));

				switch (attrib.attrib) {
				case SP_VERTEX_ATTRIB_POSITION: gattrib.name = "POSITION"; break;
				case SP_VERTEX_ATTRIB_NORMAL: gattrib.name = "NORMAL"; break;
				case SP_VERTEX_ATTRIB_TANGENT: gattrib.name = "TANGENT"; break;
				case SP_VERTEX_ATTRIB_UV: gattrib.name = "TEXCOORD_0"; break;
				case SP_VERTEX_ATTRIB_COLOR: gattrib.name = "COLOR_0"; break;
				case SP_VERTEX_ATTRIB_BONE_INDEX: gattrib.name = "JOINTS_0"; break;
				case SP_VERTEX_ATTRIB_BONE_WEIGHT: gattrib.name = "WEIGHTS_0"; break;
				default: /* nop */ break;
				}

				if (gattrib.name) {
					gprim.attributes.push_back(std::move(gattrib));
				}
			}
		}

		if (part.bones.size() > 0) {
			rh::hash_set<ufbx_node*> included_nodes;
			rh::array<float> ibm;
			gskin.joints.reserve(part.bones.size());
			ibm.reserve(part.bones.size() * 16);
			for (const mesh_bone &bone : part.bones) {
				auto it = mod.node_mapping.find(bone.node);
				assert(it);
				gskin.joints.push_back(it->value);

				included_nodes.insert(bone.node);

				float mat[16];
				for (uint32_t col = 0; col < 4; col++) {
					mat[col*4 + 0] = (float)bone.mesh_to_bind.cols[col].x;
					mat[col*4 + 1] = (float)bone.mesh_to_bind.cols[col].y;
					mat[col*4 + 2] = (float)bone.mesh_to_bind.cols[col].z;
					mat[col*4 + 3] = col == 3 ? 1.0f : 0.0f;
				}
				ibm.insert_back(mat, 16);
			}

			float identity_matrix[] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

			for (const mesh_bone &bone : part.bones) {
				ufbx_node *parent = bone.node->parent;
				while (parent) {
					if (included_nodes.insert(parent).inserted) {
						auto it = mod.node_mapping.find(parent);
						assert(it);
						gskin.joints.push_back(it->value);
						ibm.insert_back(identity_matrix, 16);
					}
					if (parent == part.root_bone) break;
					parent = parent->parent;
				}
			}

			{
				auto it = mod.node_mapping.find(part.root_bone);
				assert(it);
				gskin.skeleton = it->value;
			}

			rh::slice<const char> ibm_data;
			ibm_data.data = (char*)ibm.data();
			ibm_data.size = ibm.size() * sizeof(float);
			uint32_t buffer_view = gltf_push_buffer(gfile, ibm_data, 0, 0); // ARRAY_BUFFER

			gltf_accessor gacc;
			gacc.buffer_view = buffer_view;
			gacc.count = (uint32_t)gskin.joints.size();
			gacc.component_type = 5126;
			gacc.type = "MAT4";
			gskin.ibm_accessor = gltf_push_accessor(gfile, gacc);
		}

		gmesh.primitives.push_back(std::move(gprim));

		if (gnode.mesh_ref.mesh == ~0u) {
			gnode.mesh_ref = gltf_push_mesh(gfile, std::move(gmesh), std::move(gskin));
		} else {
			auto it = mod.node_mapping.find(gnode.node);
			assert(it);

			uint32_t parent_ix = mod.nodes[it->value].parent_ix;
			if (parent_ix != ~0u) {
				gfile.nodes[parent_ix].children.push_back((uint32_t)gfile.nodes.size());
			}

			gltf_node gnode_copy;
			gnode_copy.node = gnode.node;
			gnode_copy.name = nullptr;
			gnode_copy.mesh_ref = gltf_push_mesh(gfile, std::move(gmesh), std::move(gskin));
			gfile.nodes.push_back(std::move(gnode_copy));
		}
	}

	return gfile;
}

static void write_data(FILE *f, const void *data, size_t size)
{
	size_t num = fwrite(data, 1, size, f);
	if (num != size) {
		fclose(f);
		failf("Failed to write output data");
	}
}

template <typename T>
static void write_pod(FILE *f, T &t)
{
	write_data(f, &t, sizeof(T));
}

template <typename T>
static void write_pod(FILE *f, rh::slice<T> slice)
{
	write_data(f, slice.data, sizeof(T) * slice.size);
}

struct glb_header
{
	uint32_t magic;
	uint32_t version;
	uint32_t length;
};

struct glb_chunk_header
{
	uint32_t chunk_length;
	uint32_t chunk_type;
};

void write_gtlf_node(jso_stream *s, const gltf_node &node)
{
	jso_object(s);

	if (node.name) {
		jso_prop_string(s, "name", node.name);
	}
	if (node.children.size() > 0) {
		jso_prop_array(s, "children");
		for (uint32_t ix : node.children) {
			jso_uint(s, ix);
		}
		jso_end_array(s);
	}

	jso_prop_array(s, "translation");
	jso_double(s, node.node->transform.translation.x);
	jso_double(s, node.node->transform.translation.y);
	jso_double(s, node.node->transform.translation.z);
	jso_end_array(s);

	jso_prop_array(s, "scale");
	jso_double(s, node.node->transform.scale.x);
	jso_double(s, node.node->transform.scale.y);
	jso_double(s, node.node->transform.scale.z);
	jso_end_array(s);

	jso_prop_array(s, "rotation");
	jso_double(s, node.node->transform.rotation.x);
	jso_double(s, node.node->transform.rotation.y);
	jso_double(s, node.node->transform.rotation.z);
	jso_double(s, node.node->transform.rotation.w);
	jso_end_array(s);

	gltf_mesh_ref ref = node.mesh_ref;
	if (ref.mesh != ~0u) {
		jso_prop_uint(s, "mesh", ref.mesh);
		if (ref.skin != ~0u) {
			jso_prop_uint(s, "skin", ref.skin);
		}
	}

	jso_end_object(s);
}

void write_gltf(FILE *f, const gltf_file &gfile)
{
	jso_stream jso, *s = &jso;
	jso_init_growable(s);
	s->pretty = true;

	jso_object(s);

	jso_prop_int(s, "scene", 0);

	jso_prop_object(s, "asset");
	jso_prop_string(s, "version", "2.0");
	jso_end_object(s);

	jso_prop_array(s, "scenes");
	jso_object(s);
	jso_prop_string(s, "name", "singleScene");
	jso_prop_array(s, "nodes");
	jso_int(s, 0);
	jso_end_array(s);
	jso_end_object(s);
	jso_end_array(s);

	jso_prop_array(s, "nodes");
	for (const gltf_node &node : gfile.nodes) {
		write_gtlf_node(s, node);
	}
	jso_end_array(s);

	jso_prop_array(s, "meshes");
	uint32_t mesh_ix = 0;
	for (const gltf_mesh &mesh : gfile.meshes) {
		jso_object(s);

		char name[512];
		snprintf(name, sizeof(name), "spmesh_%u", ++mesh_ix);
		jso_prop_string(s, "name", name);

		jso_prop_array(s, "primitives");
		for (const gltf_primitive &prim : mesh.primitives) {
			jso_object(s);
			jso_prop_object(s, "attributes");
			for (const gltf_attribute &attrib : prim.attributes) {
				jso_prop_uint(s, attrib.name, attrib.accessor);
			}
			jso_end_object(s);
			jso_prop_uint(s, "indices", prim.index_accessor);
			jso_prop_uint(s, "mode", 4);
			jso_end_object(s);
		}
		jso_end_array(s);

		jso_end_object(s);
	}
	jso_end_array(s);

	jso_prop_array(s, "skins");
	uint32_t skin_ix = 0;
	for (const gltf_skin &skin : gfile.skins) {

		jso_object(s);

		char name[512];
		snprintf(name, sizeof(name), "spskin_%u", ++skin_ix);
		jso_prop_string(s, "name", name);

		jso_prop_uint(s, "inverseBindMatrices", skin.ibm_accessor);

		jso_prop_uint(s, "skeleton", skin.skeleton);

		jso_prop_array(s, "joints");
		for (uint32_t ix : skin.joints) {
			jso_uint(s, ix);
		}
		jso_end_array(s);

		jso_end_object(s);
	}
	jso_end_array(s);

	jso_prop_array(s, "accessors");
	for (const gltf_accessor &acc : gfile.accessors) {
		jso_object(s);
		jso_prop_uint(s, "bufferView", acc.buffer_view);
		jso_prop_uint(s, "byteOffset", acc.offset);
		jso_prop_uint(s, "componentType", acc.component_type);
		jso_prop_uint(s, "count", acc.count);
		jso_prop_string(s, "type", acc.type);
		if (acc.normalized) {
			jso_prop_boolean(s, "normalized", true);
		}

		if (acc.has_bounds) {
			jso_prop_array(s, "min");
			for (uint32_t i = 0; i < acc.bounds.num_components; i++) {
				jso_float(s, acc.bounds.min[i]);
			}
			jso_end_array(s);

			jso_prop_array(s, "max");
			for (uint32_t i = 0; i < acc.bounds.num_components; i++) {
				jso_float(s, acc.bounds.max[i]);
			}
			jso_end_array(s);
		}

		jso_end_object(s);
	}
	jso_end_array(s);

	jso_prop_array(s, "bufferViews");
	for (const gltf_buffer_view &view : gfile.buffer_views) {
		jso_object(s);
		jso_prop_uint(s, "buffer", 0);
		jso_prop_uint(s, "byteOffset", view.offset);
		jso_prop_uint(s, "byteLength", view.size);
		if (view.stride) {
			jso_prop_uint(s, "byteStride", view.stride);
		}
		if (view.target) {
			jso_prop_uint(s, "target", view.target);
		}
		jso_end_object(s);
	}
	jso_end_array(s);

	jso_prop_array(s, "buffers");
	jso_object(s);
	jso_prop_uint(s, "byteLength", (uint32_t)gfile.binary_data.size());
	jso_end_object(s);
	jso_end_array(s);

	jso_end_object(s);

	void *json_data = s->data;
	size_t json_size = s->pos;
	size_t json_pad = (size_t)-(int32_t)json_size & 3;

	size_t total_size = sizeof(glb_header) + 2 * sizeof(glb_chunk_header) + json_size + json_pad + gfile.binary_data.size();

	{
		glb_header header;
		header.magic = 0x46546C67; // glTF
		header.version = 2;
		header.length = (uint32_t)total_size;
		write_data(f, &header, sizeof(header));
	}

	{
		glb_chunk_header chunk;
		chunk.chunk_length = (uint32_t)(json_size + json_pad);
		chunk.chunk_type = 0x4E4F534A; // JSON
		write_data(f, &chunk, sizeof(chunk));
	}

	write_data(f, json_data, json_size);
	write_data(f, "    ", json_pad);

	{
		glb_chunk_header chunk;
		chunk.chunk_length = (uint32_t)gfile.binary_data.size();
		chunk.chunk_type = 0x004E4942; // BIN\0
		write_data(f, &chunk, sizeof(chunk));
	}

	write_data(f, gfile.binary_data.data(), gfile.binary_data.size());

	jso_close(s);
}

struct acl_zeroing_allocator final : acl::IAllocator
{
	acl_zeroing_allocator(acl::IAllocator &inner) : inner(inner) { }

	acl::IAllocator &inner;

	virtual void* allocate(size_t size, size_t alignment = k_default_alignment) override final
	{
		void *ptr = inner.allocate(size, alignment);
		if (ptr) memset(ptr, 0, size + ((size_t)-(ptrdiff_t)size & (alignment - 1)));
		return ptr;
	}

	virtual void deallocate(void* ptr, size_t size) override final
	{
		inner.deallocate(ptr, size);
	}
};

// ACL streams seem to have some garbage in trailing data (!?!?!)
// Zero buffers to make results reproducible
acl::ANSIAllocator acl_ansi_ator;
acl_zeroing_allocator acl_ator { acl_ansi_ator };

struct animation_opts
{
	acl::compression_level8 level = acl::compression_level8::medium;
};

rh::array<char> compress_animation(model &mod, ufbx_scene *scene, const animation_opts &opts)
{
	rh::array<char> result;

	rh::array<acl::RigidBone> bones;
	bones.reserve(mod.nodes.size());

	for (model_node &node : mod.nodes) {
		ufbx_node *n = node.node;
		acl::RigidBone bone;
		bone.name = acl::String(acl_ator, n->name.data, n->name.length);
		if (node.parent_ix != ~0u) {
			bone.parent_index = (uint16_t)node.parent_ix;
		}
		bone.vertex_distance = 3.0f;
		// bone.bind_transform.translation = rtm::vector_load3(n->transform.translation.v);
		// bone.bind_transform.rotation = rtm::quat_load(n->transform.rotation.v);
		bones.push_back(std::move(bone));
	}

	acl::RigidSkeleton skeleton(acl_ator, bones.data(), (uint16_t)bones.size());

	if (scene->anim_stacks.size == 0) {
		failf("No aniamtion stacks found");
	}
	ufbx_anim_stack *stack = &scene->anim_stacks.data[0];

	double sample_rate = 30.0;
	double anim_duration = stack->time_end - stack->time_begin;
	if (anim_duration < 1.0/sample_rate) anim_duration  = 1.0/sample_rate;

	uint32_t num_samples = (uint32_t)(anim_duration * sample_rate + 0.9999);
	
	acl::AnimationClip clip(acl_ator, skeleton, num_samples, (float)sample_rate, acl::String());

	for (uint32_t bi = 0; bi < mod.nodes.size(); bi++) {
		acl::AnimatedBone &bone = clip.get_animated_bone((uint16_t)bi);
		ufbx_node *node = mod.nodes[bi].node;

		for (uint32_t si = 0; si < num_samples; si++) {
			double time = (double)si / sample_rate + stack->time_begin;
			ufbx_transform t = ufbx_evaluate_transform(scene, node, stack, time);
			bone.translation_track.set_sample(si, rtm::vector_load3(t.translation.v));
			bone.rotation_track.set_sample(si, rtm::quat_load(t.rotation.v));
			bone.scale_track.set_sample(si, rtm::vector_load3(t.scale.v));
		}
	}

	acl::CompressionSettings settings = acl::get_default_compression_settings();
	settings.level = opts.level;

	acl::qvvf_transform_error_metric error_metric;
	settings.error_metric = &error_metric;

	acl::OutputStats stats;
	acl::CompressedClip *compressed_clip = nullptr;

	acl::ErrorResult err = acl::uniformly_sampled::compress_clip(acl_ator, clip, settings, compressed_clip, stats);
	if (err.any()) failf("Failed to compress animation: %s", err.c_str());

	result.insert_back((const char*)compressed_clip, compressed_clip->get_size());

	acl_ator.deallocate(compressed_clip, compressed_clip->get_size());

	return result;
}

struct string_pool
{
	rhmap map;
	rh::array<char> data;

	string_pool()
	{
		rhmap_init_inline(&map);
	}

	~string_pool()
	{
		free(rhmap_reset_inline(&map));
	}

	string_pool(string_pool &&rhs) : map(rhs.map), data(std::move(rhs.data)) {
		rhmap_init_inline(&rhs.map);
	}
	string_pool& operator=(string_pool &&rhs) {
		std::swap(data, rhs.data);
		std::swap(map, rhs.map);
		return *this;
	}

	string_pool(const string_pool &rhs) = delete;
	string_pool& operator=(const string_pool &rhs) = delete;

	spfile_string insert(const char *str, size_t size)
	{
		if (map.size == map.capacity) {
			size_t count, alloc_size;
			rhmap_grow_inline(&map, &count, &alloc_size, 16, 0);
			free(rhmap_rehash_inline(&map, count, alloc_size, malloc(alloc_size)));
		}

		uint32_t hash = rh::hash_buffer(str, size), scan, offset;
		while (rhmap_find_inline(&map, hash, &scan, &offset)) {
			if (!strcmp(data.data() + offset, str)) {
				return { offset, (uint32_t)size };
			}
		}

		offset = (uint32_t)data.size();
		rhmap_insert_inline(&map, hash, scan, offset);
		data.insert_back(str, size);
		data.push_back('\0');
		return { offset, (uint32_t)size };
	}

	spfile_string insert(const char *str) { return insert(str, strlen(str)); }
	spfile_string insert(ufbx_string str) { return insert(str.data, str.length); }
};

spmdl_vec3 to_spmdl(ufbx_vec3 v) { return { (float)v.x, (float)v.y, (float)v.z }; }
spmdl_vec4 to_spmdl(ufbx_vec4 v) { return { (float)v.x, (float)v.y, (float)v.z, (float)v.w }; }
spmdl_matrix to_spmdl(const ufbx_matrix &v) {
	return {
		to_spmdl(v.cols[0]), to_spmdl(v.cols[1]), to_spmdl(v.cols[2]), to_spmdl(v.cols[3]),
	};
}

struct compress_opts
{
	sp_compression_type type = SP_COMPRESSION_ZSTD;
	int level = 10;
	double uncompressed_threshold = 0.95;
};

struct compress_result
{
	rh::array<char> data;
	sp_compression_type type;
};

compress_result compress(rh::slice<const char> data, const compress_opts &opts, uint32_t pre_padding=0)
{
	compress_result result;
	result.data.resize_uninit(sp_get_compression_bound(opts.type, data.size) + pre_padding);
	result.type = opts.type;

	size_t compressed_size = sp_compress_buffer(opts.type, result.data.data() + pre_padding, result.data.size() - pre_padding, data.data, data.size, opts.level);
	if (compressed_size >= (size_t)(data.size * opts.uncompressed_threshold)) {
		if (opts.type != SP_COMPRESSION_NONE) {
			result.data.resize_uninit(pre_padding + data.size);
			memcpy(result.data.data() + pre_padding, data.data, data.size);
			result.type = SP_COMPRESSION_NONE;
		}
	} else {
		result.data.resize_uninit(pre_padding + compressed_size);
	}

	if (pre_padding > 0) {
		memset(result.data.data(), 0, pre_padding);
	}

	return result;
}

template <typename T>
void init_section(uint32_t &offset, spfile_section &section, compress_result &res, rh::slice<T> data, const compress_opts &opts, spfile_section_magic magic, uint32_t index=0)
{
	uint32_t pre_padding = 0;
	while (offset % 16 != 0) {
		offset++;
		pre_padding++;
	}
	res = compress(rh::slice<const char>((char*)data.data, data.size * sizeof(T)), opts, pre_padding);
	section.magic = magic;
	section.compression_type = res.type;
	section.index = index;
	section.offset = offset;
	section.uncompressed_size = (uint32_t)data.size * sizeof(T);
	section.compressed_size = (uint32_t)res.data.size() - pre_padding;
	offset += section.compressed_size;
}

template <typename T>
spmdl_buffer sp_push_buffer(rh::array<char> &geometry, rh::slice<T> data, uint32_t stride=sizeof(T))
{
	spmdl_buffer buf;
	buf.offset = (uint32_t)geometry.size();
	buf.encoded_size = (uint32_t)data.size * sizeof(T);
	buf.stride = stride;
	geometry.insert_back((char*)data.data, data.size * sizeof(T));
	return buf;
}

int main(int argc, char **argv)
{
	const char *input_file = NULL;
	const char *output_file = NULL;
	bool show_help = false;
	int level = 10;
	int num_threads = 1;
	vertex_format mesh_format = { };
	bool combine_meshes = false;
	bool combine_everything = false;
	bool transform_to_root = false;
	bool do_mesh = false;
	bool do_anim = false;
	const char *format_spec = "";

	// -- Parse arguments

	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		int left = argc - argi - 1;

		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			g_verbose = true;
		} else if (!strcmp(arg, "--help")) {
			show_help = true;
		} else if (!strcmp(arg, "--combine-materials")) {
			combine_meshes = true;
		} else if (!strcmp(arg, "--combine-everything")) {
			combine_meshes = true;
			combine_everything = true;
		} else if (!strcmp(arg, "--transform-to-root")) {
			transform_to_root = true;
		} else if (!strcmp(arg, "--mesh")) {
			do_mesh = true;
		} else if (!strcmp(arg, "--anim")) {
			do_anim = true;
		} else if (left >= 1) {
			if (!strcmp(arg, "-i") || !strcmp(arg, "--input")) {
				input_file = argv[++argi];
			} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
				output_file = argv[++argi];
			} else if (!strcmp(arg, "-l") || !strcmp(arg, "--level")) {
				level = atoi(argv[++argi]);
				if (level <= 0 || level > 20) {
					failf("Invalid level %d, must be between 1-20", level);
				}
			} else if (!strcmp(arg, "-j") || !strcmp(arg, "--threads")) {
				num_threads = atoi(argv[++argi]);
				if (num_threads <= 0 || num_threads > 10000) failf("Bad number of threads: %d");
			} else if (!strcmp(arg, "--vertex")) {
				format_spec = argv[++argi];
			}
		}
	}

	if (show_help) {
		printf("%s",
			"Usage: sf-model -i <input> -o <output> --mesh p3f:n3f:u2f [options]\n"
			"    -i / --input <path>: Input filename in any format stb_image supports\n"
			"    -o / --output <path>: Destination filename\n"
			"    -j / --threads <num>: Number of threads to use\n"
			"    -v / --verbose: Verbose output\n"
		);

		return 0;
	}

	// -- Validate arguments

	if (!input_file) failf("Input file required: -i <input>");
	if (!output_file) failf("Output file required: -o <output>");
	mesh_format = parse_attribs(format_spec);

	// -- Load input FBX

	ufbx_load_opts ufbx_opts = { };

	// if (!do_mesh) ufbx_opts.ignore_geometry = true;
	// if (!do_anim) ufbx_opts.ignore_animation = true;

	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(input_file, &ufbx_opts, &error);
	if (!scene) {
		fprintf(stderr, "Failed to load FBX file: %s\n", input_file);
		for (size_t i = 0; i < error.stack_size; i++) {
			ufbx_error_frame f = error.stack[i];
			fprintf(stderr, "> %s (%s line %u)\n", f.description, f.function, f.source_line);
		}
		exit(1);
	}

	mesh_opts mesh_opts;
	mesh_opts.transform_to_root = transform_to_root;

	optimize_opts optimize_opts;
	animation_opts anim_opts;
	if (level <= 3) anim_opts.level = acl::compression_level8::lowest;
	else if (level <= 7) anim_opts.level = acl::compression_level8::low;
	else if (level <= 14) anim_opts.level = acl::compression_level8::medium;
	else if (level <= 18) anim_opts.level = acl::compression_level8::high;
	else anim_opts.level = acl::compression_level8::highest;

	compress_opts compress_opts;
	compress_opts.level = level;

	mesh_opts.format = mesh_format;
	mesh_data_format fmt = create_mesh_data_format(mesh_opts.format);

	mesh_limits limits;
	limits.max_bones = 64;
	limits.max_vertices = 0xffff;

	if (g_verbose) {
		printf("Vertex format: %s\n", format_spec);
		for (uint32_t si = 0; si < mesh_format.num_streams; si++) {
			printf("  Stream %u, stride %u bytes:\n", si, mesh_format.stream_stride[si]);
			for (uint32_t ai = 0; ai < mesh_format.num_attribs; ai++) {
				spmdl_attrib &attrib = mesh_format.attribs[ai];
				if (attrib.stream != si) continue;

				attrib_info attrib_info = attrib_list[attrib.attrib];
				sp_format_info fmt_info = sp_format_infos[attrib.format];
				printf("   %3d (0x%02x)  %-12s %-8s   (%2u bytes)\n", attrib.offset, attrib.offset, attrib_info.name, fmt_info.short_name, fmt_info.block_size);
			}
		}
	}

	model model;

	rh::array<mesh_part> parts;

	for (ufbx_mesh &mesh : scene->meshes) {
		parts.insert_back(process_mesh(&mesh, fmt, mesh_opts));
	}

	// Retain nodes
	for (mesh_part &part : parts) {
		add_node(model, &part.mesh->node);
		for (mesh_bone &bone : part.bones) {
			add_node(model, bone.node);
		}
	}

	string_pool str_pool;

	if (do_mesh) {

		if (combine_meshes) {
			rh::hash_map<merge_key, mesh_part> combined_parts;

			for (mesh_part &part : parts) {
				merge_key key;
				key.material = combine_everything ? nullptr : part.material;
				key.format = part.format;

				mesh_part &combined = combined_parts[key];
				if (combined.mesh == nullptr) {
					combined = std::move(part);
				} else {
					merge_mesh_part(combined, part);
				}
			}

			parts.clear();
			for (auto &pair : combined_parts) {
				parts.push_back(std::move(pair.value));
			}
		}

		{
			rh::array<mesh_part> split_parts;
			split_parts.reserve(parts.size());

			for (mesh_part &part : parts) {
				split_parts.insert_back(split_mesh(std::move(part), limits));
			}

			parts = std::move(split_parts);
		}

		for (mesh_part &part : parts) {
			optimize_mesh_part(part, optimize_opts);
		}

		rh::array<spmdl_node> sp_nodes;
		rh::array<spmdl_bone> sp_bones;
		rh::array<spmdl_mesh> sp_meshes;
		rh::array<char> sp_vertex;
		rh::array<char> sp_index;

		sp_nodes.reserve(model.nodes.size());
		sp_meshes.reserve(parts.size());

		for (model_node &node : model.nodes) {
			spmdl_node sp_node = { };
			sp_node.parent = node.parent_ix;
			sp_node.name = str_pool.insert(node.node->name);
			sp_node.translation = to_spmdl(node.node->transform.translation);
			sp_node.rotation = to_spmdl(node.node->transform.rotation);
			sp_node.scale = to_spmdl(node.node->transform.scale);
			sp_nodes.push_back(std::move(sp_node));
		}

		for (mesh_part &part : parts) {
			spmdl_mesh sp_mesh = { };
			sp_mesh.node = find_node(model, &part.mesh->node);
			sp_mesh.num_attribs = part.format.num_attribs;
			sp_mesh.num_vertex_buffers = part.format.num_streams;
			sp_mesh.num_indices = (uint32_t)part.num_indices;
			sp_mesh.num_vertices = (uint32_t)part.num_vertices;

			if (part.bones.size() > 0) {
				sp_mesh.bone_offset = (uint32_t)sp_bones.size();
				sp_mesh.num_bones = (uint32_t)part.bones.size();
				for (mesh_bone &bone : part.bones) {
					spmdl_bone sp_bone = { };
					sp_bone.node = find_node(model, bone.node);
					sp_bone.mesh_to_bone = to_spmdl(bone.mesh_to_bind);
					sp_bones.push_back(sp_bone);
				}
			}

			if (part.num_vertices < UINT16_MAX) {
				rh::array<uint16_t> indices16;
				indices16.resize_uninit(part.index_data.size());
				uint16_t *dst = indices16.data();
				for (uint32_t ix : part.index_data) {
					*dst++ = (uint16_t)ix;
				}
				sp_mesh.index_buffer = sp_push_buffer(sp_index, indices16.slice());
			} else {
				sp_mesh.index_buffer = sp_push_buffer(sp_index, part.index_data.slice());
			}

			// Pad indices to 4 bytes
			while (sp_index.size() % 4 != 0) sp_index.push_back(0);

			rh::array<char> vertex_data;
			for (uint32_t i = 0; i < part.format.num_streams; i++) {
				uint32_t stride = part.format.stream_stride[i];
				encode_vertex_stream(vertex_data, part, i);

				// Pre-pad vertices to stride bytes
				while (sp_vertex.size() % stride != 0) sp_vertex.push_back(0);

				sp_mesh.vertex_buffers[i] = sp_push_buffer(sp_vertex, vertex_data.slice(), stride);
			}

			memcpy(sp_mesh.attribs, part.format.attribs, sizeof(sp_mesh.attribs));

			sp_meshes.push_back(std::move(sp_mesh));
		}

		spmdl_header header = { };
		header.header.magic = SPFILE_HEADER_SPMDL;
		header.header.header_info_size = sizeof(spmdl_info);
		header.header.num_sections = 5;
		header.header.version = 1;
		header.info.num_nodes = (uint32_t)sp_nodes.size();
		header.info.num_bones = (uint32_t)sp_bones.size();
		header.info.num_meshes = (uint32_t)sp_meshes.size();

		compress_result c_nodes, c_bones, c_meshes, c_strings, c_vertex, c_index;

		uint32_t offset = sizeof(header);
		init_section(offset, header.s_nodes, c_nodes, sp_nodes.slice(), compress_opts, SPFILE_SECTION_NODES);
		init_section(offset, header.s_bones, c_bones, sp_bones.slice(), compress_opts, SPFILE_SECTION_BONES);
		init_section(offset, header.s_meshes, c_meshes, sp_meshes.slice(), compress_opts, SPFILE_SECTION_MESHES);
		init_section(offset, header.s_strings, c_strings, str_pool.data.slice(), compress_opts, SPFILE_SECTION_STRINGS);
		init_section(offset, header.s_vertex, c_vertex, sp_vertex.slice(), compress_opts, SPFILE_SECTION_VERTEX);
		init_section(offset, header.s_index, c_index, sp_index.slice(), compress_opts, SPFILE_SECTION_INDEX);

		{
			FILE *f = fopen(output_file, "wb");

			write_pod(f, header);
			write_pod(f, c_nodes.data.slice());
			write_pod(f, c_bones.data.slice());
			write_pod(f, c_meshes.data.slice());
			write_pod(f, c_strings.data.slice());
			write_pod(f, c_vertex.data.slice());
			write_pod(f, c_index.data.slice());

			fclose(f);
		}

#if 0
		{
			FILE *f = fopen(output_file, "wb");

			gltf_file gfile = convert_to_gltf(model, parts.slice());
			write_gltf(f, gfile);

			fclose(f);
		}
#endif

	}

	if (do_anim) {

		rh::array<spanim_bone> bones;
		bones.reserve(model.nodes.size());
		for (model_node &node : model.nodes) {
			spanim_bone bone;
			bone.parent = node.parent_ix;
			bone.name = str_pool.insert(node.node->name);
			bones.push_back(std::move(bone));
		}

		rh::array<char> anim_data;

		anim_data = compress_animation(model, scene, anim_opts);

		compress_result c_bones, c_strings, c_animation;

		spanim_header header = { };
		header.header.magic = SPFILE_HEADER_SPANIM;
		header.header.version = 1;
		header.header.header_info_size = sizeof(spanim_info);
		header.header.num_sections = 3;
		header.info.duration = 1.0;

		uint32_t offset = sizeof(header);
		init_section(offset, header.s_bones, c_bones, bones.slice(), compress_opts, SPFILE_SECTION_BONES);
		init_section(offset, header.s_strings, c_strings, str_pool.data.slice(), compress_opts, SPFILE_SECTION_STRINGS);
		init_section(offset, header.s_animation, c_animation, anim_data.slice(), compress_opts, SPFILE_SECTION_ANIMATION);

		{
			FILE *f = fopen(output_file, "wb");

			write_pod(f, header);
			write_pod(f, c_bones.data.slice());
			write_pod(f, c_strings.data.slice());
			write_pod(f, c_animation.data.slice());

			fclose(f);
		}
	}

	ufbx_free_scene(scene);

	return 0;
}
