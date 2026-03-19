#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "uni_tektronix_r3f.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

#define UNI_TEKTRONIX_R3X_EXPECTED_FILE_ID "Tektronix RSA300 Data File"
#define UNI_TEKTRONIX_R3X_EXPECTED_ENDIAN 0x12345678u

struct uni_tektronix_r3a_reader {
    FILE* data_file;
    uni_tektronix_r3f_header header;
    uint64_t file_size;
    uint64_t total_samples;
    size_t bytes_per_sample;
};

struct uni_tektronix_r3a_writer {
    FILE* data_file;
    uni_tektronix_r3f_header header;
    uint64_t written_samples;
};

static size_t
r3x_strnlen_bounded(const char* s, size_t bound) {
    size_t n = 0u;
    if (s == NULL) {
        return 0u;
    }
    while (n < bound && s[n] != '\0') {
        ++n;
    }
    return n;
}

static int
r3x_strcasecmp_suffix(const char* text, const char* suffix) {
    size_t text_len = 0u;
    size_t suffix_len = 0u;

    if (text == NULL || suffix == NULL) {
        return 0;
    }

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (text_len < suffix_len) {
        return 0;
    }

    for (size_t i = 0u; i < suffix_len; ++i) {
        const unsigned char a = (unsigned char)text[text_len - suffix_len + i];
        const unsigned char b = (unsigned char)suffix[i];
        if (tolower(a) != tolower(b)) {
            return 0;
        }
    }
    return 1;
}

static char*
r3x_make_sibling_path(const char* path, const char* desired_ext) {
    const size_t path_len = strlen(path);
    const size_t ext_len = strlen(desired_ext);
    size_t stem_len = path_len;
    char* result = NULL;

    if (r3x_strcasecmp_suffix(path, ".r3a") || r3x_strcasecmp_suffix(path, ".r3h") ||
        r3x_strcasecmp_suffix(path, ".r3f")) {
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

static uint16_t
r3x_read_u16_le(const uint8_t* src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8u);
}

static uint32_t
r3x_read_u32_le(const uint8_t* src) {
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8u) |
           ((uint32_t)src[2] << 16u) |
           ((uint32_t)src[3] << 24u);
}

static int32_t
r3x_read_i32_le(const uint8_t* src) {
    return (int32_t)r3x_read_u32_le(src);
}

static uint64_t
r3x_read_u64_le(const uint8_t* src) {
    return (uint64_t)src[0] |
           ((uint64_t)src[1] << 8u) |
           ((uint64_t)src[2] << 16u) |
           ((uint64_t)src[3] << 24u) |
           ((uint64_t)src[4] << 32u) |
           ((uint64_t)src[5] << 40u) |
           ((uint64_t)src[6] << 48u) |
           ((uint64_t)src[7] << 56u);
}

static float
r3x_read_f32_le(const uint8_t* src) {
    const uint32_t raw = r3x_read_u32_le(src);
    float value = 0.0f;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static double
r3x_read_f64_le(const uint8_t* src) {
    const uint64_t raw = r3x_read_u64_le(src);
    double value = 0.0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void
r3x_write_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8u) & 0xffu);
}

static void
r3x_write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8u) & 0xffu);
    dst[2] = (uint8_t)((value >> 16u) & 0xffu);
    dst[3] = (uint8_t)((value >> 24u) & 0xffu);
}

static void
r3x_write_i32_le(uint8_t* dst, int32_t value) {
    r3x_write_u32_le(dst, (uint32_t)value);
}

static void
r3x_write_u64_le(uint8_t* dst, uint64_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8u) & 0xffu);
    dst[2] = (uint8_t)((value >> 16u) & 0xffu);
    dst[3] = (uint8_t)((value >> 24u) & 0xffu);
    dst[4] = (uint8_t)((value >> 32u) & 0xffu);
    dst[5] = (uint8_t)((value >> 40u) & 0xffu);
    dst[6] = (uint8_t)((value >> 48u) & 0xffu);
    dst[7] = (uint8_t)((value >> 56u) & 0xffu);
}

static void
r3x_write_f32_le(uint8_t* dst, float value) {
    uint32_t raw = 0u;
    memcpy(&raw, &value, sizeof(raw));
    r3x_write_u32_le(dst, raw);
}

static void
r3x_write_f64_le(uint8_t* dst, double value) {
    uint64_t raw = 0u;
    memcpy(&raw, &value, sizeof(raw));
    r3x_write_u64_le(dst, raw);
}

static void
r3x_parse_datetime(const uint8_t* src, uni_tektronix_r3f_datetime* out_time) {
    out_time->year = r3x_read_i32_le(src + 0u);
    out_time->month = r3x_read_i32_le(src + 4u);
    out_time->day = r3x_read_i32_le(src + 8u);
    out_time->hour = r3x_read_i32_le(src + 12u);
    out_time->minute = r3x_read_i32_le(src + 16u);
    out_time->second = r3x_read_i32_le(src + 20u);
    out_time->nanosecond = r3x_read_i32_le(src + 24u);
}

static void
r3x_write_datetime(uint8_t* dst, const uni_tektronix_r3f_datetime* time_value) {
    r3x_write_i32_le(dst + 0u, time_value->year);
    r3x_write_i32_le(dst + 4u, time_value->month);
    r3x_write_i32_le(dst + 8u, time_value->day);
    r3x_write_i32_le(dst + 12u, time_value->hour);
    r3x_write_i32_le(dst + 16u, time_value->minute);
    r3x_write_i32_le(dst + 20u, time_value->second);
    r3x_write_i32_le(dst + 24u, time_value->nanosecond);
}

static void
r3x_copy_string_field(char* dst, size_t dst_size, const uint8_t* src, size_t src_size) {
    size_t copy_len = 0u;

    if (dst_size == 0u) {
        return;
    }

    while (copy_len < src_size && src[copy_len] != 0u) {
        ++copy_len;
    }
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1u;
    }

    if (copy_len > 0u) {
        memcpy(dst, src, copy_len);
    }
    dst[copy_len] = '\0';
}

static void
r3x_write_string_field(uint8_t* dst, size_t field_size, const char* src) {
    const size_t copy_len = r3x_strnlen_bounded(src, field_size == 0u ? 0u : field_size - 1u);
    memset(dst, 0, field_size);
    if (copy_len > 0u) {
        memcpy(dst, src, copy_len);
    }
}

static void
r3x_parse_header_bytes(const uint8_t* bytes, uni_tektronix_r3f_header* header) {
    memset(header, 0, sizeof(*header));

    r3x_copy_string_field(header->file_id, sizeof(header->file_id), bytes + 0u, 27u);
    header->endian_check = r3x_read_u32_le(bytes + 512u);
    memcpy(header->file_format_version, bytes + 516u, sizeof(header->file_format_version));
    memcpy(header->api_sw_version, bytes + 520u, sizeof(header->api_sw_version));
    memcpy(header->fx3_fw_version, bytes + 524u, sizeof(header->fx3_fw_version));
    memcpy(header->fpga_fw_version, bytes + 528u, sizeof(header->fpga_fw_version));
    r3x_copy_string_field(header->device_serial_number,
                          sizeof(header->device_serial_number),
                          bytes + 532u,
                          64u);
    r3x_copy_string_field(header->device_nomenclature,
                          sizeof(header->device_nomenclature),
                          bytes + 596u,
                          32u);

    header->reference_level_dbm = r3x_read_f64_le(bytes + 1024u);
    header->rf_center_frequency_hz = r3x_read_f64_le(bytes + 1032u);
    header->device_temperature_c = r3x_read_f64_le(bytes + 1040u);
    header->alignment_state = r3x_read_u32_le(bytes + 1048u);
    header->frequency_reference_state = r3x_read_u32_le(bytes + 1052u);
    header->trigger_mode = r3x_read_u32_le(bytes + 1056u);
    header->trigger_source = r3x_read_u32_le(bytes + 1060u);
    header->trigger_transition = r3x_read_u32_le(bytes + 1064u);
    header->trigger_level_dbm = r3x_read_f64_le(bytes + 1068u);

    header->file_data_type = r3x_read_u32_le(bytes + 2048u);
    header->frame_offset_bytes = r3x_read_i32_le(bytes + 2052u);
    header->frame_size_bytes = r3x_read_i32_le(bytes + 2056u);
    header->sample_offset_bytes = r3x_read_i32_le(bytes + 2060u);
    header->samples_per_frame = r3x_read_i32_le(bytes + 2064u);
    header->non_sample_offset_bytes = r3x_read_i32_le(bytes + 2068u);
    header->non_sample_size_bytes = r3x_read_i32_le(bytes + 2072u);
    header->if_center_frequency_hz = r3x_read_f64_le(bytes + 2076u);
    header->sample_rate_sps = r3x_read_f64_le(bytes + 2084u);
    header->bandwidth_hz = r3x_read_f64_le(bytes + 2092u);
    header->data_corrected = r3x_read_u32_le(bytes + 2100u);
    header->ref_time_type = r3x_read_u32_le(bytes + 2104u);
    r3x_parse_datetime(bytes + 2108u, &header->ref_wall_time);
    header->ref_sample_count = r3x_read_u64_le(bytes + 2136u);
    header->ref_sample_ticks_per_second = r3x_read_u64_le(bytes + 2144u);
    r3x_parse_datetime(bytes + 2152u, &header->ref_utc_time);
    header->ref_time_source = r3x_read_u32_le(bytes + 2180u);
    header->start_sample_count = r3x_read_u64_le(bytes + 2184u);
    r3x_parse_datetime(bytes + 2192u, &header->start_wall_time);

    header->sample_gain_scaling_factor = r3x_read_f64_le(bytes + 3072u);
    header->signal_path_delay_seconds = r3x_read_f64_le(bytes + 3080u);
    header->correction.correction_type = r3x_read_u32_le(bytes + 4096u);
    header->correction.table_entry_count = r3x_read_u32_le(bytes + 4352u);

    for (size_t i = 0u; i < UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES; ++i) {
        header->correction.frequency_hz[i] = r3x_read_f32_le(bytes + 4356u + (i * 4u));
        header->correction.amplitude_db[i] = r3x_read_f32_le(bytes + 6360u + (i * 4u));
        header->correction.phase_deg[i] = r3x_read_f32_le(bytes + 8364u + (i * 4u));
    }
}

static void
r3x_build_header_bytes(const uni_tektronix_r3f_header* header, uint8_t* bytes) {
    memset(bytes, 0, UNI_TEKTRONIX_R3F_HEADER_SIZE);

    r3x_write_string_field(bytes + 0u, 27u, UNI_TEKTRONIX_R3X_EXPECTED_FILE_ID);
    r3x_write_u32_le(bytes + 512u, header->endian_check);
    memcpy(bytes + 516u, header->file_format_version, sizeof(header->file_format_version));
    memcpy(bytes + 520u, header->api_sw_version, sizeof(header->api_sw_version));
    memcpy(bytes + 524u, header->fx3_fw_version, sizeof(header->fx3_fw_version));
    memcpy(bytes + 528u, header->fpga_fw_version, sizeof(header->fpga_fw_version));
    r3x_write_string_field(bytes + 532u, 64u, header->device_serial_number);
    r3x_write_string_field(bytes + 596u, 32u, header->device_nomenclature);

    r3x_write_f64_le(bytes + 1024u, header->reference_level_dbm);
    r3x_write_f64_le(bytes + 1032u, header->rf_center_frequency_hz);
    r3x_write_f64_le(bytes + 1040u, header->device_temperature_c);
    r3x_write_u32_le(bytes + 1048u, header->alignment_state);
    r3x_write_u32_le(bytes + 1052u, header->frequency_reference_state);
    r3x_write_u32_le(bytes + 1056u, header->trigger_mode);
    r3x_write_u32_le(bytes + 1060u, header->trigger_source);
    r3x_write_u32_le(bytes + 1064u, header->trigger_transition);
    r3x_write_f64_le(bytes + 1068u, header->trigger_level_dbm);

    r3x_write_u32_le(bytes + 2048u, header->file_data_type);
    r3x_write_i32_le(bytes + 2052u, header->frame_offset_bytes);
    r3x_write_i32_le(bytes + 2056u, header->frame_size_bytes);
    r3x_write_i32_le(bytes + 2060u, header->sample_offset_bytes);
    r3x_write_i32_le(bytes + 2064u, header->samples_per_frame);
    r3x_write_i32_le(bytes + 2068u, header->non_sample_offset_bytes);
    r3x_write_i32_le(bytes + 2072u, header->non_sample_size_bytes);
    r3x_write_f64_le(bytes + 2076u, header->if_center_frequency_hz);
    r3x_write_f64_le(bytes + 2084u, header->sample_rate_sps);
    r3x_write_f64_le(bytes + 2092u, header->bandwidth_hz);
    r3x_write_u32_le(bytes + 2100u, header->data_corrected);
    r3x_write_u32_le(bytes + 2104u, header->ref_time_type);
    r3x_write_datetime(bytes + 2108u, &header->ref_wall_time);
    r3x_write_u64_le(bytes + 2136u, header->ref_sample_count);
    r3x_write_u64_le(bytes + 2144u, header->ref_sample_ticks_per_second);
    r3x_write_datetime(bytes + 2152u, &header->ref_utc_time);
    r3x_write_u32_le(bytes + 2180u, header->ref_time_source);
    r3x_write_u64_le(bytes + 2184u, header->start_sample_count);
    r3x_write_datetime(bytes + 2192u, &header->start_wall_time);

    r3x_write_f64_le(bytes + 3072u, header->sample_gain_scaling_factor);
    r3x_write_f64_le(bytes + 3080u, header->signal_path_delay_seconds);
    r3x_write_u32_le(bytes + 4096u, header->correction.correction_type);
    r3x_write_u32_le(bytes + 4352u, header->correction.table_entry_count);

    for (size_t i = 0u; i < UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES; ++i) {
        r3x_write_f32_le(bytes + 4356u + (i * 4u), header->correction.frequency_hz[i]);
        r3x_write_f32_le(bytes + 6360u + (i * 4u), header->correction.amplitude_db[i]);
        r3x_write_f32_le(bytes + 8364u + (i * 4u), header->correction.phase_deg[i]);
    }
}

static size_t
r3x_bytes_per_sample(uint32_t file_data_type) {
    if (file_data_type == UNI_TEKTRONIX_R3F_FILE_DATA_TYPE_INT16_IF || file_data_type == 2u) {
        return sizeof(int16_t);
    }
    return 0u;
}

static uni_tektronix_r3f_status
r3x_validate_common_parsed_header(const uni_tektronix_r3f_header* header) {
    if (header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (strncmp(header->file_id, UNI_TEKTRONIX_R3X_EXPECTED_FILE_ID, 27) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    if (header->endian_check != UNI_TEKTRONIX_R3X_EXPECTED_ENDIAN) {
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }
    if (r3x_bytes_per_sample(header->file_data_type) == 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }
    if (header->correction.table_entry_count > UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
r3x_normalize_header_for_write(uni_tektronix_r3f_header* header) {
    if (header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    memcpy(header->file_id, UNI_TEKTRONIX_R3X_EXPECTED_FILE_ID, 27u);
    header->file_id[27] = '\0';
    if (header->endian_check == 0u) {
        header->endian_check = UNI_TEKTRONIX_R3X_EXPECTED_ENDIAN;
    }
    if (header->endian_check != UNI_TEKTRONIX_R3X_EXPECTED_ENDIAN) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (header->file_format_version[0] == 0u &&
        header->file_format_version[1] == 0u &&
        header->file_format_version[2] == 0u &&
        header->file_format_version[3] == 0u) {
        header->file_format_version[0] = 1u;
        header->file_format_version[1] = 2u;
        header->file_format_version[2] = 0u;
        header->file_format_version[3] = 0u;
    }
    if (header->file_data_type == 0u) {
        header->file_data_type = UNI_TEKTRONIX_R3F_FILE_DATA_TYPE_INT16_IF;
    }
    if (r3x_bytes_per_sample(header->file_data_type) == 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }
    if (header->correction.table_entry_count > UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static int
r3x_seek_abs(FILE* file, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, (__int64)offset, SEEK_SET);
#else
    return fseeko(file, (off_t)offset, SEEK_SET);
#endif
}

static int
r3x_seek_end(FILE* file) {
#if defined(_WIN32)
    return _fseeki64(file, 0, SEEK_END);
#else
    return fseeko(file, (off_t)0, SEEK_END);
#endif
}

static int
r3x_tell(FILE* file, uint64_t* out_offset) {
#if defined(_WIN32)
    const __int64 pos = _ftelli64(file);
    if (pos < 0) {
        return -1;
    }
    *out_offset = (uint64_t)pos;
#else
    const off_t pos = ftello(file);
    if (pos < 0) {
        return -1;
    }
    *out_offset = (uint64_t)pos;
#endif
    return 0;
}

static uni_tektronix_r3f_status
r3x_get_file_size(FILE* file, uint64_t* out_size) {
    if (r3x_seek_end(file) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (r3x_tell(file, out_size) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (r3x_seek_abs(file, 0u) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
r3x_write_i16_le_samples(FILE* file, const int16_t* samples, size_t sample_count) {
    uint8_t buffer[4096u * sizeof(int16_t)];
    size_t offset = 0u;

    while (offset < sample_count) {
        const size_t chunk = (sample_count - offset) > 4096u ? 4096u : (sample_count - offset);
        for (size_t i = 0u; i < chunk; ++i) {
            r3x_write_u16_le(buffer + (i * 2u), (uint16_t)samples[offset + i]);
        }
        if (fwrite(buffer, sizeof(int16_t), chunk, file) != chunk) {
            return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
        }
        offset += chunk;
    }

    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
r3x_read_i16_le_samples(FILE* file, int16_t* out_samples, size_t sample_count) {
    uint8_t buffer[4096u * sizeof(int16_t)];
    size_t offset = 0u;

    while (offset < sample_count) {
        const size_t chunk = (sample_count - offset) > 4096u ? 4096u : (sample_count - offset);
        if (fread(buffer, sizeof(int16_t), chunk, file) != chunk) {
            return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
        }
        for (size_t i = 0u; i < chunk; ++i) {
            out_samples[offset + i] = (int16_t)r3x_read_u16_le(buffer + (i * 2u));
        }
        offset += chunk;
    }

    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_header_read_file(const char* path, uni_tektronix_r3f_header* out_header) {
    uint8_t bytes[UNI_TEKTRONIX_R3F_HEADER_SIZE];
    FILE* file = NULL;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (path == NULL || out_header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (fread(bytes, 1u, sizeof(bytes), file) != sizeof(bytes)) {
        fclose(file);
        return UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE;
    }
    fclose(file);

    r3x_parse_header_bytes(bytes, out_header);
    status = r3x_validate_common_parsed_header(out_header);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return status;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_header_write_file(const char* path, const uni_tektronix_r3f_header* header) {
    uni_tektronix_r3f_header header_copy;
    uint8_t bytes[UNI_TEKTRONIX_R3F_HEADER_SIZE];
    FILE* file = NULL;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (path == NULL || header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    header_copy = *header;
    status = r3x_normalize_header_for_write(&header_copy);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return status;
    }

    r3x_build_header_bytes(&header_copy, bytes);

    file = fopen(path, "wb");
    if (file == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (fwrite(bytes, 1u, sizeof(bytes), file) != sizeof(bytes)) {
        fclose(file);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (fclose(file) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_header_set_framed_layout(uni_tektronix_r3f_header* header) {
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    status = r3x_normalize_header_for_write(header);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return status;
    }

    header->frame_offset_bytes = (int32_t)UNI_TEKTRONIX_R3F_HEADER_SIZE;
    header->frame_size_bytes = (int32_t)UNI_TEKTRONIX_R3F_STANDARD_FRAME_SIZE;
    header->sample_offset_bytes = 0;
    header->samples_per_frame = (int32_t)UNI_TEKTRONIX_R3F_STANDARD_SAMPLES_PER_FRAME;
    header->non_sample_offset_bytes = 16356;
    header->non_sample_size_bytes = (int32_t)UNI_TEKTRONIX_R3F_STANDARD_FOOTER_SIZE;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_header_set_raw_layout(uni_tektronix_r3f_header* header) {
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    status = r3x_normalize_header_for_write(header);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return status;
    }

    header->frame_offset_bytes = 0;
    header->frame_size_bytes = 0;
    header->sample_offset_bytes = 0;
    header->samples_per_frame = 0;
    header->non_sample_offset_bytes = 0;
    header->non_sample_size_bytes = 0;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3a_reader_open(const char* data_path,
                              const char* header_path,
                              uni_tektronix_r3a_reader** out_reader) {
    uni_tektronix_r3a_reader* reader = NULL;
    char* inferred_data_path = NULL;
    char* inferred_header_path = NULL;
    const char* resolved_data_path = NULL;
    const char* resolved_header_path = NULL;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (data_path == NULL || out_reader == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_reader = NULL;

    if (header_path != NULL) {
        resolved_data_path = data_path;
        resolved_header_path = header_path;
    } else if (r3x_strcasecmp_suffix(data_path, ".r3h")) {
        resolved_header_path = data_path;
        inferred_data_path = r3x_make_sibling_path(data_path, ".r3a");
        if (inferred_data_path == NULL) {
            return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
        }
        resolved_data_path = inferred_data_path;
    } else if (r3x_strcasecmp_suffix(data_path, ".r3a")) {
        resolved_data_path = data_path;
        inferred_header_path = r3x_make_sibling_path(data_path, ".r3h");
        if (inferred_header_path == NULL) {
            return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
        }
        resolved_header_path = inferred_header_path;
    } else {
        inferred_data_path = r3x_make_sibling_path(data_path, ".r3a");
        inferred_header_path = r3x_make_sibling_path(data_path, ".r3h");
        if (inferred_data_path == NULL || inferred_header_path == NULL) {
            free(inferred_data_path);
            free(inferred_header_path);
            return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
        }
        resolved_data_path = inferred_data_path;
        resolved_header_path = inferred_header_path;
    }

    reader = (uni_tektronix_r3a_reader*)calloc(1u, sizeof(*reader));
    if (reader == NULL) {
        free(inferred_data_path);
        free(inferred_header_path);
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    status = uni_tektronix_r3f_header_read_file(resolved_header_path, &reader->header);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        free(reader);
        free(inferred_data_path);
        free(inferred_header_path);
        return status;
    }

    reader->bytes_per_sample = r3x_bytes_per_sample(reader->header.file_data_type);
    if (reader->bytes_per_sample != sizeof(int16_t)) {
        free(reader);
        free(inferred_data_path);
        free(inferred_header_path);
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }

    reader->data_file = fopen(resolved_data_path, "rb");
    if (reader->data_file == NULL) {
        free(reader);
        free(inferred_data_path);
        free(inferred_header_path);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    status = r3x_get_file_size(reader->data_file, &reader->file_size);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        uni_tektronix_r3a_reader_close(reader);
        free(inferred_data_path);
        free(inferred_header_path);
        return status;
    }
    if (reader->file_size % (uint64_t)reader->bytes_per_sample != 0u) {
        uni_tektronix_r3a_reader_close(reader);
        free(inferred_data_path);
        free(inferred_header_path);
        return UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE;
    }
    reader->total_samples = reader->file_size / (uint64_t)reader->bytes_per_sample;

    *out_reader = reader;
    free(inferred_data_path);
    free(inferred_header_path);
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

void
uni_tektronix_r3a_reader_close(uni_tektronix_r3a_reader* reader) {
    if (reader == NULL) {
        return;
    }
    if (reader->data_file != NULL) {
        fclose(reader->data_file);
    }
    free(reader);
}

uni_tektronix_r3f_status
uni_tektronix_r3a_reader_get_header(const uni_tektronix_r3a_reader* reader,
                                    uni_tektronix_r3f_header* out_header) {
    if (reader == NULL || out_header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_header = reader->header;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3a_reader_get_sample_count(const uni_tektronix_r3a_reader* reader,
                                          uint64_t* out_sample_count) {
    if (reader == NULL || out_sample_count == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_sample_count = reader->total_samples;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3a_reader_read_samples_i16(uni_tektronix_r3a_reader* reader,
                                          uint64_t start_sample_index,
                                          size_t requested_samples,
                                          int16_t* out_samples,
                                          size_t out_capacity,
                                          size_t* out_samples_read) {
    size_t limit = 0u;
    uint64_t available = 0u;
    const uint64_t file_offset = start_sample_index * (uint64_t)reader->bytes_per_sample;

    if (reader == NULL || out_samples_read == NULL || (requested_samples > 0u && out_samples == NULL)) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    *out_samples_read = 0u;
    if (requested_samples == 0u || out_capacity == 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_OK;
    }
    if (start_sample_index > reader->total_samples) {
        return UNI_TEKTRONIX_R3F_STATUS_OUT_OF_RANGE;
    }
    if (start_sample_index == reader->total_samples) {
        return UNI_TEKTRONIX_R3F_STATUS_OK;
    }

    available = reader->total_samples - start_sample_index;
    limit = requested_samples;
    if ((uint64_t)limit > available) {
        limit = (size_t)available;
    }
    if (limit > out_capacity) {
        limit = out_capacity;
    }

    if (r3x_seek_abs(reader->data_file, file_offset) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (r3x_read_i16_le_samples(reader->data_file, out_samples, limit) != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    *out_samples_read = limit;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3a_writer_create(const char* data_path,
                                const char* header_path,
                                const uni_tektronix_r3f_header* header,
                                uni_tektronix_r3a_writer** out_writer) {
    uni_tektronix_r3a_writer* writer = NULL;
    char* inferred_data_path = NULL;
    char* inferred_header_path = NULL;
    const char* resolved_data_path = NULL;
    const char* resolved_header_path = NULL;
    uni_tektronix_r3f_header header_copy;
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (data_path == NULL || header == NULL || out_writer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_writer = NULL;

    if (header_path != NULL) {
        resolved_data_path = data_path;
        resolved_header_path = header_path;
    } else if (r3x_strcasecmp_suffix(data_path, ".r3h")) {
        resolved_header_path = data_path;
        inferred_data_path = r3x_make_sibling_path(data_path, ".r3a");
        if (inferred_data_path == NULL) {
            return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
        }
        resolved_data_path = inferred_data_path;
    } else if (r3x_strcasecmp_suffix(data_path, ".r3a")) {
        resolved_data_path = data_path;
        inferred_header_path = r3x_make_sibling_path(data_path, ".r3h");
        if (inferred_header_path == NULL) {
            return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
        }
        resolved_header_path = inferred_header_path;
    } else {
        inferred_data_path = r3x_make_sibling_path(data_path, ".r3a");
        inferred_header_path = r3x_make_sibling_path(data_path, ".r3h");
        if (inferred_data_path == NULL || inferred_header_path == NULL) {
            free(inferred_data_path);
            free(inferred_header_path);
            return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
        }
        resolved_data_path = inferred_data_path;
        resolved_header_path = inferred_header_path;
    }

    writer = (uni_tektronix_r3a_writer*)calloc(1u, sizeof(*writer));
    if (writer == NULL) {
        free(inferred_data_path);
        free(inferred_header_path);
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    header_copy = *header;
    status = uni_tektronix_r3f_header_set_raw_layout(&header_copy);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        free(writer);
        free(inferred_data_path);
        free(inferred_header_path);
        return status;
    }

    status = uni_tektronix_r3f_header_write_file(resolved_header_path, &header_copy);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        free(writer);
        free(inferred_data_path);
        free(inferred_header_path);
        return status;
    }

    writer->data_file = fopen(resolved_data_path, "wb");
    if (writer->data_file == NULL) {
        free(writer);
        free(inferred_data_path);
        free(inferred_header_path);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    writer->header = header_copy;

    *out_writer = writer;
    free(inferred_data_path);
    free(inferred_header_path);
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3a_writer_append_samples_i16(uni_tektronix_r3a_writer* writer,
                                            const int16_t* samples,
                                            size_t sample_count) {
    uni_tektronix_r3f_status status = UNI_TEKTRONIX_R3F_STATUS_OK;

    if (writer == NULL || (sample_count > 0u && samples == NULL)) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (sample_count == 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_OK;
    }

    status = r3x_write_i16_le_samples(writer->data_file, samples, sample_count);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return status;
    }
    writer->written_samples += (uint64_t)sample_count;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3a_writer_close(uni_tektronix_r3a_writer* writer) {
    int close_status = 0;

    if (writer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (fflush(writer->data_file) != 0) {
        uni_tektronix_r3a_writer_discard(writer);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    close_status = fclose(writer->data_file);
    writer->data_file = NULL;
    free(writer);
    return close_status == 0 ? UNI_TEKTRONIX_R3F_STATUS_OK
                             : UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
}

void
uni_tektronix_r3a_writer_discard(uni_tektronix_r3a_writer* writer) {
    if (writer == NULL) {
        return;
    }
    if (writer->data_file != NULL) {
        fclose(writer->data_file);
    }
    free(writer);
}
