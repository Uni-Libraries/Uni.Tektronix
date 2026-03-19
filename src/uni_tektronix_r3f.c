#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "uni_tektronix_r3f.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

#define UNI_TEKTRONIX_R3F_FOOTER_LAYOUT_SIZE 28u
#define UNI_TEKTRONIX_R3F_EXPECTED_FILE_ID "Tektronix RSA300 Data File"
#define UNI_TEKTRONIX_R3F_EXPECTED_ENDIAN 0x12345678u

struct uni_tektronix_r3f_reader {
    FILE* file;
    uni_tektronix_r3f_header header;
    uint64_t file_size;
    uint64_t frame_count;
    uint64_t total_samples;
    size_t bytes_per_sample;
    size_t frame_sample_region_bytes;
    uint8_t* sample_scratch;
};

struct uni_tektronix_r3f_writer {
    FILE* file;
    uni_tektronix_r3f_header header;
    int16_t* sample_buffer;
    size_t buffered_samples;
    uint8_t* frame_scratch;
    uint64_t written_frames;
};

static size_t
r3f_strnlen_bounded(const char* s, size_t bound) {
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (n < bound && s[n] != '\0') {
        ++n;
    }
    return n;
}

static uint16_t
r3f_read_u16_le(const uint8_t* src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8u);
}

static uint32_t
r3f_read_u32_le(const uint8_t* src) {
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8u) |
           ((uint32_t)src[2] << 16u) |
           ((uint32_t)src[3] << 24u);
}

static int32_t
r3f_read_i32_le(const uint8_t* src) {
    return (int32_t)r3f_read_u32_le(src);
}

static uint64_t
r3f_read_u64_le(const uint8_t* src) {
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
r3f_read_f32_le(const uint8_t* src) {
    const uint32_t raw = r3f_read_u32_le(src);
    float value = 0.0f;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static double
r3f_read_f64_le(const uint8_t* src) {
    const uint64_t raw = r3f_read_u64_le(src);
    double value = 0.0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void
r3f_write_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8u) & 0xffu);
}

static void
r3f_write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8u) & 0xffu);
    dst[2] = (uint8_t)((value >> 16u) & 0xffu);
    dst[3] = (uint8_t)((value >> 24u) & 0xffu);
}

static void
r3f_write_i32_le(uint8_t* dst, int32_t value) {
    r3f_write_u32_le(dst, (uint32_t)value);
}

static void
r3f_write_u64_le(uint8_t* dst, uint64_t value) {
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
r3f_write_f32_le(uint8_t* dst, float value) {
    uint32_t raw = 0u;
    memcpy(&raw, &value, sizeof(raw));
    r3f_write_u32_le(dst, raw);
}

static void
r3f_write_f64_le(uint8_t* dst, double value) {
    uint64_t raw = 0u;
    memcpy(&raw, &value, sizeof(raw));
    r3f_write_u64_le(dst, raw);
}

static void
r3f_parse_datetime(const uint8_t* src, uni_tektronix_r3f_datetime* out_time) {
    out_time->year = r3f_read_i32_le(src + 0u);
    out_time->month = r3f_read_i32_le(src + 4u);
    out_time->day = r3f_read_i32_le(src + 8u);
    out_time->hour = r3f_read_i32_le(src + 12u);
    out_time->minute = r3f_read_i32_le(src + 16u);
    out_time->second = r3f_read_i32_le(src + 20u);
    out_time->nanosecond = r3f_read_i32_le(src + 24u);
}

static void
r3f_write_datetime(uint8_t* dst, const uni_tektronix_r3f_datetime* time_value) {
    r3f_write_i32_le(dst + 0u, time_value->year);
    r3f_write_i32_le(dst + 4u, time_value->month);
    r3f_write_i32_le(dst + 8u, time_value->day);
    r3f_write_i32_le(dst + 12u, time_value->hour);
    r3f_write_i32_le(dst + 16u, time_value->minute);
    r3f_write_i32_le(dst + 20u, time_value->second);
    r3f_write_i32_le(dst + 24u, time_value->nanosecond);
}

static void
r3f_copy_string_field(char* dst, size_t dst_size, const uint8_t* src, size_t src_size) {
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
r3f_write_string_field(uint8_t* dst, size_t field_size, const char* src) {
    const size_t copy_len = r3f_strnlen_bounded(src, field_size == 0u ? 0u : field_size - 1u);
    memset(dst, 0, field_size);
    if (copy_len > 0u) {
        memcpy(dst, src, copy_len);
    }
}

static size_t
r3f_bytes_per_sample(uint32_t file_data_type) {
    if (file_data_type == UNI_TEKTRONIX_R3F_FILE_DATA_TYPE_INT16_IF || file_data_type == 2u) {
        return sizeof(int16_t);
    }
    return 0u;
}

static int
r3f_seek_abs(FILE* file, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, (__int64)offset, SEEK_SET);
#else
    return fseeko(file, (off_t)offset, SEEK_SET);
#endif
}

static int
r3f_seek_end(FILE* file) {
#if defined(_WIN32)
    return _fseeki64(file, 0, SEEK_END);
#else
    return fseeko(file, (off_t)0, SEEK_END);
#endif
}

static int
r3f_tell(FILE* file, uint64_t* out_offset) {
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
r3f_get_file_size(FILE* file, uint64_t* out_size) {
    if (r3f_seek_end(file) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (r3f_tell(file, out_size) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (r3f_seek_abs(file, 0u) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static void
r3f_parse_footer_bytes(const uint8_t* bytes, uni_tektronix_r3f_frame_footer* footer) {
    footer->reserved[0] = r3f_read_u16_le(bytes + 0u);
    footer->reserved[1] = r3f_read_u16_le(bytes + 2u);
    footer->reserved[2] = r3f_read_u16_le(bytes + 4u);
    footer->reserved[3] = r3f_read_u16_le(bytes + 6u);
    footer->frame_id = r3f_read_u32_le(bytes + 8u);
    footer->trigger2_index = r3f_read_u16_le(bytes + 12u);
    footer->trigger1_index = r3f_read_u16_le(bytes + 14u);
    footer->time_sync_index = r3f_read_u16_le(bytes + 16u);
    footer->frame_status = r3f_read_u16_le(bytes + 18u);
    footer->timestamp = r3f_read_u64_le(bytes + 20u);
}

static void
r3f_write_footer_bytes(uint8_t* bytes, const uni_tektronix_r3f_frame_footer* footer) {
    r3f_write_u16_le(bytes + 0u, footer->reserved[0]);
    r3f_write_u16_le(bytes + 2u, footer->reserved[1]);
    r3f_write_u16_le(bytes + 4u, footer->reserved[2]);
    r3f_write_u16_le(bytes + 6u, footer->reserved[3]);
    r3f_write_u32_le(bytes + 8u, footer->frame_id);
    r3f_write_u16_le(bytes + 12u, footer->trigger2_index);
    r3f_write_u16_le(bytes + 14u, footer->trigger1_index);
    r3f_write_u16_le(bytes + 16u, footer->time_sync_index);
    r3f_write_u16_le(bytes + 18u, footer->frame_status);
    r3f_write_u64_le(bytes + 20u, footer->timestamp);
}

static void
r3f_parse_header_bytes(const uint8_t* bytes, uni_tektronix_r3f_header* header) {
    memset(header, 0, sizeof(*header));

    r3f_copy_string_field(header->file_id, sizeof(header->file_id), bytes + 0u, 27u);
    header->endian_check = r3f_read_u32_le(bytes + 512u);
    memcpy(header->file_format_version, bytes + 516u, sizeof(header->file_format_version));
    memcpy(header->api_sw_version, bytes + 520u, sizeof(header->api_sw_version));
    memcpy(header->fx3_fw_version, bytes + 524u, sizeof(header->fx3_fw_version));
    memcpy(header->fpga_fw_version, bytes + 528u, sizeof(header->fpga_fw_version));
    r3f_copy_string_field(header->device_serial_number,
                          sizeof(header->device_serial_number),
                          bytes + 532u,
                          64u);
    r3f_copy_string_field(header->device_nomenclature,
                          sizeof(header->device_nomenclature),
                          bytes + 596u,
                          32u);

    header->reference_level_dbm = r3f_read_f64_le(bytes + 1024u);
    header->rf_center_frequency_hz = r3f_read_f64_le(bytes + 1032u);
    header->device_temperature_c = r3f_read_f64_le(bytes + 1040u);
    header->alignment_state = r3f_read_u32_le(bytes + 1048u);
    header->frequency_reference_state = r3f_read_u32_le(bytes + 1052u);
    header->trigger_mode = r3f_read_u32_le(bytes + 1056u);
    header->trigger_source = r3f_read_u32_le(bytes + 1060u);
    header->trigger_transition = r3f_read_u32_le(bytes + 1064u);
    header->trigger_level_dbm = r3f_read_f64_le(bytes + 1068u);

    header->file_data_type = r3f_read_u32_le(bytes + 2048u);
    header->frame_offset_bytes = r3f_read_i32_le(bytes + 2052u);
    header->frame_size_bytes = r3f_read_i32_le(bytes + 2056u);
    header->sample_offset_bytes = r3f_read_i32_le(bytes + 2060u);
    header->samples_per_frame = r3f_read_i32_le(bytes + 2064u);
    header->non_sample_offset_bytes = r3f_read_i32_le(bytes + 2068u);
    header->non_sample_size_bytes = r3f_read_i32_le(bytes + 2072u);
    header->if_center_frequency_hz = r3f_read_f64_le(bytes + 2076u);
    header->sample_rate_sps = r3f_read_f64_le(bytes + 2084u);
    header->bandwidth_hz = r3f_read_f64_le(bytes + 2092u);
    header->data_corrected = r3f_read_u32_le(bytes + 2100u);
    header->ref_time_type = r3f_read_u32_le(bytes + 2104u);
    r3f_parse_datetime(bytes + 2108u, &header->ref_wall_time);
    header->ref_sample_count = r3f_read_u64_le(bytes + 2136u);
    header->ref_sample_ticks_per_second = r3f_read_u64_le(bytes + 2144u);
    r3f_parse_datetime(bytes + 2152u, &header->ref_utc_time);
    header->ref_time_source = r3f_read_u32_le(bytes + 2180u);
    header->start_sample_count = r3f_read_u64_le(bytes + 2184u);
    r3f_parse_datetime(bytes + 2192u, &header->start_wall_time);

    header->sample_gain_scaling_factor = r3f_read_f64_le(bytes + 3072u);
    header->signal_path_delay_seconds = r3f_read_f64_le(bytes + 3080u);
    header->correction.correction_type = r3f_read_u32_le(bytes + 4096u);
    header->correction.table_entry_count = r3f_read_u32_le(bytes + 4352u);

    for (size_t i = 0u; i < UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES; ++i) {
        header->correction.frequency_hz[i] = r3f_read_f32_le(bytes + 4356u + (i * 4u));
        header->correction.amplitude_db[i] = r3f_read_f32_le(bytes + 6360u + (i * 4u));
        header->correction.phase_deg[i] = r3f_read_f32_le(bytes + 8364u + (i * 4u));
    }
}

static void
r3f_build_header_bytes(const uni_tektronix_r3f_header* header, uint8_t* bytes) {
    memset(bytes, 0, UNI_TEKTRONIX_R3F_HEADER_SIZE);

    r3f_write_string_field(bytes + 0u, 27u, UNI_TEKTRONIX_R3F_EXPECTED_FILE_ID);
    r3f_write_u32_le(bytes + 512u, header->endian_check);
    memcpy(bytes + 516u, header->file_format_version, sizeof(header->file_format_version));
    memcpy(bytes + 520u, header->api_sw_version, sizeof(header->api_sw_version));
    memcpy(bytes + 524u, header->fx3_fw_version, sizeof(header->fx3_fw_version));
    memcpy(bytes + 528u, header->fpga_fw_version, sizeof(header->fpga_fw_version));
    r3f_write_string_field(bytes + 532u, 64u, header->device_serial_number);
    r3f_write_string_field(bytes + 596u, 32u, header->device_nomenclature);

    r3f_write_f64_le(bytes + 1024u, header->reference_level_dbm);
    r3f_write_f64_le(bytes + 1032u, header->rf_center_frequency_hz);
    r3f_write_f64_le(bytes + 1040u, header->device_temperature_c);
    r3f_write_u32_le(bytes + 1048u, header->alignment_state);
    r3f_write_u32_le(bytes + 1052u, header->frequency_reference_state);
    r3f_write_u32_le(bytes + 1056u, header->trigger_mode);
    r3f_write_u32_le(bytes + 1060u, header->trigger_source);
    r3f_write_u32_le(bytes + 1064u, header->trigger_transition);
    r3f_write_f64_le(bytes + 1068u, header->trigger_level_dbm);

    r3f_write_u32_le(bytes + 2048u, header->file_data_type);
    r3f_write_i32_le(bytes + 2052u, header->frame_offset_bytes);
    r3f_write_i32_le(bytes + 2056u, header->frame_size_bytes);
    r3f_write_i32_le(bytes + 2060u, header->sample_offset_bytes);
    r3f_write_i32_le(bytes + 2064u, header->samples_per_frame);
    r3f_write_i32_le(bytes + 2068u, header->non_sample_offset_bytes);
    r3f_write_i32_le(bytes + 2072u, header->non_sample_size_bytes);
    r3f_write_f64_le(bytes + 2076u, header->if_center_frequency_hz);
    r3f_write_f64_le(bytes + 2084u, header->sample_rate_sps);
    r3f_write_f64_le(bytes + 2092u, header->bandwidth_hz);
    r3f_write_u32_le(bytes + 2100u, header->data_corrected);
    r3f_write_u32_le(bytes + 2104u, header->ref_time_type);
    r3f_write_datetime(bytes + 2108u, &header->ref_wall_time);
    r3f_write_u64_le(bytes + 2136u, header->ref_sample_count);
    r3f_write_u64_le(bytes + 2144u, header->ref_sample_ticks_per_second);
    r3f_write_datetime(bytes + 2152u, &header->ref_utc_time);
    r3f_write_u32_le(bytes + 2180u, header->ref_time_source);
    r3f_write_u64_le(bytes + 2184u, header->start_sample_count);
    r3f_write_datetime(bytes + 2192u, &header->start_wall_time);

    r3f_write_f64_le(bytes + 3072u, header->sample_gain_scaling_factor);
    r3f_write_f64_le(bytes + 3080u, header->signal_path_delay_seconds);
    r3f_write_u32_le(bytes + 4096u, header->correction.correction_type);
    r3f_write_u32_le(bytes + 4352u, header->correction.table_entry_count);

    for (size_t i = 0u; i < UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES; ++i) {
        r3f_write_f32_le(bytes + 4356u + (i * 4u), header->correction.frequency_hz[i]);
        r3f_write_f32_le(bytes + 6360u + (i * 4u), header->correction.amplitude_db[i]);
        r3f_write_f32_le(bytes + 8364u + (i * 4u), header->correction.phase_deg[i]);
    }
}

static uni_tektronix_r3f_status
r3f_validate_parsed_header(const uni_tektronix_r3f_header* header,
                           uint64_t file_size,
                           size_t* out_bytes_per_sample,
                           size_t* out_frame_sample_region_bytes,
                           uint64_t* out_frame_count,
                           uint64_t* out_total_samples) {
    const size_t bytes_per_sample = r3f_bytes_per_sample(header->file_data_type);
    uint64_t data_bytes = 0u;
    uint64_t frame_count = 0u;
    uint64_t total_samples = 0u;

    if (strncmp(header->file_id, UNI_TEKTRONIX_R3F_EXPECTED_FILE_ID, 27) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    if (header->endian_check != UNI_TEKTRONIX_R3F_EXPECTED_ENDIAN) {
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }
    if (bytes_per_sample == 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }
    if (header->frame_offset_bytes < (int32_t)UNI_TEKTRONIX_R3F_HEADER_SIZE ||
        header->frame_size_bytes <= 0 ||
        header->sample_offset_bytes < 0 ||
        header->samples_per_frame <= 0 ||
        header->non_sample_offset_bytes < 0 ||
        header->non_sample_size_bytes < (int32_t)UNI_TEKTRONIX_R3F_FOOTER_LAYOUT_SIZE) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    if (header->correction.table_entry_count > UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }

    const uint64_t sample_region_bytes = (uint64_t)(uint32_t)header->samples_per_frame * (uint64_t)bytes_per_sample;
    const uint64_t sample_region_start = (uint64_t)(uint32_t)header->sample_offset_bytes;
    const uint64_t footer_region_start = (uint64_t)(uint32_t)header->non_sample_offset_bytes;
    const uint64_t footer_region_end = footer_region_start + (uint64_t)(uint32_t)header->non_sample_size_bytes;
    const uint64_t frame_size = (uint64_t)(uint32_t)header->frame_size_bytes;

    if (sample_region_start + sample_region_bytes > frame_size) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    if (footer_region_end > frame_size) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    if (sample_region_start + sample_region_bytes > footer_region_start) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT;
    }
    if ((uint64_t)(uint32_t)header->frame_offset_bytes > file_size) {
        return UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE;
    }

    data_bytes = file_size - (uint64_t)(uint32_t)header->frame_offset_bytes;
    if (data_bytes % frame_size != 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE;
    }

    frame_count = data_bytes / frame_size;
    total_samples = frame_count * (uint64_t)(uint32_t)header->samples_per_frame;

    *out_bytes_per_sample = bytes_per_sample;
    *out_frame_sample_region_bytes = (size_t)sample_region_bytes;
    *out_frame_count = frame_count;
    *out_total_samples = total_samples;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
r3f_write_zero_bytes(FILE* file, size_t count) {
    uint8_t zeros[4096u];
    memset(zeros, 0, sizeof(zeros));

    while (count > 0u) {
        const size_t chunk = count > sizeof(zeros) ? sizeof(zeros) : count;
        if (fwrite(zeros, 1u, chunk, file) != chunk) {
            return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
        }
        count -= chunk;
    }

    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static uni_tektronix_r3f_status
r3f_validate_writer_header(uni_tektronix_r3f_header* header) {
    size_t bytes_per_sample = 0u;
    uint64_t sample_region_bytes = 0u;
    uint64_t frame_size = 0u;
    uint64_t sample_region_start = 0u;
    uint64_t footer_region_start = 0u;
    uint64_t footer_region_end = 0u;

    memcpy(header->file_id, UNI_TEKTRONIX_R3F_EXPECTED_FILE_ID, 27u);
    header->file_id[27] = '\0';
    if (header->endian_check == 0u) {
        header->endian_check = UNI_TEKTRONIX_R3F_EXPECTED_ENDIAN;
    }
    if (header->endian_check != UNI_TEKTRONIX_R3F_EXPECTED_ENDIAN) {
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
    bytes_per_sample = r3f_bytes_per_sample(header->file_data_type);
    if (bytes_per_sample != sizeof(int16_t)) {
        return UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT;
    }
    if (header->frame_offset_bytes < (int32_t)UNI_TEKTRONIX_R3F_HEADER_SIZE ||
        header->frame_size_bytes <= 0 ||
        header->sample_offset_bytes < 0 ||
        header->samples_per_frame <= 0 ||
        header->non_sample_offset_bytes < 0 ||
        header->non_sample_size_bytes < (int32_t)UNI_TEKTRONIX_R3F_FOOTER_LAYOUT_SIZE) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (header->correction.table_entry_count > UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    sample_region_bytes = (uint64_t)(uint32_t)header->samples_per_frame * (uint64_t)sizeof(int16_t);
    frame_size = (uint64_t)(uint32_t)header->frame_size_bytes;
    sample_region_start = (uint64_t)(uint32_t)header->sample_offset_bytes;
    footer_region_start = (uint64_t)(uint32_t)header->non_sample_offset_bytes;
    footer_region_end = footer_region_start + (uint64_t)(uint32_t)header->non_sample_size_bytes;

    if (sample_region_start + sample_region_bytes > frame_size) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (footer_region_end > frame_size) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (sample_region_start + sample_region_bytes > footer_region_start) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

static void
r3f_build_auto_footer(const uni_tektronix_r3f_writer* writer,
                      uni_tektronix_r3f_frame_footer* footer) {
    uint64_t base_timestamp = writer->header.start_sample_count;

    uni_tektronix_r3f_frame_footer_init_defaults(footer);
    footer->frame_id = (uint32_t)writer->written_frames;
    if (base_timestamp == 0u) {
        base_timestamp = writer->header.ref_sample_count;
    }
    footer->timestamp = base_timestamp +
                        (writer->written_frames * (uint64_t)(uint32_t)writer->header.samples_per_frame);
}

static uni_tektronix_r3f_status
r3f_commit_current_frame(uni_tektronix_r3f_writer* writer,
                         const uni_tektronix_r3f_frame_footer* footer_override) {
    uni_tektronix_r3f_frame_footer footer;
    uint8_t* sample_region = NULL;
    uint8_t* footer_region = NULL;

    if (writer->buffered_samples != (size_t)writer->header.samples_per_frame) {
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    memset(writer->frame_scratch, 0, (size_t)writer->header.frame_size_bytes);
    sample_region = writer->frame_scratch + (size_t)writer->header.sample_offset_bytes;
    footer_region = writer->frame_scratch + (size_t)writer->header.non_sample_offset_bytes;

    for (size_t i = 0u; i < (size_t)writer->header.samples_per_frame; ++i) {
        r3f_write_u16_le(sample_region + (i * sizeof(int16_t)), (uint16_t)writer->sample_buffer[i]);
    }

    if (footer_override != NULL) {
        footer = *footer_override;
    } else {
        r3f_build_auto_footer(writer, &footer);
    }
    r3f_write_footer_bytes(footer_region, &footer);

    if (fwrite(writer->frame_scratch, 1u, (size_t)writer->header.frame_size_bytes, writer->file) !=
        (size_t)writer->header.frame_size_bytes) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    writer->written_frames += 1u;
    writer->buffered_samples = 0u;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

const char*
uni_tektronix_r3f_status_string(uni_tektronix_r3f_status status) {
    switch (status) {
        case UNI_TEKTRONIX_R3F_STATUS_OK:
            return "ok";
        case UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT:
            return "invalid argument";
        case UNI_TEKTRONIX_R3F_STATUS_IO_ERROR:
            return "i/o error";
        case UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT:
            return "invalid R3F format";
        case UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT:
            return "unsupported R3F variant";
        case UNI_TEKTRONIX_R3F_STATUS_OUT_OF_RANGE:
            return "index out of range";
        case UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE:
            return "truncated file";
        case UNI_TEKTRONIX_R3F_STATUS_BUFFER_TOO_SMALL:
            return "buffer too small";
        case UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING:
            return "partial frame pending";
        case UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR:
            return "internal error";
        default:
            return "unknown status";
    }
}

void
uni_tektronix_r3f_header_init_defaults(uni_tektronix_r3f_header* header) {
    if (header == NULL) {
        return;
    }

    memset(header, 0, sizeof(*header));
    memcpy(header->file_id, UNI_TEKTRONIX_R3F_EXPECTED_FILE_ID, 27u);
    header->file_id[27] = '\0';
    header->endian_check = UNI_TEKTRONIX_R3F_EXPECTED_ENDIAN;
    header->file_format_version[0] = 1u;
    header->file_format_version[1] = 2u;
    header->file_format_version[2] = 0u;
    header->file_format_version[3] = 0u;
    header->file_data_type = UNI_TEKTRONIX_R3F_FILE_DATA_TYPE_INT16_IF;
    header->frame_offset_bytes = (int32_t)UNI_TEKTRONIX_R3F_HEADER_SIZE;
    header->frame_size_bytes = (int32_t)UNI_TEKTRONIX_R3F_STANDARD_FRAME_SIZE;
    header->sample_offset_bytes = 0;
    header->samples_per_frame = (int32_t)UNI_TEKTRONIX_R3F_STANDARD_SAMPLES_PER_FRAME;
    header->non_sample_offset_bytes = 16356;
    header->non_sample_size_bytes = (int32_t)UNI_TEKTRONIX_R3F_STANDARD_FOOTER_SIZE;
    header->if_center_frequency_hz = 28.0e6;
    header->sample_rate_sps = 112.0e6;
    header->bandwidth_hz = 40.0e6;
    header->ref_sample_ticks_per_second = 112000000u;
    header->sample_gain_scaling_factor = 1.0;
}

void
uni_tektronix_r3f_frame_footer_init_defaults(uni_tektronix_r3f_frame_footer* footer) {
    if (footer == NULL) {
        return;
    }
    memset(footer, 0, sizeof(*footer));
}

uni_tektronix_r3f_status
uni_tektronix_r3f_reader_open(const char* path, uni_tektronix_r3f_reader** out_reader) {
    uni_tektronix_r3f_reader* reader = NULL;
    uint8_t header_bytes[UNI_TEKTRONIX_R3F_HEADER_SIZE];
    uni_tektronix_r3f_status status;

    if (path == NULL || out_reader == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_reader = NULL;

    reader = (uni_tektronix_r3f_reader*)calloc(1u, sizeof(*reader));
    if (reader == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    reader->file = fopen(path, "rb");
    if (reader->file == NULL) {
        free(reader);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    status = r3f_get_file_size(reader->file, &reader->file_size);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fclose(reader->file);
        free(reader);
        return status;
    }
    if (reader->file_size < UNI_TEKTRONIX_R3F_HEADER_SIZE) {
        fclose(reader->file);
        free(reader);
        return UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE;
    }
    if (fread(header_bytes, 1u, sizeof(header_bytes), reader->file) != sizeof(header_bytes)) {
        fclose(reader->file);
        free(reader);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    r3f_parse_header_bytes(header_bytes, &reader->header);
    status = r3f_validate_parsed_header(&reader->header,
                                        reader->file_size,
                                        &reader->bytes_per_sample,
                                        &reader->frame_sample_region_bytes,
                                        &reader->frame_count,
                                        &reader->total_samples);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        fclose(reader->file);
        free(reader);
        return status;
    }

    reader->sample_scratch = (uint8_t*)malloc(reader->frame_sample_region_bytes);
    if (reader->sample_scratch == NULL) {
        fclose(reader->file);
        free(reader);
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    *out_reader = reader;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

void
uni_tektronix_r3f_reader_close(uni_tektronix_r3f_reader* reader) {
    if (reader == NULL) {
        return;
    }
    if (reader->file != NULL) {
        fclose(reader->file);
    }
    free(reader->sample_scratch);
    free(reader);
}

uni_tektronix_r3f_status
uni_tektronix_r3f_reader_get_header(const uni_tektronix_r3f_reader* reader,
                                    uni_tektronix_r3f_header* out_header) {
    if (reader == NULL || out_header == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_header = reader->header;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_reader_get_frame_count(const uni_tektronix_r3f_reader* reader,
                                         uint64_t* out_frame_count) {
    if (reader == NULL || out_frame_count == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_frame_count = reader->frame_count;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_reader_get_sample_count(const uni_tektronix_r3f_reader* reader,
                                          uint64_t* out_sample_count) {
    if (reader == NULL || out_sample_count == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_sample_count = reader->total_samples;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_reader_read_samples_i16(uni_tektronix_r3f_reader* reader,
                                          uint64_t start_sample_index,
                                          size_t requested_samples,
                                          int16_t* out_samples,
                                          size_t out_capacity,
                                          size_t* out_samples_read) {
    size_t total_written = 0u;
    size_t limit = 0u;
    uint64_t available = 0u;

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

    while (total_written < limit) {
        const uint64_t absolute_sample = start_sample_index + (uint64_t)total_written;
        const uint64_t frame_index = absolute_sample / (uint64_t)(uint32_t)reader->header.samples_per_frame;
        const uint32_t sample_in_frame = (uint32_t)(absolute_sample % (uint64_t)(uint32_t)reader->header.samples_per_frame);
        const size_t frame_remaining = (size_t)(uint32_t)reader->header.samples_per_frame - (size_t)sample_in_frame;
        const size_t take = (limit - total_written) < frame_remaining ? (limit - total_written) : frame_remaining;
        const uint64_t file_offset = (uint64_t)(uint32_t)reader->header.frame_offset_bytes +
                                     (frame_index * (uint64_t)(uint32_t)reader->header.frame_size_bytes) +
                                     (uint64_t)(uint32_t)reader->header.sample_offset_bytes +
                                     ((uint64_t)sample_in_frame * (uint64_t)reader->bytes_per_sample);
        const size_t bytes_to_read = take * reader->bytes_per_sample;

        if (r3f_seek_abs(reader->file, file_offset) != 0) {
            return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
        }
        if (fread(reader->sample_scratch, 1u, bytes_to_read, reader->file) != bytes_to_read) {
            return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
        }

        for (size_t i = 0u; i < take; ++i) {
            out_samples[total_written + i] = (int16_t)r3f_read_u16_le(reader->sample_scratch + (i * reader->bytes_per_sample));
        }
        total_written += take;
    }

    *out_samples_read = total_written;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_reader_read_frame_footer(uni_tektronix_r3f_reader* reader,
                                           uint64_t frame_index,
                                           uni_tektronix_r3f_frame_footer* out_footer) {
    uint8_t footer_bytes[UNI_TEKTRONIX_R3F_FOOTER_LAYOUT_SIZE];
    const uint64_t footer_offset = (uint64_t)(uint32_t)reader->header.frame_offset_bytes +
                                   (frame_index * (uint64_t)(uint32_t)reader->header.frame_size_bytes) +
                                   (uint64_t)(uint32_t)reader->header.non_sample_offset_bytes;

    if (reader == NULL || out_footer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (frame_index >= reader->frame_count) {
        return UNI_TEKTRONIX_R3F_STATUS_OUT_OF_RANGE;
    }
    if (r3f_seek_abs(reader->file, footer_offset) != 0) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if (fread(footer_bytes, 1u, sizeof(footer_bytes), reader->file) != sizeof(footer_bytes)) {
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    r3f_parse_footer_bytes(footer_bytes, out_footer);
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_writer_create(const char* path,
                                const uni_tektronix_r3f_header* header,
                                uni_tektronix_r3f_writer** out_writer) {
    uni_tektronix_r3f_writer* writer = NULL;
    uint8_t header_bytes[UNI_TEKTRONIX_R3F_HEADER_SIZE];
    uni_tektronix_r3f_status status;

    if (path == NULL || header == NULL || out_writer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    *out_writer = NULL;

    writer = (uni_tektronix_r3f_writer*)calloc(1u, sizeof(*writer));
    if (writer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }
    writer->header = *header;

    status = r3f_validate_writer_header(&writer->header);
    if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
        free(writer);
        return status;
    }

    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        free(writer);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    writer->sample_buffer = (int16_t*)malloc((size_t)writer->header.samples_per_frame * sizeof(int16_t));
    writer->frame_scratch = (uint8_t*)malloc((size_t)writer->header.frame_size_bytes);
    if (writer->sample_buffer == NULL || writer->frame_scratch == NULL) {
        uni_tektronix_r3f_writer_discard(writer);
        return UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR;
    }

    r3f_build_header_bytes(&writer->header, header_bytes);
    if (fwrite(header_bytes, 1u, sizeof(header_bytes), writer->file) != sizeof(header_bytes)) {
        uni_tektronix_r3f_writer_discard(writer);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }
    if ((size_t)writer->header.frame_offset_bytes > UNI_TEKTRONIX_R3F_HEADER_SIZE) {
        status = r3f_write_zero_bytes(writer->file,
                                      (size_t)writer->header.frame_offset_bytes - UNI_TEKTRONIX_R3F_HEADER_SIZE);
        if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
            uni_tektronix_r3f_writer_discard(writer);
            return status;
        }
    }

    *out_writer = writer;
    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_writer_append_samples_i16(uni_tektronix_r3f_writer* writer,
                                            const int16_t* samples,
                                            size_t sample_count) {
    size_t consumed = 0u;

    if (writer == NULL || (sample_count > 0u && samples == NULL)) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }

    while (consumed < sample_count) {
        const size_t frame_space = (size_t)writer->header.samples_per_frame - writer->buffered_samples;
        const size_t take = (sample_count - consumed) < frame_space ? (sample_count - consumed) : frame_space;
        memcpy(writer->sample_buffer + writer->buffered_samples, samples + consumed, take * sizeof(int16_t));
        writer->buffered_samples += take;
        consumed += take;

        if (writer->buffered_samples == (size_t)writer->header.samples_per_frame) {
            const uni_tektronix_r3f_status status = r3f_commit_current_frame(writer, NULL);
            if (status != UNI_TEKTRONIX_R3F_STATUS_OK) {
                return status;
            }
        }
    }

    return UNI_TEKTRONIX_R3F_STATUS_OK;
}

uni_tektronix_r3f_status
uni_tektronix_r3f_writer_append_frame_i16(uni_tektronix_r3f_writer* writer,
                                          const int16_t* frame_samples,
                                          const uni_tektronix_r3f_frame_footer* footer) {
    if (writer == NULL || frame_samples == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (writer->buffered_samples != 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING;
    }

    memcpy(writer->sample_buffer,
           frame_samples,
           (size_t)writer->header.samples_per_frame * sizeof(int16_t));
    writer->buffered_samples = (size_t)writer->header.samples_per_frame;
    return r3f_commit_current_frame(writer, footer);
}

uni_tektronix_r3f_status
uni_tektronix_r3f_writer_flush_partial_frame(uni_tektronix_r3f_writer* writer,
                                             int16_t pad_value,
                                             const uni_tektronix_r3f_frame_footer* footer) {
    if (writer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (writer->buffered_samples == 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_OK;
    }

    for (size_t i = writer->buffered_samples; i < (size_t)writer->header.samples_per_frame; ++i) {
        writer->sample_buffer[i] = pad_value;
    }
    writer->buffered_samples = (size_t)writer->header.samples_per_frame;
    return r3f_commit_current_frame(writer, footer);
}

uni_tektronix_r3f_status
uni_tektronix_r3f_writer_close(uni_tektronix_r3f_writer* writer) {
    int close_status = 0;

    if (writer == NULL) {
        return UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT;
    }
    if (writer->buffered_samples != 0u) {
        return UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING;
    }
    if (fflush(writer->file) != 0) {
        uni_tektronix_r3f_writer_discard(writer);
        return UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
    }

    close_status = fclose(writer->file);
    writer->file = NULL;
    free(writer->sample_buffer);
    free(writer->frame_scratch);
    free(writer);

    return close_status == 0 ? UNI_TEKTRONIX_R3F_STATUS_OK
                             : UNI_TEKTRONIX_R3F_STATUS_IO_ERROR;
}

void
uni_tektronix_r3f_writer_discard(uni_tektronix_r3f_writer* writer) {
    if (writer == NULL) {
        return;
    }
    if (writer->file != NULL) {
        fclose(writer->file);
    }
    free(writer->sample_buffer);
    free(writer->frame_scratch);
    free(writer);
}
