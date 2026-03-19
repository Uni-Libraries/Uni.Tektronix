#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "uni_tektronix_r3f.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UT_PI 3.141592653589793238462643383279502884
#define UT_READ_CHUNK_SAMPLES 65536u
#define UT_WAV_BUFFER_IQ_SAMPLES 4096u

typedef enum ut_input_kind {
    UT_INPUT_R3F = 0,
    UT_INPUT_R3A = 1
} ut_input_kind;

typedef enum ut_tail_mode {
    UT_TAIL_MODE_PAD = 0,
    UT_TAIL_MODE_DROP = 1,
    UT_TAIL_MODE_ERROR = 2
} ut_tail_mode;

typedef struct ut_source {
    ut_input_kind kind;
    uni_tektronix_r3f_header header;
    uint64_t sample_count;
    union {
        uni_tektronix_r3f_reader* r3f;
        uni_tektronix_r3a_reader* r3a;
    } handle;
} ut_source;

typedef struct ut_wav_writer {
    FILE* file;
    uint32_t sample_rate_hz;
    uint64_t iq_sample_count;
} ut_wav_writer;

typedef struct ut_ddc_state {
    double sample_rate_sps;
    double if_frequency_hz;
    double gain;
    uint32_t decimate;
    size_t taps;
    double* coeffs;
    double* hist_i;
    double* hist_q;
    size_t hist_pos;
    uint32_t decim_phase;
    double phase;
    double phase_step;
} ut_ddc_state;

static void
ut_print_help(FILE* stream) {
    fprintf(stream,
            "uni_tektronix_tool - CLI for Tektronix R3F/R3A/R3H captures\n\n"
            "Usage:\n"
            "  uni_tektronix_tool info <input>\n"
            "  uni_tektronix_tool validate <input>\n"
            "  uni_tektronix_tool split <input.r3f> <output-base|output.r3a|output.r3h>\n"
            "  uni_tektronix_tool pack <input-base|input.r3a|input.r3h> <output.r3f> [--tail-mode pad|drop|error] [--pad-value N]\n"
            "  uni_tektronix_tool extract-raw <input> <output.i16> [--start N] [--count N]\n"
            "  uni_tektronix_tool trim <input> <output.r3f|output-base|output.r3a|output.r3h> --start N [--count N] [--tail-mode pad|drop|error] [--pad-value N]\n"
            "  uni_tektronix_tool to-wav <input> <output.wav> [--start N] [--count N] [--decimate N] [--gain G] [--if-frequency Hz]\n"
            "  uni_tektronix_tool export-footers <input.r3f> <output.csv> [--start-frame N] [--count N]\n"
            "\nAliases: to-raw == split, to-r3f == pack, wav == to-wav, to-i16 == extract-raw\n");
}

static const char*
ut_kind_name(ut_input_kind kind) {
    return kind == UT_INPUT_R3F ? "r3f" : "r3a/r3h";
}

static int
ut_has_extension(const char* path, const char* ext) {
    size_t path_len = 0u;
    size_t ext_len = 0u;

    if (path == NULL || ext == NULL) {
        return 0;
    }
    path_len = strlen(path);
    ext_len = strlen(ext);
    if (path_len < ext_len) {
        return 0;
    }
    for (size_t i = 0u; i < ext_len; ++i) {
        char a = path[path_len - ext_len + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static char*
ut_strdup_local(const char* text) {
    const size_t len = strlen(text);
    char* copy = (char*)malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1u);
    return copy;
}

static char*
ut_make_sibling_path(const char* path, const char* desired_ext) {
    const size_t path_len = strlen(path);
    const size_t ext_len = strlen(desired_ext);
    size_t stem_len = path_len;
    char* result = NULL;

    if (ut_has_extension(path, ".r3a") || ut_has_extension(path, ".r3h") || ut_has_extension(path, ".r3f")) {
        stem_len = path_len - 4u;
    }

    result = (char*)malloc(stem_len + ext_len + 1u);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, path, stem_len);
    memcpy(result + stem_len, desired_ext, ext_len + 1u);
    return result;
}

static int
ut_file_exists(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int
ut_parse_u64(const char* text, uint64_t* out_value) {
    unsigned long long value = 0ull;
    char* end = NULL;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtoull(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    *out_value = (uint64_t)value;
    return 1;
}

static int
ut_parse_i32(const char* text, int32_t* out_value) {
    long value = 0l;
    char* end = NULL;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    *out_value = (int32_t)value;
    return 1;
}

static int
ut_parse_u32(const char* text, uint32_t* out_value) {
    unsigned long value = 0ul;
    char* end = NULL;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    *out_value = (uint32_t)value;
    return 1;
}

static int
ut_parse_double(const char* text, double* out_value) {
    double value = 0.0;
    char* end = NULL;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtod(text, &end);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    *out_value = value;
    return 1;
}

static int
ut_parse_tail_mode(const char* text, ut_tail_mode* out_mode) {
    if (text == NULL || out_mode == NULL) {
        return 0;
    }
    if (strcmp(text, "pad") == 0) {
        *out_mode = UT_TAIL_MODE_PAD;
        return 1;
    }
    if (strcmp(text, "drop") == 0) {
        *out_mode = UT_TAIL_MODE_DROP;
        return 1;
    }
    if (strcmp(text, "error") == 0) {
        *out_mode = UT_TAIL_MODE_ERROR;
        return 1;
    }
    return 0;
}

static void
ut_source_close(ut_source* source) {
    if (source == NULL) {
        return;
    }
    if (source->kind == UT_INPUT_R3F) {
        uni_tektronix_r3f_reader_close(source->handle.r3f);
        source->handle.r3f = NULL;
    } else {
        uni_tektronix_r3a_reader_close(source->handle.r3a);
        source->handle.r3a = NULL;
    }
}

static uni_tektronix_r3f_status
ut_source_open(const char* path, ut_source* out_source) {
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;
    char* inferred_r3f = NULL;

    if (path == NULL || out_source == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    memset(out_source, 0, sizeof(*out_source));

    if (ut_has_extension(path, ".r3f")) {
        out_source->kind = UT_INPUT_R3F;
        status = uni_tektronix_r3f_reader_open(path, &out_source->handle.r3f);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            return status;
        }
        (void)uni_tektronix_r3f_reader_get_header(out_source->handle.r3f, &out_source->header);
        (void)uni_tektronix_r3f_reader_get_sample_count(out_source->handle.r3f, &out_source->sample_count);
        return UNI_TEKTRONIX_R3F_STATUS_OK;
    }

    if (!ut_has_extension(path, ".r3a") && !ut_has_extension(path, ".r3h")) {
        inferred_r3f = ut_make_sibling_path(path, ".r3f");
        if (inferred_r3f != NULL && ut_file_exists(inferred_r3f)) {
            out_source->kind = UT_INPUT_R3F;
            status = uni_tektronix_r3f_reader_open(inferred_r3f, &out_source->handle.r3f);
            free(inferred_r3f);
            if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
                return status;
            }
            (void)uni_tektronix_r3f_reader_get_header(out_source->handle.r3f, &out_source->header);
            (void)uni_tektronix_r3f_reader_get_sample_count(out_source->handle.r3f, &out_source->sample_count);
            return UNI_TEKTRONIX_R3F_STATUS_OK;
        }
        free(inferred_r3f);
        inferred_r3f = NULL;
    }

    out_source->kind = UT_INPUT_R3A;
    status = uni_tektronix_r3a_reader_open(path, NULL, &out_source->handle.r3a);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return status;
    }
    (void)uni_tektronix_r3a_reader_get_header(out_source->handle.r3a, &out_source->header);
    (void)uni_tektronix_r3a_reader_get_sample_count(out_source->handle.r3a, &out_source->sample_count);
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
ut_source_read_samples(ut_source* source,
                       uint64_t start_sample_index,
                       size_t requested_samples,
                       int16_t* out_samples,
                       size_t out_capacity,
                       size_t* out_samples_read) {
    if (source->kind == UT_INPUT_R3F) {
        return uni_tektronix_r3f_reader_read_samples_i16(source->handle.r3f,
                                                         start_sample_index,
                                                         requested_samples,
                                                         out_samples,
                                                         out_capacity,
                                                         out_samples_read);
    }
    return uni_tektronix_r3a_reader_read_samples_i16(source->handle.r3a,
                                                     start_sample_index,
                                                     requested_samples,
                                                     out_samples,
                                                     out_capacity,
                                                     out_samples_read);
}

static int
ut_resolve_sample_window(const ut_source* source,
                         uint64_t start_sample,
                         uint64_t requested_count,
                         uint64_t* out_start,
                         uint64_t* out_count) {
    const uint64_t total_samples = source->sample_count;

    if (start_sample > total_samples) {
        return 0;
    }

    *out_start = start_sample;
    if (requested_count == UINT64_MAX) {
        *out_count = total_samples - start_sample;
        return 1;
    }
    if (requested_count > (total_samples - start_sample)) {
        return 0;
    }
    *out_count = requested_count;
    return 1;
}

static void
ut_adjust_trim_header(uni_tektronix_r3f_header* header, uint64_t start_sample) {
    if (header == NULL) {
        return;
    }
    if (start_sample != 0u) {
        header->start_sample_count += start_sample;
    }
}

static uni_tektronix_r3f_status
ut_stream_range_to_r3a(ut_source* source,
                       uint64_t start_sample,
                       uint64_t sample_count,
                       const char* output_data_path,
                       const char* output_header_path,
                       const uni_tektronix_r3f_header* header_template) {
    uni_tektronix_r3a_writer* writer = NULL;
    int16_t* buffer = NULL;
    uint64_t remaining = sample_count;
    uint64_t cursor = start_sample;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    buffer = (int16_t*)malloc(UT_READ_CHUNK_SAMPLES * sizeof(int16_t));
    if (buffer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    status = uni_tektronix_r3a_writer_create(output_data_path, output_header_path, header_template, &writer);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        free(buffer);
        return status;
    }

    while (remaining > 0u) {
        const size_t want = remaining > UT_READ_CHUNK_SAMPLES ? UT_READ_CHUNK_SAMPLES : (size_t)remaining;
        size_t got = 0u;

        status = ut_source_read_samples(source, cursor, want, buffer, UT_READ_CHUNK_SAMPLES, &got);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            uni_tektronix_r3a_writer_discard(writer);
            free(buffer);
            return status;
        }
        if (got == 0u) {
            break;
        }

        status = uni_tektronix_r3a_writer_append_samples_i16(writer, buffer, got);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            uni_tektronix_r3a_writer_discard(writer);
            free(buffer);
            return status;
        }

        cursor += (uint64_t)got;
        remaining -= (uint64_t)got;
    }

    free(buffer);
    return uni_tektronix_r3a_writer_close(writer);
}

static uni_tektronix_r3f_status
ut_stream_range_to_raw_i16_file(ut_source* source,
                                uint64_t start_sample,
                                uint64_t sample_count,
                                const char* output_path) {
    FILE* file = NULL;
    int16_t* buffer = NULL;
    uint64_t remaining = sample_count;
    uint64_t cursor = start_sample;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (output_path == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    file = fopen(output_path, "wb");
    if (file == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    buffer = (int16_t*)malloc(UT_READ_CHUNK_SAMPLES * sizeof(int16_t));
    if (buffer == NULL) {
        fclose(file);
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    while (remaining > 0u) {
        const size_t want = remaining > UT_READ_CHUNK_SAMPLES ? UT_READ_CHUNK_SAMPLES : (size_t)remaining;
        size_t got = 0u;

        status = ut_source_read_samples(source, cursor, want, buffer, UT_READ_CHUNK_SAMPLES, &got);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            free(buffer);
            fclose(file);
            return status;
        }
        if (got == 0u) {
            break;
        }

        for (size_t i = 0u; i < got; ++i) {
            unsigned char bytes[2];
            uint16_t sample_u16 = (uint16_t)buffer[i];
            bytes[0] = (unsigned char)(sample_u16 & 0xffu);
            bytes[1] = (unsigned char)((sample_u16 >> 8u) & 0xffu);
            if (fwrite(bytes, 1u, sizeof(bytes), file) != sizeof(bytes)) {
                free(buffer);
                fclose(file);
                return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
            }
        }

        cursor += (uint64_t)got;
        remaining -= (uint64_t)got;
    }

    free(buffer);
    if (fclose(file) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
ut_stream_range_to_r3f(ut_source* source,
                       uint64_t start_sample,
                       uint64_t sample_count,
                       const char* output_path,
                       const uni_tektronix_r3f_header* header_template,
                       ut_tail_mode tail_mode,
                       int16_t pad_value) {
    uni_tektronix_r3f_writer* writer = NULL;
    int16_t* read_buffer = NULL;
    int16_t* frame_buffer = NULL;
    uint64_t remaining = sample_count;
    uint64_t cursor = start_sample;
    size_t frame_fill = 0u;
    const size_t samples_per_frame = (size_t)(uint32_t)header_template->samples_per_frame;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    read_buffer = (int16_t*)malloc(UT_READ_CHUNK_SAMPLES * sizeof(int16_t));
    frame_buffer = (int16_t*)malloc(samples_per_frame * sizeof(int16_t));
    if (read_buffer == NULL || frame_buffer == NULL) {
        free(read_buffer);
        free(frame_buffer);
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    status = uni_tektronix_r3f_writer_create(output_path, header_template, &writer);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        free(read_buffer);
        free(frame_buffer);
        return status;
    }

    while (remaining > 0u) {
        const size_t want = remaining > UT_READ_CHUNK_SAMPLES ? UT_READ_CHUNK_SAMPLES : (size_t)remaining;
        size_t got = 0u;
        size_t offset = 0u;

        status = ut_source_read_samples(source, cursor, want, read_buffer, UT_READ_CHUNK_SAMPLES, &got);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            uni_tektronix_r3f_writer_discard(writer);
            free(read_buffer);
            free(frame_buffer);
            return status;
        }
        if (got == 0u) {
            break;
        }

        while (offset < got) {
            const size_t take = (got - offset) < (samples_per_frame - frame_fill)
                                    ? (got - offset)
                                    : (samples_per_frame - frame_fill);
            memcpy(frame_buffer + frame_fill, read_buffer + offset, take * sizeof(int16_t));
            frame_fill += take;
            offset += take;

            if (frame_fill == samples_per_frame) {
                status = uni_tektronix_r3f_writer_append_frame_i16(writer, frame_buffer, NULL);
                if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
                    uni_tektronix_r3f_writer_discard(writer);
                    free(read_buffer);
                    free(frame_buffer);
                    return status;
                }
                frame_fill = 0u;
            }
        }

        cursor += (uint64_t)got;
        remaining -= (uint64_t)got;
    }

    if (frame_fill != 0u) {
        if (tail_mode == UT_TAIL_MODE_ERROR) {
            uni_tektronix_r3f_writer_discard(writer);
            (void)remove(output_path);
            free(read_buffer);
            free(frame_buffer);
            return UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING;
        }
        if (tail_mode == UT_TAIL_MODE_PAD) {
            for (size_t i = frame_fill; i < samples_per_frame; ++i) {
                frame_buffer[i] = pad_value;
            }
            status = uni_tektronix_r3f_writer_append_frame_i16(writer, frame_buffer, NULL);
            if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
                uni_tektronix_r3f_writer_discard(writer);
                free(read_buffer);
                free(frame_buffer);
                return status;
            }
        }
    }

    free(read_buffer);
    free(frame_buffer);
    return uni_tektronix_r3f_writer_close(writer);
}

static int
ut_resolve_raw_pair_output(const char* argument, char** out_data_path, char** out_header_path) {
    if (argument == NULL || out_data_path == NULL || out_header_path == NULL) {
        return 0;
    }
    *out_data_path = NULL;
    *out_header_path = NULL;

    if (ut_has_extension(argument, ".r3a")) {
        *out_data_path = ut_strdup_local(argument);
        *out_header_path = ut_make_sibling_path(argument, ".r3h");
    } else if (ut_has_extension(argument, ".r3h")) {
        *out_header_path = ut_strdup_local(argument);
        *out_data_path = ut_make_sibling_path(argument, ".r3a");
    } else {
        *out_data_path = ut_make_sibling_path(argument, ".r3a");
        *out_header_path = ut_make_sibling_path(argument, ".r3h");
    }

    if (*out_data_path == NULL || *out_header_path == NULL) {
        free(*out_data_path);
        free(*out_header_path);
        *out_data_path = NULL;
        *out_header_path = NULL;
        return 0;
    }
    return 1;
}

static void
ut_print_datetime(FILE* stream, const uni_tektronix_r3f_datetime* dt) {
    fprintf(stream,
            "%04d-%02d-%02d %02d:%02d:%02d.%09d",
            (int)dt->year,
            (int)dt->month,
            (int)dt->day,
            (int)dt->hour,
            (int)dt->minute,
            (int)dt->second,
            (int)dt->nanosecond);
}

static int
ut_is_raw_layout(const uni_tektronix_r3f_header* header) {
    return header->frame_offset_bytes == 0 &&
           header->frame_size_bytes == 0 &&
           header->sample_offset_bytes == 0 &&
           header->samples_per_frame == 0 &&
           header->non_sample_offset_bytes == 0 &&
           header->non_sample_size_bytes == 0;
}

static int
ut_command_info(int argc, char** argv) {
    ut_source source;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (argc != 3) {
        ut_print_help(stderr);
        return 2;
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "info: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }

    printf("type: %s\n", ut_kind_name(source.kind));
    printf("layout: %s\n", ut_is_raw_layout(&source.header) ? "raw" : "framed");
    printf("sample-count: %llu\n", (unsigned long long)source.sample_count);
    if (source.kind == UT_INPUT_R3F) {
        uint64_t frame_count = 0u;
        if (uni_tektronix_r3f_reader_get_frame_count(source.handle.r3f, &frame_count) == UNI_TEKTRONIX_R3F_STATUS_OK) {
            printf("frame-count: %llu\n", (unsigned long long)frame_count);
        }
    }
    printf("sample-rate-sps: %.6f\n", source.header.sample_rate_sps);
    if (source.header.sample_rate_sps > 0.0) {
        printf("duration-seconds: %.9f\n", (double)source.sample_count / source.header.sample_rate_sps);
    }
    printf("rf-center-hz: %.3f\n", source.header.rf_center_frequency_hz);
    printf("if-center-hz: %.3f\n", source.header.if_center_frequency_hz);
    printf("bandwidth-hz: %.3f\n", source.header.bandwidth_hz);
    printf("reference-level-dbm: %.3f\n", source.header.reference_level_dbm);
    printf("start-sample-count: %llu\n", (unsigned long long)source.header.start_sample_count);
    printf("ref-sample-count: %llu\n", (unsigned long long)source.header.ref_sample_count);
    printf("device-serial: %s\n", source.header.device_serial_number);
    printf("device-name: %s\n", source.header.device_nomenclature);
    printf("start-wall-time: ");
    ut_print_datetime(stdout, &source.header.start_wall_time);
    printf("\nref-wall-time: ");
    ut_print_datetime(stdout, &source.header.ref_wall_time);
    printf("\n");

    ut_source_close(&source);
    return 0;
}

static int
ut_command_validate(int argc, char** argv) {
    ut_source source;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (argc != 3) {
        ut_print_help(stderr);
        return 2;
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "validate: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }

    printf("OK %s samples=%llu\n", ut_kind_name(source.kind), (unsigned long long)source.sample_count);
    ut_source_close(&source);
    return 0;
}

static int
ut_command_split(int argc, char** argv) {
    ut_source source;
    uni_tektronix_r3f_header header_copy;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;
    char* output_data_path = NULL;
    char* output_header_path = NULL;

    if (argc != 4) {
        ut_print_help(stderr);
        return 2;
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "split: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }
    if (source.kind != UT_INPUT_R3F) {
        fprintf(stderr, "split: input must be an .r3f file\n");
        ut_source_close(&source);
        return 1;
    }

    if (!ut_resolve_raw_pair_output(argv[3], &output_data_path, &output_header_path)) {
        fprintf(stderr, "split: cannot derive output .r3a/.r3h paths\n");
        ut_source_close(&source);
        return 1;
    }

    header_copy = source.header;
    status = ut_stream_range_to_r3a(&source,
                                    0u,
                                    source.sample_count,
                                    output_data_path,
                                    output_header_path,
                                    &header_copy);
    ut_source_close(&source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "split: failed: %s\n", uni_tektronix_r3f_status_string(status));
        free(output_data_path);
        free(output_header_path);
        return 1;
    }

    printf("wrote %s and %s\n", output_data_path, output_header_path);
    free(output_data_path);
    free(output_header_path);
    return 0;
}

static int
ut_command_pack(int argc, char** argv) {
    ut_source source;
    uni_tektronix_r3f_header header_copy;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;
    ut_tail_mode tail_mode = UT_TAIL_MODE_PAD;
    int32_t pad_value_i32 = 0;

    if (argc < 4) {
        ut_print_help(stderr);
        return 2;
    }

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--tail-mode") == 0) {
            if (i + 1 >= argc || !ut_parse_tail_mode(argv[i + 1], &tail_mode)) {
                fprintf(stderr, "pack: invalid --tail-mode value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--pad-value") == 0) {
            if (i + 1 >= argc || !ut_parse_i32(argv[i + 1], &pad_value_i32)) {
                fprintf(stderr, "pack: invalid --pad-value value\n");
                return 2;
            }
            ++i;
        } else {
            fprintf(stderr, "pack: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "pack: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }
    if (source.kind != UT_INPUT_R3A) {
        fprintf(stderr, "pack: input must be a raw .r3a/.r3h pair\n");
        ut_source_close(&source);
        return 1;
    }

    header_copy = source.header;
    status = uni_tektronix_r3f_header_set_framed_layout(&header_copy);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "pack: invalid header: %s\n", uni_tektronix_r3f_status_string(status));
        ut_source_close(&source);
        return 1;
    }

    status = ut_stream_range_to_r3f(&source,
                                    0u,
                                    source.sample_count,
                                    argv[3],
                                    &header_copy,
                                    tail_mode,
                                    (int16_t)pad_value_i32);
    ut_source_close(&source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "pack: failed: %s\n", uni_tektronix_r3f_status_string(status));
        return 1;
    }

    printf("wrote %s\n", argv[3]);
    return 0;
}

static int
ut_command_extract_raw(int argc, char** argv) {
    ut_source source;
    uint64_t start = 0u;
    uint64_t count = UINT64_MAX;
    uint64_t resolved_start = 0u;
    uint64_t resolved_count = 0u;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (argc < 4) {
        ut_print_help(stderr);
        return 2;
    }

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--start") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &start)) {
                fprintf(stderr, "extract-raw: invalid --start value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--count") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &count)) {
                fprintf(stderr, "extract-raw: invalid --count value\n");
                return 2;
            }
            ++i;
        } else {
            fprintf(stderr, "extract-raw: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "extract-raw: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }
    if (!ut_resolve_sample_window(&source, start, count, &resolved_start, &resolved_count)) {
        fprintf(stderr, "extract-raw: requested range is out of bounds\n");
        ut_source_close(&source);
        return 1;
    }

    status = ut_stream_range_to_raw_i16_file(&source, resolved_start, resolved_count, argv[3]);
    ut_source_close(&source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "extract-raw: failed: %s\n", uni_tektronix_r3f_status_string(status));
        return 1;
    }

    printf("wrote %s\n", argv[3]);
    return 0;
}

static int
ut_command_trim(int argc, char** argv) {
    ut_source source;
    uni_tektronix_r3f_header header_copy;
    uint64_t start = UINT64_MAX;
    uint64_t count = UINT64_MAX;
    uint64_t resolved_start = 0u;
    uint64_t resolved_count = 0u;
    ut_tail_mode tail_mode = UT_TAIL_MODE_PAD;
    int32_t pad_value_i32 = 0;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;
    char* output_data_path = NULL;
    char* output_header_path = NULL;

    if (argc < 5) {
        ut_print_help(stderr);
        return 2;
    }

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--start") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &start)) {
                fprintf(stderr, "trim: invalid --start value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--count") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &count)) {
                fprintf(stderr, "trim: invalid --count value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--tail-mode") == 0) {
            if (i + 1 >= argc || !ut_parse_tail_mode(argv[i + 1], &tail_mode)) {
                fprintf(stderr, "trim: invalid --tail-mode value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--pad-value") == 0) {
            if (i + 1 >= argc || !ut_parse_i32(argv[i + 1], &pad_value_i32)) {
                fprintf(stderr, "trim: invalid --pad-value value\n");
                return 2;
            }
            ++i;
        } else {
            fprintf(stderr, "trim: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    if (start == UINT64_MAX) {
        fprintf(stderr, "trim: --start is required\n");
        return 2;
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "trim: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }
    if (!ut_resolve_sample_window(&source, start, count, &resolved_start, &resolved_count)) {
        fprintf(stderr, "trim: requested range is out of bounds\n");
        ut_source_close(&source);
        return 1;
    }

    header_copy = source.header;
    ut_adjust_trim_header(&header_copy, resolved_start);

    if (ut_has_extension(argv[3], ".r3f")) {
        status = uni_tektronix_r3f_header_set_framed_layout(&header_copy);
        if (status == UNI_TEKTRONIX_R3F_STATUS_OK) {
            status = ut_stream_range_to_r3f(&source,
                                            resolved_start,
                                            resolved_count,
                                            argv[3],
                                            &header_copy,
                                            tail_mode,
                                            (int16_t)pad_value_i32);
        }
    } else {
        if (!ut_resolve_raw_pair_output(argv[3], &output_data_path, &output_header_path)) {
            fprintf(stderr, "trim: cannot derive output .r3a/.r3h paths\n");
            ut_source_close(&source);
            return 1;
        }
        status = ut_stream_range_to_r3a(&source,
                                        resolved_start,
                                        resolved_count,
                                        output_data_path,
                                        output_header_path,
                                        &header_copy);
    }

    ut_source_close(&source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "trim: failed: %s\n", uni_tektronix_r3f_status_string(status));
        free(output_data_path);
        free(output_header_path);
        return 1;
    }

    if (output_data_path != NULL && output_header_path != NULL) {
        printf("wrote %s and %s\n", output_data_path, output_header_path);
    } else {
        printf("wrote %s\n", argv[3]);
    }
    free(output_data_path);
    free(output_header_path);
    return 0;
}

static void
ut_write_u16_le(unsigned char* dst, uint16_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8u) & 0xffu);
}

static void
ut_write_u32_le(unsigned char* dst, uint32_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8u) & 0xffu);
    dst[2] = (unsigned char)((value >> 16u) & 0xffu);
    dst[3] = (unsigned char)((value >> 24u) & 0xffu);
}

static int
ut_wav_open(ut_wav_writer* writer, const char* path, uint32_t sample_rate_hz) {
    unsigned char header[44u];

    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        return 0;
    }
    writer->sample_rate_hz = sample_rate_hz;

    memcpy(header + 0u, "RIFF", 4u);
    ut_write_u32_le(header + 4u, 36u);
    memcpy(header + 8u, "WAVE", 4u);
    memcpy(header + 12u, "fmt ", 4u);
    ut_write_u32_le(header + 16u, 16u);
    ut_write_u16_le(header + 20u, 1u);
    ut_write_u16_le(header + 22u, 2u);
    ut_write_u32_le(header + 24u, sample_rate_hz);
    ut_write_u32_le(header + 28u, sample_rate_hz * 4u);
    ut_write_u16_le(header + 32u, 4u);
    ut_write_u16_le(header + 34u, 16u);
    memcpy(header + 36u, "data", 4u);
    ut_write_u32_le(header + 40u, 0u);

    if (fwrite(header, 1u, sizeof(header), writer->file) != sizeof(header)) {
        fclose(writer->file);
        writer->file = NULL;
        return 0;
    }
    return 1;
}

static int
ut_wav_write_iq_pairs(ut_wav_writer* writer, const int16_t* interleaved_iq, size_t iq_sample_count) {
    if (iq_sample_count == 0u) {
        return 1;
    }
    if (fwrite(interleaved_iq, sizeof(int16_t) * 2u, iq_sample_count, writer->file) != iq_sample_count) {
        return 0;
    }
    writer->iq_sample_count += (uint64_t)iq_sample_count;
    return 1;
}

static int
ut_wav_close(ut_wav_writer* writer) {
    unsigned char buffer[4u];
    uint64_t data_bytes = writer->iq_sample_count * 4u;
    uint64_t riff_size = 36u + data_bytes;

    if (writer->file == NULL) {
        return 0;
    }
    if (data_bytes > 0xffffffffu || riff_size > 0xffffffffu) {
        fclose(writer->file);
        writer->file = NULL;
        return 0;
    }
    if (fseek(writer->file, 4L, SEEK_SET) != 0) {
        fclose(writer->file);
        writer->file = NULL;
        return 0;
    }
    ut_write_u32_le(buffer, (uint32_t)riff_size);
    if (fwrite(buffer, 1u, sizeof(buffer), writer->file) != sizeof(buffer)) {
        fclose(writer->file);
        writer->file = NULL;
        return 0;
    }
    if (fseek(writer->file, 40L, SEEK_SET) != 0) {
        fclose(writer->file);
        writer->file = NULL;
        return 0;
    }
    ut_write_u32_le(buffer, (uint32_t)data_bytes);
    if (fwrite(buffer, 1u, sizeof(buffer), writer->file) != sizeof(buffer)) {
        fclose(writer->file);
        writer->file = NULL;
        return 0;
    }
    if (fclose(writer->file) != 0) {
        writer->file = NULL;
        return 0;
    }
    writer->file = NULL;
    return 1;
}

static double
ut_sinc(double x) {
    if (fabs(x) < 1e-12) {
        return 1.0;
    }
    return sin(UT_PI * x) / (UT_PI * x);
}

static int
ut_ddc_init(ut_ddc_state* state,
            double sample_rate_sps,
            double if_frequency_hz,
            uint32_t decimate,
            double gain,
            double bandwidth_hz,
            uint64_t start_sample_index) {
    const double output_sample_rate = sample_rate_sps / (double)decimate;
    const double half_bandwidth = bandwidth_hz > 0.0 ? (bandwidth_hz * 0.5) : (output_sample_rate * 0.45);
    double cutoff_hz = half_bandwidth;
    double coeff_sum = 0.0;

    if (sample_rate_sps <= 0.0 || decimate == 0u) {
        return 0;
    }
    if (cutoff_hz <= 0.0 || cutoff_hz > (output_sample_rate * 0.45)) {
        cutoff_hz = output_sample_rate * 0.45;
    }
    if (cutoff_hz >= sample_rate_sps * 0.5) {
        cutoff_hz = sample_rate_sps * 0.49;
    }

    memset(state, 0, sizeof(*state));
    state->sample_rate_sps = sample_rate_sps;
    state->if_frequency_hz = if_frequency_hz;
    state->gain = gain;
    state->decimate = decimate;
    state->taps = decimate > 1u ? ((size_t)decimate * 8u + 1u) : 63u;
    if ((state->taps % 2u) == 0u) {
        ++state->taps;
    }
    if (state->taps > 255u) {
        state->taps = 255u;
    }

    state->coeffs = (double*)calloc(state->taps, sizeof(double));
    state->hist_i = (double*)calloc(state->taps, sizeof(double));
    state->hist_q = (double*)calloc(state->taps, sizeof(double));
    if (state->coeffs == NULL || state->hist_i == NULL || state->hist_q == NULL) {
        free(state->coeffs);
        free(state->hist_i);
        free(state->hist_q);
        memset(state, 0, sizeof(*state));
        return 0;
    }

    {
        const double fc = cutoff_hz / sample_rate_sps;
        const double center = (double)(state->taps - 1u) / 2.0;
        for (size_t n = 0u; n < state->taps; ++n) {
            const double x = (double)n - center;
            const double window = 0.54 - 0.46 * cos((2.0 * UT_PI * (double)n) / (double)(state->taps - 1u));
            state->coeffs[n] = 2.0 * fc * ut_sinc(2.0 * fc * x) * window;
            coeff_sum += state->coeffs[n];
        }
    }

    if (fabs(coeff_sum) < 1e-18) {
        free(state->coeffs);
        free(state->hist_i);
        free(state->hist_q);
        memset(state, 0, sizeof(*state));
        return 0;
    }
    for (size_t n = 0u; n < state->taps; ++n) {
        state->coeffs[n] /= coeff_sum;
    }

    state->phase_step = (2.0 * UT_PI * if_frequency_hz) / sample_rate_sps;
    state->phase = fmod(state->phase_step * (double)start_sample_index, 2.0 * UT_PI);
    if (state->phase < 0.0) {
        state->phase += 2.0 * UT_PI;
    }
    return 1;
}

static void
ut_ddc_destroy(ut_ddc_state* state) {
    if (state == NULL) {
        return;
    }
    free(state->coeffs);
    free(state->hist_i);
    free(state->hist_q);
    memset(state, 0, sizeof(*state));
}

static int16_t
ut_clamp_i16(double value) {
    if (value > 32767.0) {
        return 32767;
    }
    if (value < -32768.0) {
        return -32768;
    }
    return (int16_t)lrint(value);
}

static size_t
ut_ddc_process_block(ut_ddc_state* state,
                     const int16_t* input,
                     size_t input_count,
                     int16_t* out_interleaved_iq,
                     size_t out_iq_capacity) {
    size_t out_count = 0u;

    for (size_t sample_index = 0u; sample_index < input_count; ++sample_index) {
        const double x = (double)input[sample_index] * state->gain;
        const double mixed_i = x * cos(state->phase);
        const double mixed_q = -x * sin(state->phase);

        state->hist_i[state->hist_pos] = mixed_i;
        state->hist_q[state->hist_pos] = mixed_q;
        state->hist_pos = (state->hist_pos + 1u) % state->taps;

        if (state->decim_phase == 0u) {
            double acc_i = 0.0;
            double acc_q = 0.0;
            size_t hist_index = state->hist_pos;

            for (size_t tap = 0u; tap < state->taps; ++tap) {
                hist_index = (hist_index == 0u) ? (state->taps - 1u) : (hist_index - 1u);
                acc_i += state->coeffs[tap] * state->hist_i[hist_index];
                acc_q += state->coeffs[tap] * state->hist_q[hist_index];
            }

            if (out_count < out_iq_capacity) {
                out_interleaved_iq[out_count * 2u + 0u] = ut_clamp_i16(acc_i * 2.0);
                out_interleaved_iq[out_count * 2u + 1u] = ut_clamp_i16(acc_q * 2.0);
                ++out_count;
            }
        }

        state->decim_phase = (state->decim_phase + 1u) % state->decimate;
        state->phase += state->phase_step;
        if (state->phase >= 2.0 * UT_PI) {
            state->phase = fmod(state->phase, 2.0 * UT_PI);
        }
    }

    return out_count;
}

static int
ut_command_to_wav(int argc, char** argv) {
    ut_source source;
    uint64_t start = 0u;
    uint64_t count = UINT64_MAX;
    uint64_t resolved_start = 0u;
    uint64_t resolved_count = 0u;
    uint32_t decimate = 1u;
    double gain = 1.0;
    double if_frequency_override = NAN;
    double sample_rate_out = 0.0;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;
    ut_ddc_state ddc;
    ut_wav_writer wav;
    size_t read_chunk_samples = UT_READ_CHUNK_SAMPLES;
    int16_t* read_buffer = NULL;
    int16_t* iq_buffer = NULL;

    if (argc < 4) {
        ut_print_help(stderr);
        return 2;
    }

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--start") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &start)) {
                fprintf(stderr, "to-wav: invalid --start value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--count") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &count)) {
                fprintf(stderr, "to-wav: invalid --count value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--decimate") == 0) {
            if (i + 1 >= argc || !ut_parse_u32(argv[i + 1], &decimate) || decimate == 0u) {
                fprintf(stderr, "to-wav: invalid --decimate value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--gain") == 0) {
            if (i + 1 >= argc || !ut_parse_double(argv[i + 1], &gain)) {
                fprintf(stderr, "to-wav: invalid --gain value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--if-frequency") == 0) {
            if (i + 1 >= argc || !ut_parse_double(argv[i + 1], &if_frequency_override)) {
                fprintf(stderr, "to-wav: invalid --if-frequency value\n");
                return 2;
            }
            ++i;
        } else {
            fprintf(stderr, "to-wav: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    status = ut_source_open(argv[2], &source);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "to-wav: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }
    if (!ut_resolve_sample_window(&source, start, count, &resolved_start, &resolved_count)) {
        fprintf(stderr, "to-wav: requested range is out of bounds\n");
        ut_source_close(&source);
        return 1;
    }
    if (source.header.sample_rate_sps <= 0.0) {
        fprintf(stderr, "to-wav: sample rate is not available in the header\n");
        ut_source_close(&source);
        return 1;
    }
    sample_rate_out = source.header.sample_rate_sps / (double)decimate;
    if (sample_rate_out <= 0.0 || sample_rate_out > 4294967295.0) {
        fprintf(stderr, "to-wav: output sample rate is out of WAV range\n");
        ut_source_close(&source);
        return 1;
    }
    if (source.header.sample_rate_sps / (double)decimate < source.header.bandwidth_hz && source.header.bandwidth_hz > 0.0) {
        fprintf(stderr,
                "to-wav: warning: decimated sample rate %.3f Hz is lower than nominal bandwidth %.3f Hz; spectrum will be truncated\n",
                sample_rate_out,
                source.header.bandwidth_hz);
    }

    if (((uint64_t)decimate * (uint64_t)UT_WAV_BUFFER_IQ_SAMPLES) < (uint64_t)read_chunk_samples) {
        read_chunk_samples = (size_t)((uint64_t)decimate * (uint64_t)UT_WAV_BUFFER_IQ_SAMPLES);
        if (read_chunk_samples == 0u) {
            read_chunk_samples = 1u;
        }
    }

    if (!ut_ddc_init(&ddc,
                     source.header.sample_rate_sps,
                     isnan(if_frequency_override) ? source.header.if_center_frequency_hz : if_frequency_override,
                     decimate,
                     gain,
                     source.header.bandwidth_hz,
                     resolved_start)) {
        fprintf(stderr, "to-wav: failed to initialize DDC\n");
        ut_source_close(&source);
        return 1;
    }

    if (!ut_wav_open(&wav, argv[3], (uint32_t)llround(sample_rate_out))) {
        fprintf(stderr, "to-wav: cannot create '%s'\n", argv[3]);
        ut_ddc_destroy(&ddc);
        ut_source_close(&source);
        return 1;
    }

    read_buffer = (int16_t*)malloc(read_chunk_samples * sizeof(int16_t));
    iq_buffer = (int16_t*)malloc(UT_WAV_BUFFER_IQ_SAMPLES * 2u * sizeof(int16_t));
    if (read_buffer == NULL || iq_buffer == NULL) {
        fprintf(stderr, "to-wav: out of memory\n");
        free(read_buffer);
        free(iq_buffer);
        ut_wav_close(&wav);
        ut_ddc_destroy(&ddc);
        ut_source_close(&source);
        return 1;
    }

    {
        uint64_t remaining = resolved_count;
        uint64_t cursor = resolved_start;
        while (remaining > 0u) {
            const size_t want = remaining > read_chunk_samples ? read_chunk_samples : (size_t)remaining;
            size_t got = 0u;
            size_t produced = 0u;

            status = ut_source_read_samples(&source, cursor, want, read_buffer, read_chunk_samples, &got);
            if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
                fprintf(stderr, "to-wav: read failed: %s\n", uni_tektronix_r3f_status_string(status));
                free(read_buffer);
                free(iq_buffer);
                ut_wav_close(&wav);
                ut_ddc_destroy(&ddc);
                ut_source_close(&source);
                return 1;
            }
            if (got == 0u) {
                break;
            }

            produced = ut_ddc_process_block(&ddc,
                                            read_buffer,
                                            got,
                                            iq_buffer,
                                            UT_WAV_BUFFER_IQ_SAMPLES);
            if (!ut_wav_write_iq_pairs(&wav, iq_buffer, produced)) {
                fprintf(stderr, "to-wav: write failed\n");
                free(read_buffer);
                free(iq_buffer);
                ut_wav_close(&wav);
                ut_ddc_destroy(&ddc);
                ut_source_close(&source);
                return 1;
            }

            cursor += (uint64_t)got;
            remaining -= (uint64_t)got;
        }
    }

    free(read_buffer);
    free(iq_buffer);
    ut_ddc_destroy(&ddc);
    ut_source_close(&source);
    if (!ut_wav_close(&wav)) {
        fprintf(stderr, "to-wav: finalizing WAV failed (the file may be too large for classic RIFF/WAV)\n");
        return 1;
    }

    printf("wrote %s\n", argv[3]);
    return 0;
}

static int
ut_command_export_footers(int argc, char** argv) {
    uni_tektronix_r3f_reader* reader = NULL;
    uint64_t frame_count = 0u;
    uint64_t start_frame = 0u;
    uint64_t count = UINT64_MAX;
    uint64_t resolved_count = 0u;
    FILE* csv = NULL;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (argc < 4) {
        ut_print_help(stderr);
        return 2;
    }
    if (!ut_has_extension(argv[2], ".r3f")) {
        fprintf(stderr, "export-footers: input must be an .r3f file\n");
        return 1;
    }

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--start-frame") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &start_frame)) {
                fprintf(stderr, "export-footers: invalid --start-frame value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--count") == 0) {
            if (i + 1 >= argc || !ut_parse_u64(argv[i + 1], &count)) {
                fprintf(stderr, "export-footers: invalid --count value\n");
                return 2;
            }
            ++i;
        } else {
            fprintf(stderr, "export-footers: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    status = uni_tektronix_r3f_reader_open(argv[2], &reader);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "export-footers: cannot open '%s': %s\n", argv[2], uni_tektronix_r3f_status_string(status));
        return 1;
    }
    status = uni_tektronix_r3f_reader_get_frame_count(reader, &frame_count);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fprintf(stderr, "export-footers: cannot get frame count: %s\n", uni_tektronix_r3f_status_string(status));
        uni_tektronix_r3f_reader_close(reader);
        return 1;
    }
    if (start_frame > frame_count || (count != UINT64_MAX && count > (frame_count - start_frame))) {
        fprintf(stderr, "export-footers: requested range is out of bounds\n");
        uni_tektronix_r3f_reader_close(reader);
        return 1;
    }
    resolved_count = (count == UINT64_MAX) ? (frame_count - start_frame) : count;

    csv = fopen(argv[3], "w");
    if (csv == NULL) {
        fprintf(stderr, "export-footers: cannot create '%s'\n", argv[3]);
        uni_tektronix_r3f_reader_close(reader);
        return 1;
    }

    fprintf(csv,
            "frame_index,frame_id,trigger2_index,trigger1_index,time_sync_index,frame_status,timestamp,reserved0,reserved1,reserved2,reserved3\n");
    for (uint64_t i = 0u; i < resolved_count; ++i) {
        uni_tektronix_r3f_frame_footer footer;
        status = uni_tektronix_r3f_reader_read_frame_footer(reader, start_frame + i, &footer);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            fprintf(stderr, "export-footers: read failed: %s\n", uni_tektronix_r3f_status_string(status));
            fclose(csv);
            uni_tektronix_r3f_reader_close(reader);
            return 1;
        }
        fprintf(csv,
                "%llu,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u\n",
                (unsigned long long)(start_frame + i),
                footer.frame_id,
                (unsigned)footer.trigger2_index,
                (unsigned)footer.trigger1_index,
                (unsigned)footer.time_sync_index,
                (unsigned)footer.frame_status,
                (unsigned long long)footer.timestamp,
                (unsigned)footer.reserved[0],
                (unsigned)footer.reserved[1],
                (unsigned)footer.reserved[2],
                (unsigned)footer.reserved[3]);
    }

    fclose(csv);
    uni_tektronix_r3f_reader_close(reader);
    printf("wrote %s\n", argv[3]);
    return 0;
}

int
main(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        ut_print_help(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        return ut_command_info(argc, argv);
    }
    if (strcmp(argv[1], "validate") == 0) {
        return ut_command_validate(argc, argv);
    }
    if (strcmp(argv[1], "split") == 0 || strcmp(argv[1], "to-raw") == 0) {
        return ut_command_split(argc, argv);
    }
    if (strcmp(argv[1], "pack") == 0 || strcmp(argv[1], "to-r3f") == 0) {
        return ut_command_pack(argc, argv);
    }
    if (strcmp(argv[1], "extract-raw") == 0 || strcmp(argv[1], "to-i16") == 0) {
        return ut_command_extract_raw(argc, argv);
    }
    if (strcmp(argv[1], "trim") == 0) {
        return ut_command_trim(argc, argv);
    }
    if (strcmp(argv[1], "to-wav") == 0 || strcmp(argv[1], "wav") == 0) {
        return ut_command_to_wav(argc, argv);
    }
    if (strcmp(argv[1], "export-footers") == 0) {
        return ut_command_export_footers(argc, argv);
    }

    fprintf(stderr, "unknown command '%s'\n", argv[1]);
    ut_print_help(stderr);
    return 2;
}
