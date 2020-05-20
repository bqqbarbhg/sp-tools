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
#include "rh_hash.h"

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
	{ "uv", "uv", SP_VERTEX_ATTRIB_UV },
	{ "col", "color", SP_VERTEX_ATTRIB_COLOR },
	{ "bonei", "bone-index", SP_VERTEX_ATTRIB_BONE_INDEX },
	{ "bonew", "bone-weight", SP_VERTEX_ATTRIB_BONE_WEIGHT },
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
	uint32_t position_offset_in_floats = 0;
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
	ufbx_mesh *mesh;
	vertex_format format;
	mesh_data_format data_format;
	ufbx_material *material;
	size_t num_indices = 0;
	size_t num_vertices = 0;
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
};

template <typename T>
struct buffer_hash
{
};

mesh_data_format create_mesh_data_format(const mesh_opts &opts)
{
	mesh_data_format fmt = { };

	for (uint32_t i = 0; i < opts.format.num_attribs; i++) {
		const spmdl_attrib &attrib = opts.format.attribs[i];

		uint32_t attrib_size_in_floats = 0;
		switch (attrib.attrib) {
		case SP_VERTEX_ATTRIB_POSITION:
			fmt.position_offset_in_floats = fmt.vertex_size_in_floats;
			attrib_size_in_floats = 3;
			break;
		case SP_VERTEX_ATTRIB_NORMAL: attrib_size_in_floats = 3; break;
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
		}

		fmt.attrib_size_in_floats[i] = (uint8_t)attrib_size_in_floats;
		fmt.vertex_size_in_floats += attrib_size_in_floats;
	}

	return fmt;
}

mesh_part process_mesh_part(ufbx_mesh *mesh, const mesh_data_format &fmt, const mesh_opts &opts, const mesh_part_source &src, uint32_t material_ix)
{
	mesh_part part;
	part.mesh = mesh;
	part.format = opts.format;
	part.data_format = fmt;

	if (mesh->materials.size) {
		part.material = mesh->materials.data[material_ix];
	} else {
		part.material = nullptr;
	}

	rh::array<float> vertex_data;
	vertex_data.resize(fmt.vertex_size_in_floats * src.indices.size());

	rh::array<int32_t> bone_remap;
	bone_remap.resize(src.bones.size(), -1);

	uint32_t attrib_offset_in_floats = 0;
	for (uint32_t i = 0; i < opts.format.num_attribs; i++) {
		const spmdl_attrib &attrib = opts.format.attribs[i];

		size_t stride_in_floats = fmt.vertex_size_in_floats;
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
			if (src.normals) {
				for (size_t i : src.indices) {
					ufbx_vec3 v = src.normals[mesh->vertex_normal.indices[i]];
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_UV:
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

	part.vertex_data.resize_uninit(part.num_vertices * fmt.vertex_size_in_floats);

	meshopt_optimizeVertexCache(part.index_data.data(), part.index_data.data(), part.num_indices, part.num_vertices);

	return part;
}

void merge_mesh_part(mesh_part &dst, const mesh_part src)
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

	float *src_vertex_begin = dst.vertex_data.data() + dst.data_format.vertex_size_in_floats * dst.num_vertices;
	uint32_t attrib_offset_in_floats = 0;
	for (uint32_t i = 0; i < dst.format.num_attribs; i++) {
		const spmdl_attrib &attrib = dst.format.attribs[i];

		size_t stride_in_floats = dst.data_format.vertex_size_in_floats;
		float *dst_data = dst.vertex_data.data() + attrib_offset_in_floats;
		switch (attrib.attrib) {

		case SP_VERTEX_ATTRIB_BONE_INDEX:
			{
				for (size_t vi = 0; vi < src.num_vertices; vi++) {
					for (size_t j = 0; j < dst.data_format.weights_per_vertex; j++) {
						dst_data[j] = (float)bone_remap[(int32_t)dst_data[j]];
					}
				}
			}
			break;

		}

		attrib_offset_in_floats += (uint32_t)dst.data_format.attrib_size_in_floats[i];
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

		part.vertex_data.reserve(split.vertex_indices.size() * src.data_format.vertex_size_in_floats);
		part.bones.reserve(split.bone_indices.size());

		for (uint32_t bone_ix : split.bone_indices) {
			part.bones.push_back(src.bones[bone_ix]);
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
			for (size_t i = 0; i < mesh->vertex_position.num_elements; i++) {
				ufbx_vec3 v = ufbx_transform_position(&normal_to_root, mesh->vertex_position.data[i]);
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

			mesh_bone bone;
			bone.node = skin.bone;
			bone.mesh_to_bind = skin.mesh_to_bind;
			src.bones.push_back(bone);

			for (size_t i = 0; i < num_weights; i++) {
				bone_weight *dst = weights.data() + skin.indices[i] * fmt.weights_per_vertex;
				bone_weight w { skin_index, (float)skin.weights[i] };
				for (int c = 0; c < fmt.weights_per_vertex; c++) {
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
			for (int j = 0; j < fmt.weights_per_vertex; j++) {
				total += w[j].weight;
			}
			if (total > 0.0f) {
				int32_t last_index = 0;
				for (int j = 0; j < fmt.weights_per_vertex; j++) {
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

	uint32_t num_materials = mesh->materials.size;
	if (num_materials == 0) num_materials = 1;

	int32_t *face_material = mesh->face_material;
	for (uint32_t mat_ix = 0; mat_ix < mesh->materials.size; mat_ix++) {

		src.indices.clear();

		size_t num_triangles = 0;

		for (size_t fi = 0; fi < mesh->num_faces; fi++) {
			if (face_material && face_material[fi] != mat_ix) continue;
			ufbx_face face = mesh->faces[fi];
			for (size_t i = 1; i + 2 <= face.num_indices; i++) {
				src.indices.push_back(face.index_begin + 0);
				src.indices.push_back(face.index_begin + i + 0);
				src.indices.push_back(face.index_begin + i + 1);
			}
		}

		mesh_part part = process_mesh_part(mesh, fmt, opts, src, mat_ix);
		parts.push_back(std::move(part));
	}

	return parts;
}

int main(int argc, char **argv)
{
	const char *input_file = NULL;
	const char *output_file = NULL;
	bool verbose = false;
	bool show_help = false;
	int level = 10;
	int num_threads = 1;
	vertex_format mesh_format;

	// -- Parse arguments

	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		int left = argc - argi - 1;

		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			verbose = true;
		} else if (!strcmp(arg, "--help")) {
			show_help = true;
		} else if (!strcmp(arg, "--combine-meshes")) {
			// mp_opts.combine_meshes = true;
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
			} else if (!strcmp(arg, "--mesh")) {
				mesh_format = parse_attribs(argv[++argi]);
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

	// -- Load input FBX

	ufbx_load_opts ufbx_opts = { };
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

	mesh_opts opts;
	opts.format = mesh_format;
	mesh_data_format fmt = create_mesh_data_format(opts);

	mesh_limits limits;
	limits.max_bones = 32;
	limits.max_vertices = 1024;

	rh::array<mesh_part> parts;

	for (ufbx_mesh &mesh : scene->meshes) {
		for (mesh_part &part : process_mesh(&mesh, fmt, opts)) {
			for (mesh_part &split_part : split_mesh(std::move(part), limits)) {
				parts.push_back(std::move(split_part));
			}
		}
	}

	return 0;
}
