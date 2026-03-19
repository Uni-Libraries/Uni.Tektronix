#ifndef UNI_TEKTRONIX_R3F_H
#define UNI_TEKTRONIX_R3F_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(__has_include)
#  if __has_include("uni_tektronix_export.h")
#    include "uni_tektronix_export.h"
#  else
#    define UNI_TEKTRONIX_EXPORT
#    define UNI_TEKTRONIX_NO_EXPORT
#    define UNI_TEKTRONIX_DEPRECATED
#  endif
#else
#  include "uni_tektronix_export.h"
#endif

/** @brief Size of the fixed R3F/R3H header block in bytes. */
#define UNI_TEKTRONIX_R3F_HEADER_SIZE 16384u

/** @brief Default Tektronix formatted frame size in bytes. */
#define UNI_TEKTRONIX_R3F_STANDARD_FRAME_SIZE 16384u

/** @brief Default number of 16-bit IF samples stored in one formatted frame. */
#define UNI_TEKTRONIX_R3F_STANDARD_SAMPLES_PER_FRAME 8178u

/** @brief Default Tektronix formatted footer size in bytes. */
#define UNI_TEKTRONIX_R3F_STANDARD_FOOTER_SIZE 28u

/** @brief Tektronix file-data-type code for 16-bit IF samples. */
#define UNI_TEKTRONIX_R3F_FILE_DATA_TYPE_INT16_IF 161u

/** @brief Maximum documented number of channel-correction table entries. */
#define UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES 501u

/**
 * @brief Status code returned by the uni_tektronix R3F/R3A/R3H API.
 */
typedef enum uni_tektronix_r3f_status {
    /** Operation completed successfully. */
    UNI_TEKTRONIX_R3F_STATUS_OK = 0,
    /** One or more input arguments are invalid. */
    UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT,
    /** An I/O error occurred while accessing the file. */
    UNI_TEKTRONIX_R3F_STATUS_IO_ERROR,
    /** The file is not a structurally valid Tektronix file. */
    UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT,
    /** The file uses a valid but unsupported variation of the format. */
    UNI_TEKTRONIX_R3F_STATUS_UNSUPPORTED_FORMAT,
    /** A requested frame or sample index is outside the available range. */
    UNI_TEKTRONIX_R3F_STATUS_OUT_OF_RANGE,
    /** The file ends before a complete logical structure could be read. */
    UNI_TEKTRONIX_R3F_STATUS_TRUNCATED_FILE,
    /** The provided output buffer is too small for the requested operation. */
    UNI_TEKTRONIX_R3F_STATUS_BUFFER_TOO_SMALL,
    /** The writer still contains a partial frame that must be flushed or discarded. */
    UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING,
    /** An internal error occurred. */
    UNI_TEKTRONIX_R3F_STATUS_INTERNAL_ERROR
} uni_tektronix_r3f_status;

/**
 * @brief Calendar date-time tuple used by Tektronix R3F headers.
 *
 * The fields mirror the 7-int32 tuples documented by Tektronix:
 * year, month, day, hour, minute, second, nanosecond.
 */
typedef struct uni_tektronix_r3f_datetime {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
    int32_t nanosecond;
} uni_tektronix_r3f_datetime;

/**
 * @brief Channel-correction tables stored in the R3F header.
 *
 * The arrays are exposed in the order documented by the Tektronix API manual:
 * frequency, amplitude, phase. A legacy Python reference utility bundled with
 * Tektronix appears to read the last two tables in reverse order; when exact
 * behavioural compatibility with that script matters, interpret the two arrays
 * accordingly.
 */
typedef struct uni_tektronix_r3f_channel_correction {
    uint32_t correction_type;
    uint32_t table_entry_count;
    float frequency_hz[UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES];
    float amplitude_db[UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES];
    float phase_deg[UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES];
} uni_tektronix_r3f_channel_correction;

/**
 * @brief Parsed 16-kB R3F/R3H header.
 */
typedef struct uni_tektronix_r3f_header {
    char file_id[28];
    uint32_t endian_check;
    uint8_t file_format_version[4];
    uint8_t api_sw_version[4];
    uint8_t fx3_fw_version[4];
    uint8_t fpga_fw_version[4];
    char device_serial_number[65];
    char device_nomenclature[33];

    double reference_level_dbm;
    double rf_center_frequency_hz;
    double device_temperature_c;
    uint32_t alignment_state;
    uint32_t frequency_reference_state;
    uint32_t trigger_mode;
    uint32_t trigger_source;
    uint32_t trigger_transition;
    double trigger_level_dbm;

    uint32_t file_data_type;
    int32_t frame_offset_bytes;
    int32_t frame_size_bytes;
    int32_t sample_offset_bytes;
    int32_t samples_per_frame;
    int32_t non_sample_offset_bytes;
    int32_t non_sample_size_bytes;
    double if_center_frequency_hz;
    double sample_rate_sps;
    double bandwidth_hz;
    uint32_t data_corrected;
    uint32_t ref_time_type;
    uni_tektronix_r3f_datetime ref_wall_time;
    uint64_t ref_sample_count;
    uint64_t ref_sample_ticks_per_second;
    uni_tektronix_r3f_datetime ref_utc_time;
    uint32_t ref_time_source;
    uint64_t start_sample_count;
    uni_tektronix_r3f_datetime start_wall_time;

    double sample_gain_scaling_factor;
    double signal_path_delay_seconds;
    uni_tektronix_r3f_channel_correction correction;
} uni_tektronix_r3f_header;

/**
 * @brief Parsed 28-byte transport footer attached to a formatted frame.
 *
 * The reference archive parses frame counters, trigger indexes, a status word,
 * and a 64-bit timestamp from the footer. The individual status bits are left
 * uninterpreted by this library and are exposed as a raw 16-bit value.
 */
typedef struct uni_tektronix_r3f_frame_footer {
    uint16_t reserved[4];
    uint32_t frame_id;
    uint16_t trigger2_index;
    uint16_t trigger1_index;
    uint16_t time_sync_index;
    uint16_t frame_status;
    uint64_t timestamp;
} uni_tektronix_r3f_frame_footer;

/** @brief Opaque seekable R3F reader handle. */
typedef struct uni_tektronix_r3f_reader uni_tektronix_r3f_reader;

/** @brief Opaque streaming R3F writer handle. */
typedef struct uni_tektronix_r3f_writer uni_tektronix_r3f_writer;

/** @brief Opaque seekable raw IF data reader for paired R3A/R3H files. */
typedef struct uni_tektronix_r3a_reader uni_tektronix_r3a_reader;

/** @brief Opaque streaming raw IF data writer for paired R3A/R3H files. */
typedef struct uni_tektronix_r3a_writer uni_tektronix_r3a_writer;

/**
 * @brief Return a human-readable description of a status code.
 *
 * @param status Status code returned by the API.
 * @return Static UTF-8 string describing the code.
 */
UNI_TEKTRONIX_EXPORT const char*
uni_tektronix_r3f_status_string(uni_tektronix_r3f_status status);

/**
 * @brief Initialize an R3F header structure with safe documented defaults.
 *
 * The defaults match the standard Tektronix formatted IF layout described in
 * the API programmer manual: 16-kB header, 16-kB frames, 8178 int16 samples per
 * frame, 28-byte footer, and file format version 1.2.0.0.
 *
 * @param header Header object to initialize.
 */
UNI_TEKTRONIX_EXPORT void
uni_tektronix_r3f_header_init_defaults(uni_tektronix_r3f_header* header);

/**
 * @brief Initialize a frame footer structure with zero values.
 *
 * @param footer Footer object to initialize.
 */
UNI_TEKTRONIX_EXPORT void
uni_tektronix_r3f_frame_footer_init_defaults(uni_tektronix_r3f_frame_footer* footer);

/**
 * @brief Read and parse a standalone 16-kB Tektronix header block from disk.
 *
 * This function accepts both framed-file headers stored at the beginning of an
 * `.r3f` file and raw-mode headers stored in an `.r3h` file.
 *
 * @param path Path to an `.r3h` file or to a 16-kB extracted header block.
 * @param out_header Receives the parsed header.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_header_read_file(const char* path,
                                   uni_tektronix_r3f_header* out_header);

/**
 * @brief Serialize a 16-kB Tektronix header block to disk.
 *
 * The function writes exactly @ref UNI_TEKTRONIX_R3F_HEADER_SIZE bytes.
 *
 * @param path Destination path.
 * @param header Header to serialize.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_header_write_file(const char* path,
                                    const uni_tektronix_r3f_header* header);

/**
 * @brief Reconfigure a header for the standard framed `.r3f` storage layout.
 *
 * The standard Tektronix framed layout uses a 16-kB configuration block,
 * 16-kB data frames, 8178 int16 samples per frame, and a 28-byte footer.
 *
 * @param header Header to modify in-place.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_header_set_framed_layout(uni_tektronix_r3f_header* header);

/**
 * @brief Reconfigure a header for the raw `.r3a/.r3h` storage layout.
 *
 * The Tektronix manual states that the `.r3h` file stores the same 16-kB
 * configuration block as `.r3f`, but the six frame-descriptor integers are
 * set to zero because the `.r3a` file contains only contiguous IF samples.
 *
 * @param header Header to modify in-place.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_header_set_raw_layout(uni_tektronix_r3f_header* header);

/**
 * @brief Open an existing paired raw IF capture for seekable reading.
 *
 * The reader opens a contiguous `.r3a` file and its associated `.r3h` header.
 * When @p header_path is @c NULL, the sibling `.r3h` path is inferred from
 * @p data_path by replacing `.r3a` with `.r3h`; likewise, if @p data_path
 * points at an `.r3h` file, the sibling `.r3a` path is inferred.
 *
 * @param data_path Path to `.r3a` data or to a sibling `.r3h` file.
 * @param header_path Optional explicit `.r3h` path.
 * @param out_reader Receives the newly allocated reader handle.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_reader_open(const char* data_path,
                              const char* header_path,
                              uni_tektronix_r3a_reader** out_reader);

/**
 * @brief Close a raw reader and release its resources.
 *
 * Passing @c NULL is allowed.
 *
 * @param reader Reader handle to close.
 */
UNI_TEKTRONIX_EXPORT void
uni_tektronix_r3a_reader_close(uni_tektronix_r3a_reader* reader);

/**
 * @brief Copy the parsed `.r3h` header out of an open raw reader.
 *
 * @param reader Open reader handle.
 * @param out_header Destination header structure.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_reader_get_header(const uni_tektronix_r3a_reader* reader,
                                    uni_tektronix_r3f_header* out_header);

/**
 * @brief Query the number of raw IF samples exposed by a `.r3a` file.
 *
 * @param reader Open reader handle.
 * @param out_sample_count Receives the sample count.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_reader_get_sample_count(const uni_tektronix_r3a_reader* reader,
                                          uint64_t* out_sample_count);

/**
 * @brief Read a seekable range of int16 IF samples from a raw `.r3a` file.
 *
 * The reader may satisfy the request with fewer samples than requested when the
 * range reaches end-of-file or when @p out_capacity is smaller than
 * @p requested_samples. The actual number written is returned via
 * @p out_samples_read.
 *
 * @param reader Open reader handle.
 * @param start_sample_index Zero-based sample index to start from.
 * @param requested_samples Maximum number of samples to read.
 * @param out_samples Destination buffer for raw int16 samples.
 * @param out_capacity Capacity of @p out_samples in samples.
 * @param out_samples_read Receives the number of samples actually written.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_reader_read_samples_i16(uni_tektronix_r3a_reader* reader,
                                          uint64_t start_sample_index,
                                          size_t requested_samples,
                                          int16_t* out_samples,
                                          size_t out_capacity,
                                          size_t* out_samples_read);

/**
 * @brief Create a new streaming raw IF capture pair (`.r3a` + `.r3h`).
 *
 * The writer serializes the header to the `.r3h` sidecar using the Tektronix
 * raw layout (zeroed frame-descriptor fields) and then appends contiguous
 * int16 IF samples to the `.r3a` data file.
 *
 * @param data_path Path to the `.r3a` file to create.
 * @param header_path Optional explicit `.r3h` path. When @c NULL, the sibling
 *        `.r3h` path is inferred from @p data_path.
 * @param header Header template to serialize.
 * @param out_writer Receives the newly allocated writer handle.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_writer_create(const char* data_path,
                                const char* header_path,
                                const uni_tektronix_r3f_header* header,
                                uni_tektronix_r3a_writer** out_writer);

/**
 * @brief Append an arbitrary chunk of raw IF samples to an `.r3a` writer.
 *
 * @param writer Open writer handle.
 * @param samples Source sample buffer.
 * @param sample_count Number of source samples.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_writer_append_samples_i16(uni_tektronix_r3a_writer* writer,
                                            const int16_t* samples,
                                            size_t sample_count);

/**
 * @brief Finalize and close a raw writer.
 *
 * @param writer Writer handle.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3a_writer_close(uni_tektronix_r3a_writer* writer);

/**
 * @brief Abort a raw writer and release its resources.
 *
 * Passing @c NULL is allowed.
 *
 * @param writer Writer handle to discard.
 */
UNI_TEKTRONIX_EXPORT void
uni_tektronix_r3a_writer_discard(uni_tektronix_r3a_writer* writer);

/**
 * @brief Open an existing R3F file for seekable reading.
 *
 * The function reads and validates the 16-kB header, computes the frame count
 * from the file size and frame descriptor values, and prepares the handle for
 * random-access sample reads.
 *
 * @param path Path to the existing file.
 * @param out_reader Receives the newly allocated reader handle.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_reader_open(const char* path, uni_tektronix_r3f_reader** out_reader);

/**
 * @brief Close a reader and release its resources.
 *
 * Passing @c NULL is allowed.
 *
 * @param reader Reader handle to close.
 */
UNI_TEKTRONIX_EXPORT void
uni_tektronix_r3f_reader_close(uni_tektronix_r3f_reader* reader);

/**
 * @brief Copy the parsed header out of an open reader.
 *
 * @param reader Open reader handle.
 * @param out_header Destination header structure.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_reader_get_header(const uni_tektronix_r3f_reader* reader,
                                    uni_tektronix_r3f_header* out_header);

/**
 * @brief Query the number of complete formatted frames in the file.
 *
 * @param reader Open reader handle.
 * @param out_frame_count Receives the frame count.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_reader_get_frame_count(const uni_tektronix_r3f_reader* reader,
                                         uint64_t* out_frame_count);

/**
 * @brief Query the number of raw IF samples exposed by the file.
 *
 * This value is equal to @c frame_count * samples_per_frame from the parsed
 * descriptor block.
 *
 * @param reader Open reader handle.
 * @param out_sample_count Receives the sample count.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_reader_get_sample_count(const uni_tektronix_r3f_reader* reader,
                                          uint64_t* out_sample_count);

/**
 * @brief Read a seekable range of int16 IF samples.
 *
 * The reader may satisfy the request with fewer samples than requested when the
 * range reaches end-of-file or when @p out_capacity is smaller than
 * @p requested_samples. The actual number written is returned via
 * @p out_samples_read.
 *
 * @param reader Open reader handle.
 * @param start_sample_index Zero-based sample index to start from.
 * @param requested_samples Maximum number of samples to read.
 * @param out_samples Destination buffer for raw int16 samples.
 * @param out_capacity Capacity of @p out_samples in samples.
 * @param out_samples_read Receives the number of samples actually written.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_reader_read_samples_i16(uni_tektronix_r3f_reader* reader,
                                          uint64_t start_sample_index,
                                          size_t requested_samples,
                                          int16_t* out_samples,
                                          size_t out_capacity,
                                          size_t* out_samples_read);

/**
 * @brief Read the 28-byte transport footer for a specific frame.
 *
 * @param reader Open reader handle.
 * @param frame_index Zero-based frame index.
 * @param out_footer Receives the parsed footer.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_reader_read_frame_footer(uni_tektronix_r3f_reader* reader,
                                           uint64_t frame_index,
                                           uni_tektronix_r3f_frame_footer* out_footer);

/**
 * @brief Create a new streaming R3F writer.
 *
 * The caller is expected to start from
 * uni_tektronix_r3f_header_init_defaults() and then customize semantic fields
 * such as center frequency, timestamps, and correction data. The writer emits
 * the fixed 16-kB header immediately and then accepts samples or complete
 * frames incrementally.
 *
 * @param path Path of the file to create.
 * @param header Header template to serialize.
 * @param out_writer Receives the newly allocated writer handle.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_writer_create(const char* path,
                                const uni_tektronix_r3f_header* header,
                                uni_tektronix_r3f_writer** out_writer);

/**
 * @brief Append an arbitrary chunk of raw IF samples to the writer.
 *
 * Samples are buffered until a full frame is available. Each committed frame
 * receives an auto-generated footer with zeroed trigger metadata, a sequential
 * frame identifier, and a timestamp derived from @c start_sample_count (or
 * @c ref_sample_count when @c start_sample_count is zero).
 *
 * @param writer Open writer handle.
 * @param samples Source sample buffer.
 * @param sample_count Number of source samples.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_writer_append_samples_i16(uni_tektronix_r3f_writer* writer,
                                            const int16_t* samples,
                                            size_t sample_count);

/**
 * @brief Append exactly one complete frame worth of samples.
 *
 * The call fails with UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING when the
 * writer already contains buffered samples from previous incremental writes.
 *
 * @param writer Open writer handle.
 * @param frame_samples Exactly @c samples_per_frame raw int16 samples.
 * @param footer Optional footer to store. Pass @c NULL to auto-generate one.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_writer_append_frame_i16(uni_tektronix_r3f_writer* writer,
                                          const int16_t* frame_samples,
                                          const uni_tektronix_r3f_frame_footer* footer);

/**
 * @brief Flush the current partial frame by padding the unwritten tail.
 *
 * R3F formatted files contain only complete frames. This function pads the
 * in-memory tail with @p pad_value and commits it as a full frame.
 *
 * @param writer Open writer handle.
 * @param pad_value Value used to pad unwritten samples in the last frame.
 * @param footer Optional footer to store. Pass @c NULL to auto-generate one.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_writer_flush_partial_frame(uni_tektronix_r3f_writer* writer,
                                             int16_t pad_value,
                                             const uni_tektronix_r3f_frame_footer* footer);

/**
 * @brief Finalize and close the writer.
 *
 * When a partial frame is still buffered, the function returns
 * UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING and leaves the writer open so
 * the caller can either flush the tail or discard the writer.
 *
 * @param writer Writer handle.
 * @return API status.
 */
UNI_TEKTRONIX_EXPORT uni_tektronix_r3f_status
uni_tektronix_r3f_writer_close(uni_tektronix_r3f_writer* writer);

/**
 * @brief Abort a writer and release its resources.
 *
 * The file is closed without attempting to flush a pending partial frame.
 * Passing @c NULL is allowed.
 *
 * @param writer Writer handle to discard.
 */
UNI_TEKTRONIX_EXPORT void
uni_tektronix_r3f_writer_discard(uni_tektronix_r3f_writer* writer);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* UNI_TEKTRONIX_R3F_H */
