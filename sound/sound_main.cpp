#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "sp_tools_common.h"

#include "stb_vorbis.h"

bool g_verbose;

struct wav_header
{
	char chunk_id[4];
	uint32_t chunk_size;
	char format[4];
	char fmt_id[4];
	uint32_t fmt_size;
	uint16_t audio_format;
	uint16_t num_channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	char data_id[4];
	uint32_t data_size;
};

void failf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	vfprintf(stderr, fmt, args);
	putc('\n', stderr);

	va_end(args);

	exit(1);
}

const char zero_buf[64] = { 0 };

static void write_pad(FILE *f, size_t num)
{
	fwrite(zero_buf, 1, num, f);
}

static void write_data(FILE *f, const void *data, size_t size)
{
	size_t num = fwrite(data, 1, size, f);
	if (num != size) {
		fclose(f);
		failf("Failed to write output data");
	}
}

void *compress_section(spfile_section *section, uint32_t *file_offset, uint32_t *pad, const void *data, size_t size, int level, sp_compression_type compression_type, spfile_section_magic magic)
{
	size_t bound = sp_get_compression_bound(compression_type, size);
	void *result = malloc(bound);
	double no_compress_ratio = 1.1;

	size_t compressed_size = sp_compress_buffer(compression_type, result, bound, data, size, level);
	sp_compression_type mip_type = compression_type;
	if ((double)size / (double)compressed_size < no_compress_ratio && compression_type != SP_COMPRESSION_NONE) {
		compression_type = SP_COMPRESSION_NONE;
		memcpy(result, data, size);
		compressed_size = size;
	}

	while (*file_offset % 16 != 0) {
		++*pad;
		++*file_offset;
	}

	section->magic = magic;
	section->index = 0;
	section->compressed_size = (uint32_t)compressed_size;
	section->compression_type = compression_type;
	section->uncompressed_size = (uint32_t)size;
	section->offset = *file_offset;
	*file_offset += section->compressed_size;

	return result;
}

struct take
{
	const char *input_file;
	void *data;
	size_t data_size;
	spsound_format format;
};

#define MAX_TAKES 1024

take takes[MAX_TAKES];
spsound_take sp_takes[MAX_TAKES];

void *audio_data = nullptr;
size_t audio_size = 0;
size_t audio_capacity = 0;

void push_audio(const void *data, size_t size)
{
	if (audio_size + size > audio_capacity) {
		audio_capacity *= 2;
		if (audio_capacity < audio_size + size) {
			audio_capacity = audio_size + size;
		}
		audio_data = realloc(audio_data, audio_capacity);
	}

	memcpy((char*)audio_data + audio_size, data, size);
	audio_size += size;
}

int main(int argc, char **argv)
{
	const char *output_file = NULL;
	bool show_help = false;
	int level = 10;
	uint32_t num_takes = 0;

	// -- Parse arguments

	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		int left = argc - argi - 1;

		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			g_verbose = true;
		} else if (!strcmp(arg, "--help")) {
			show_help = true;
		} else if (left >= 1) {
			if (!strcmp(arg, "-i") || !strcmp(arg, "--input")) {
				if (num_takes >= MAX_TAKES) {
					failf("Too many takes");
				}
				takes[num_takes].input_file = argv[++argi];
				num_takes++;
			} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
				output_file = argv[++argi];
			} else if (!strcmp(arg, "-l") || !strcmp(arg, "--level")) {
				level = atoi(argv[++argi]);
				if (level <= 0 || level > 20) {
					failf("Invalid level %d, must be between 1-20", level);
				}
			}
		}
	}

	if (show_help) {
		printf("%s",
			"Usage: sp-sound -i <input> -o <output>\n"
			"    -i / --input <path>: Input filename in any format stb_image supports\n"
			"    -o / --output <path>: Destination filename\n"
			"    -v / --verbose: Verbose output\n"
		);

		return 0;
	}

	for (uint32_t i = 0; i < num_takes; i++) {
		take *t = &takes[i];

		FILE *f = fopen(t->input_file, "rb");
		if (!f) failf("Failed to open input file: %s", t->input_file);

		fseek(f, 0, SEEK_END);
		size_t file_size = ftell(f);
		fseek(f, 0, SEEK_SET);

		void *file_data = malloc(file_size);
		size_t num_read = fread(file_data, 1, file_size, f);
		if (num_read != file_size) failf("Failed to read input file: %s", t->input_file);
		fclose(f);

		if (file_size >= 4 && !memcmp(file_data, "RIFF", 4)) {
			// WAV file
			if (file_size < sizeof(wav_header)) failf("Invalid WAVE file");
			wav_header *wav = (wav_header*)file_data; 

			if (memcmp(wav->format, "WAVE", 4) != 0) failf("wav: Bad Format: %.4s", wav->format);
			if (memcmp(wav->fmt_id, "fmt ", 4) != 0) failf("wav: Bad fmt chunk ID: %.4s", wav->fmt_id);
			if (wav->fmt_size != 16) failf("wav: Bad fmt chunk size: %u", wav->fmt_size);
			if (wav->audio_format != 1) failf("wav: Bad audio format: %u", wav->audio_format);
			if (wav->num_channels < 1) failf("wav: Bad number of channels: %u", wav->num_channels);
			if (wav->bits_per_sample != 8 && wav->bits_per_sample != 16 && wav->bits_per_sample != 24 && wav->bits_per_sample != 32) {
				failf("wav: Bad number of channels: %u", wav->num_channels);
			}
			if (memcmp(wav->data_id, "data", 4) != 0) failf("wav: Bad data chunk ID: %.4s", wav->fmt_id);
			uint32_t num_samples = wav->data_size / (wav->num_channels * (wav->bits_per_sample / 8));
			uint32_t num_channels = wav->num_channels <= 2 ? wav->num_channels : 2;
			if (file_size < sizeof(wav_header) + wav->data_size) failf("wav: Truncated file");

			uint32_t stride = wav->num_channels*(wav->bits_per_sample/8);

			const char *src = (const char*)file_data + sizeof(wav_header);
			int16_t *pcm_data = (int16_t*)malloc(num_samples * num_channels * sizeof(int16_t));
			int16_t *dst = pcm_data;

			for (uint32_t i = 0; i < num_samples; i++) {
				for (uint32_t j = 0; j < num_channels; j++) {
					const char *s = src + i*stride + j*(wav->bits_per_sample/8);
					int16_t d;
					switch (wav->bits_per_sample / 8) {
					case 1: d = (int16_t)((int32_t)*(uint8_t*)s * 0x101 - 32768); break;
					case 2: d = *(int16_t*)s; break;
					case 3: d = *(int16_t*)(s + 1); break;
					case 4: d = *(int16_t*)(s + 2); break;
					}
					dst[j] = d;
				}
				dst += num_channels;
			}

			spsound_take *spt = &sp_takes[i];
			spt->format = SPSOUND_FORMAT_PCM16;
			spt->file_offset = (uint32_t)audio_size;
			spt->file_size = num_samples * num_channels * sizeof(int16_t);
			spt->length_in_samples = num_samples;
			spt->length_in_seconds = (float)num_samples / (float)wav->sample_rate;
			spt->num_channels = num_channels;
			spt->sample_rate = wav->sample_rate;
			spt->temp_memory_required = 0;

			push_audio(pcm_data, spt->file_size);

		} else if (file_size >= 4 && !memcmp(file_data, "OggS", 4)) {

			int error;
			stb_vorbis *v = stb_vorbis_open_memory((const unsigned char*)file_data, (int)file_size, &error, NULL);
			if (error) failf("Failed to read Vorbis file: %d", error);

			stb_vorbis_info info = stb_vorbis_get_info(v);

			spsound_take *spt = &sp_takes[i];
			spt->format = SPSOUND_FORMAT_VORBIS;
			spt->file_offset = (uint32_t)audio_size;
			spt->file_size = (uint32_t)file_size;
			spt->length_in_samples = stb_vorbis_stream_length_in_samples(v);
			spt->length_in_seconds = stb_vorbis_stream_length_in_seconds(v);
			spt->num_channels = (uint32_t)info.channels;
			spt->sample_rate = info.sample_rate;
			spt->temp_memory_required = info.temp_memory_required + info.setup_temp_memory_required + info.setup_memory_required;

			push_audio(file_data, (uint32_t)file_size);

		} else {
			failf("Unsupported file type");
		}

		free(file_data);
	}

	spsound_header header;
	header.header.magic = SPFILE_HEADER_SPSOUND;
	header.header.version = 1;
	header.header.header_info_size = sizeof(spsound_info);
	header.header.num_sections = 2;
	header.info.num_takes = num_takes;
	header.info.temp = 0;

	uint32_t file_offset = sizeof(spsound_header);
	uint32_t take_pad = 0, audio_pad = 0;

	void *take_comp = compress_section(&header.s_takes, &file_offset, &take_pad, sp_takes, num_takes * sizeof(spsound_take), level, SP_COMPRESSION_ZSTD, SPFILE_SECTION_TAKES);
	void *audio_comp = compress_section(&header.s_audio, &file_offset, &audio_pad, audio_data, audio_size, level, SP_COMPRESSION_ZSTD, SPFILE_SECTION_TAKES);

	FILE *f = fopen(output_file, "wb");
	if (!f) failf("Failed to open output file: %s", output_file);

	write_data(f, &header, sizeof(header));

	write_pad(f, take_pad);
	write_data(f, take_comp, header.s_takes.compressed_size);

	write_pad(f, audio_pad);
	write_data(f, audio_comp, header.s_audio.compressed_size);

	fclose(f);

	return 0;
}
