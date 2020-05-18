#include "../sp_tools_common.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <vector>
#include <string>
#include <regex>
#include <limits>
#include "ufbx.h"

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
	vertex_format format;
	std::vector<float> vertex_data;
	std::vector<uint32_t> index_data;
	std::vector<mesh_bone> bones;
};

struct mesh_data
{
	std::vector<mesh_part> material_parts;
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

void process_mesh(ufbx_mesh *mesh, const mesh_data_format &fmt, const mesh_opts &opts)
{
	std::vector<float> data;
	data.resize(fmt.vertex_size_in_floats * mesh->num_indices);

	std::vector<mesh_bone> bones;

	std::vector<ufbx_vec3> transformed_positions;
	std::vector<ufbx_vec3> transformed_normals;
	ufbx_vec3 *positions = mesh->vertex_position.data;
	ufbx_vec3 *normals = mesh->vertex_normal.data;

	if (opts.transform_to_root) {
		transformed_positions.reserve(mesh->vertex_position.num_elements);
		transformed_normals.reserve(mesh->vertex_normal.num_elements);

		if (positions) {
			for (size_t i = 0; i < mesh->vertex_position.num_elements; i++) {
				ufbx_vec3 v = ufbx_transform_position(&mesh->node.to_root, mesh->vertex_position.data[i]);
				transformed_positions.push_back(v);
			}
			positions = transformed_positions.data();
		}

		if (normals) {
			ufbx_matrix normal_to_root = ufbx_get_normal_matrix(&mesh->node.to_root);
			for (size_t i = 0; i < mesh->vertex_position.num_elements; i++) {
				ufbx_vec3 v = ufbx_transform_position(&normal_to_root, mesh->vertex_position.data[i]);
				transformed_normals.push_back(v);
			}
			normals = transformed_normals.data();
		}
	}

	std::vector<bone_weight> weights;

	if (fmt.weights_per_vertex > 0 && mesh->skins.size > 0) {
		weights.resize(fmt.weights_per_vertex * mesh->num_vertices);

		int32_t skin_index = 0;
		for (ufbx_skin &skin : mesh->skins) {
			size_t num_weights = skin.num_weights;

			mesh_bone bone;
			bone.node = skin.bone;
			bone.mesh_to_bind = skin.mesh_to_bind;
			bones.push_back(bone);

			for (size_t i = 0; i < num_weights; i++) {
				bone_weight *dst = weights.data() + skin.indices[i] * fmt.weights_per_vertex;
				bone_weight w { skin_index, skin.weights[i] };
				for (int c = 0; c < fmt.weights_per_vertex; c++) {
					if (w.weight > dst[c].weight) {
						std::swap(w, dst[c]);
					}
				}
			}

			skin_index++;
		}

		// Normalize weights
		for (size_t i = 0; i < mesh->num_vertices; i++) {
			bone_weight *w = weights.data() + i * fmt.weights_per_vertex;
			float total = 0.0f;
			for (int j = 0; j < fmt.weights_per_vertex; j++) {
				total += w[j].weight;
			}
			if (total > 0.0f) {
				for (int j = 0; j < fmt.weights_per_vertex; j++) {
					w[j].weight /= total;
				}
			}
		}
	}

	uint32_t attrib_offset_in_floats = 0;
	for (uint32_t i = 0; i < opts.format.num_attribs; i++) {
		const spmdl_attrib &attrib = opts.format.attribs[i];

		size_t stride_in_floats = fmt.vertex_size_in_floats;
		size_t num_indices = mesh->num_indices;
		float *dst = data.data() + attrib_offset_in_floats;
		switch (attrib.attrib) {

		case SP_VERTEX_ATTRIB_POSITION:
			if (positions) {
				for (size_t i = 0; i < num_indices; i++) {
					ufbx_vec3 v = positions[mesh->vertex_position.indices[i]];
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_NORMAL:
			if (normals) {
				for (size_t i = 0; i < num_indices; i++) {
					ufbx_vec3 v = normals[mesh->vertex_normal.indices[i]];
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_UV:
			if (mesh->vertex_uv.data) {
				for (size_t i = 0; i < num_indices; i++) {
					ufbx_vec2 v = ufbx_get_vertex_vec2(&mesh->vertex_uv, i);
					dst[0] = (float)v.x; dst[1] = (float)v.y;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_COLOR:
			if (mesh->vertex_color.data) {
				for (size_t i = 0; i < num_indices; i++) {
					ufbx_vec4 v = ufbx_get_vertex_vec4(&mesh->vertex_color, i);
					dst[0] = (float)v.x; dst[1] = (float)v.y; dst[2] = (float)v.z; dst[3] = (float)v.w;
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_BONE_INDEX:
			if (weights.size()) {
				for (size_t i = 0; i < num_indices; i++) {
					bone_weight *w = weights.data() + mesh->vertex_position.indices[i] * fmt.weights_per_vertex;
					for (size_t j = 0; j < fmt.weights_per_vertex; j++) {
						dst[j] = (float)w[j].index;
					}
					dst += stride_in_floats;
				}
			}
			break;

		case SP_VERTEX_ATTRIB_BONE_WEIGHT:
			if (weights.size()) {
				for (size_t i = 0; i < num_indices; i++) {
					bone_weight *w = weights.data() + mesh->vertex_position.indices[i] * fmt.weights_per_vertex;
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

	for (size_t i = 0; i < mesh->num_indices; i++) {
		float *f = data.data() + i * fmt.vertex_size_in_floats;
		for (size_t j = 0; j < fmt.vertex_size_in_floats; j++) {
			printf("%8.3f ", f[j]);
		}
		printf("\n");
	}
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
	process_mesh(&scene->meshes.data[0], fmt, opts);

}
