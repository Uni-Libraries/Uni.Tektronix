#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include "uni_tektronix_r3f.h"
}

namespace {

struct TempPath {
    std::filesystem::path path;
    explicit TempPath(const std::string& name)
        : path(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::vector<int16_t> make_sequence(std::size_t count, int seed = 0) {
    std::vector<int16_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<int16_t>((static_cast<int>(i) + seed) % 32767 - 16384);
    }
    return values;
}

std::vector<int16_t> make_if_tone(std::size_t count, double sample_rate_sps, double if_frequency_hz, double amplitude) {
    std::vector<int16_t> values(count);
    constexpr double kPi = 3.141592653589793238462643383279502884;
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = 2.0 * kPi * if_frequency_hz * static_cast<double>(i) / sample_rate_sps;
        values[i] = static_cast<int16_t>(std::lrint(std::sin(phase) * amplitude));
    }
    return values;
}

uni_tektronix_r3f_header make_header() {
    uni_tektronix_r3f_header header{};
    uni_tektronix_r3f_header_init_defaults(&header);
    header.reference_level_dbm = -10.5;
    header.rf_center_frequency_hz = 915.0e6;
    header.if_center_frequency_hz = 28.0e6;
    header.sample_rate_sps = 112.0e6;
    header.bandwidth_hz = 40.0e6;
    header.device_temperature_c = 42.0;
    header.sample_gain_scaling_factor = 0.125;
    header.signal_path_delay_seconds = 12e-9;
    header.start_sample_count = 1'000'000u;
    header.ref_sample_count = 900'000u;
    header.ref_sample_ticks_per_second = 112'000'000u;
    header.ref_time_source = 3u;
    header.ref_wall_time = {2025, 3, 19, 12, 30, 15, 123456789};
    header.ref_utc_time = {2025, 3, 19, 9, 30, 15, 123456789};
    header.start_wall_time = {2025, 3, 19, 12, 30, 16, 123456789};
    std::snprintf(header.device_serial_number, sizeof(header.device_serial_number), "RSA306B-TEST");
    std::snprintf(header.device_nomenclature, sizeof(header.device_nomenclature), "RSA306B");
    header.correction.correction_type = 1u;
    header.correction.table_entry_count = 3u;
    header.correction.frequency_hz[0] = 1.0f;
    header.correction.frequency_hz[1] = 2.0f;
    header.correction.frequency_hz[2] = 3.0f;
    header.correction.amplitude_db[0] = -0.1f;
    header.correction.amplitude_db[1] = -0.2f;
    header.correction.amplitude_db[2] = -0.3f;
    header.correction.phase_deg[0] = 10.0f;
    header.correction.phase_deg[1] = 20.0f;
    header.correction.phase_deg[2] = 30.0f;
    return header;
}

std::filesystem::path tool_path() {
    std::filesystem::path path{UNI_TEKTRONIX_BUILD_DIR};
    path /= "uni_tektronix_tool";
#if defined(_WIN32)
    path += ".exe";
#endif
    return path;
}

std::string shell_quote(const std::string& text) {
#if defined(_WIN32)
    return std::string{"\""} + text + "\"";
#else
    std::string quoted{"'"};
    for (const char ch : text) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
#endif
}

int run_tool(std::initializer_list<std::string> args) {
    std::ostringstream command;
    command << shell_quote(tool_path().string());
    for (const auto& arg : args) {
        command << ' ' << shell_quote(arg);
    }
    return std::system(command.str().c_str());
}

std::vector<std::uint8_t> read_binary(const std::filesystem::path& path) {
    std::ifstream is(path, std::ios::binary);
    REQUIRE(is.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream is(path);
    REQUIRE(is.good());
    std::ostringstream text;
    text << is.rdbuf();
    return text.str();
}

std::vector<int16_t> read_i16_le_file(const std::filesystem::path& path) {
    const auto bytes = read_binary(path);
    REQUIRE((bytes.size() % 2u) == 0u);
    std::vector<int16_t> values(bytes.size() / 2u);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint16_t raw = static_cast<std::uint16_t>(bytes[i * 2u]) |
                                  (static_cast<std::uint16_t>(bytes[i * 2u + 1u]) << 8u);
        values[i] = static_cast<int16_t>(raw);
    }
    return values;
}

std::uint16_t read_u16_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    REQUIRE(offset + 2u <= bytes.size());
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u);
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    REQUIRE(offset + 4u <= bytes.size());
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

void write_exact_frame_capture(const std::filesystem::path& path,
                               const uni_tektronix_r3f_header& header,
                               const std::vector<int16_t>& samples,
                               const std::vector<uni_tektronix_r3f_frame_footer>& footers) {
    uni_tektronix_r3f_writer* writer = nullptr;
    const auto samples_per_frame = static_cast<std::size_t>(header.samples_per_frame);

    REQUIRE(samples_per_frame > 0u);
    REQUIRE(samples.size() == footers.size() * samples_per_frame);
    REQUIRE(uni_tektronix_r3f_writer_create(path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    for (std::size_t frame_index = 0; frame_index < footers.size(); ++frame_index) {
        REQUIRE(uni_tektronix_r3f_writer_append_frame_i16(writer,
                                                          samples.data() + frame_index * samples_per_frame,
                                                          &footers[frame_index]) == UNI_TEKTRONIX_R3F_STATUS_OK);
    }
    REQUIRE(uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
}

std::vector<int16_t> read_all_r3f_samples(const std::filesystem::path& path) {
    uni_tektronix_r3f_reader* reader = nullptr;
    uint64_t sample_count = 0u;
    size_t read_count = 0u;

    REQUIRE(uni_tektronix_r3f_reader_open(path.string().c_str(), &reader) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_reader_get_sample_count(reader, &sample_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    std::vector<int16_t> samples(static_cast<std::size_t>(sample_count));
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader,
                                                      0u,
                                                      samples.size(),
                                                      samples.data(),
                                                      samples.size(),
                                                      &read_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(read_count == samples.size());
    uni_tektronix_r3f_reader_close(reader);
    return samples;
}

} // namespace

TEST_CASE("roundtrip header, footers and seekable sample reads", "[r3f][reader][writer]") {
    TempPath file{"uni_tektronix_roundtrip.r3f"};
    const auto header = make_header();
    auto all_samples = make_sequence(static_cast<std::size_t>(header.samples_per_frame) * 3u, 123);

    uni_tektronix_r3f_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3f_writer_create(file.path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uni_tektronix_r3f_frame_footer footer0{};
    uni_tektronix_r3f_frame_footer footer1{};
    uni_tektronix_r3f_frame_footer footer2{};
    uni_tektronix_r3f_frame_footer_init_defaults(&footer0);
    uni_tektronix_r3f_frame_footer_init_defaults(&footer1);
    uni_tektronix_r3f_frame_footer_init_defaults(&footer2);
    footer0.frame_id = 100u;
    footer0.trigger1_index = 11u;
    footer0.trigger2_index = 12u;
    footer0.time_sync_index = 13u;
    footer0.frame_status = 0x0021u;
    footer0.timestamp = 5000u;
    footer1.frame_id = 101u;
    footer1.trigger1_index = 21u;
    footer1.trigger2_index = 22u;
    footer1.time_sync_index = 23u;
    footer1.frame_status = 0x0022u;
    footer1.timestamp = 6000u;
    footer2.frame_id = 102u;
    footer2.trigger1_index = 31u;
    footer2.trigger2_index = 32u;
    footer2.time_sync_index = 33u;
    footer2.frame_status = 0x0023u;
    footer2.timestamp = 7000u;

    const auto spf = static_cast<std::size_t>(header.samples_per_frame);
    REQUIRE(uni_tektronix_r3f_writer_append_frame_i16(writer, all_samples.data(), &footer0) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_append_frame_i16(writer, all_samples.data() + spf, &footer1) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_append_frame_i16(writer, all_samples.data() + spf * 2u, &footer2) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uni_tektronix_r3f_reader* reader = nullptr;
    REQUIRE(uni_tektronix_r3f_reader_open(file.path.string().c_str(), &reader) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uni_tektronix_r3f_header parsed{};
    REQUIRE(uni_tektronix_r3f_reader_get_header(reader, &parsed) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(parsed.rf_center_frequency_hz == Catch::Approx(header.rf_center_frequency_hz));
    CHECK(parsed.reference_level_dbm == Catch::Approx(header.reference_level_dbm));
    CHECK(parsed.device_temperature_c == Catch::Approx(header.device_temperature_c));
    CHECK(std::string(parsed.device_serial_number) == "RSA306B-TEST");
    CHECK(std::string(parsed.device_nomenclature) == "RSA306B");
    CHECK(parsed.ref_time_source == 3u);
    CHECK(parsed.start_sample_count == 1'000'000u);
    CHECK(parsed.correction.table_entry_count == 3u);
    CHECK(parsed.correction.frequency_hz[1] == Catch::Approx(2.0f));
    CHECK(parsed.correction.amplitude_db[2] == Catch::Approx(-0.3f));
    CHECK(parsed.correction.phase_deg[0] == Catch::Approx(10.0f));

    uint64_t frame_count = 0;
    uint64_t sample_count = 0;
    REQUIRE(uni_tektronix_r3f_reader_get_frame_count(reader, &frame_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_reader_get_sample_count(reader, &sample_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(frame_count == 3u);
    CHECK(sample_count == static_cast<uint64_t>(all_samples.size()));

    std::vector<int16_t> range(32u);
    size_t read_count = 0;
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader, 0u, range.size(), range.data(), range.size(), &read_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(read_count == range.size());
    CHECK(std::vector<int16_t>(range.begin(), range.end()) == std::vector<int16_t>(all_samples.begin(), all_samples.begin() + 32));

    std::vector<int16_t> cross(20u);
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader,
                                                      static_cast<uint64_t>(spf - 10u),
                                                      cross.size(),
                                                      cross.data(),
                                                      cross.size(),
                                                      &read_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(read_count == cross.size());
    CHECK(std::vector<int16_t>(cross.begin(), cross.end()) ==
          std::vector<int16_t>(all_samples.begin() + static_cast<std::ptrdiff_t>(spf - 10u),
                               all_samples.begin() + static_cast<std::ptrdiff_t>(spf + 10u)));

    uni_tektronix_r3f_frame_footer parsed_footer{};
    REQUIRE(uni_tektronix_r3f_reader_read_frame_footer(reader, 1u, &parsed_footer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(parsed_footer.frame_id == 101u);
    CHECK(parsed_footer.trigger1_index == 21u);
    CHECK(parsed_footer.trigger2_index == 22u);
    CHECK(parsed_footer.time_sync_index == 23u);
    CHECK(parsed_footer.frame_status == 0x0022u);
    CHECK(parsed_footer.timestamp == 6000u);

    uni_tektronix_r3f_reader_close(reader);
}

TEST_CASE("streaming writer buffers partial frames and can flush padded tail", "[r3f][writer]") {
    TempPath file{"uni_tektronix_partial_flush.r3f"};
    auto header = make_header();
    const auto source = make_sequence(100u, 77);

    uni_tektronix_r3f_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3f_writer_create(file.path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_append_samples_i16(writer, source.data(), source.size()) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_PARTIAL_FRAME_PENDING);
    REQUIRE(uni_tektronix_r3f_writer_flush_partial_frame(writer, static_cast<int16_t>(-1), nullptr) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uni_tektronix_r3f_reader* reader = nullptr;
    REQUIRE(uni_tektronix_r3f_reader_open(file.path.string().c_str(), &reader) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uint64_t frame_count = 0;
    REQUIRE(uni_tektronix_r3f_reader_get_frame_count(reader, &frame_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(frame_count == 1u);

    std::vector<int16_t> recovered(static_cast<std::size_t>(header.samples_per_frame));
    size_t read_count = 0;
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader,
                                                      0u,
                                                      recovered.size(),
                                                      recovered.data(),
                                                      recovered.size(),
                                                      &read_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(read_count == recovered.size());
    CHECK(std::vector<int16_t>(recovered.begin(), recovered.begin() + static_cast<std::ptrdiff_t>(source.size())) == source);
    for (std::size_t i = source.size(); i < recovered.size(); ++i) {
        CHECK(recovered[i] == static_cast<int16_t>(-1));
    }

    uni_tektronix_r3f_frame_footer footer{};
    REQUIRE(uni_tektronix_r3f_reader_read_frame_footer(reader, 0u, &footer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(footer.frame_id == 0u);
    CHECK(footer.timestamp == header.start_sample_count);

    uni_tektronix_r3f_reader_close(reader);
}

TEST_CASE("reader rejects malformed file id", "[r3f][validation]") {
    TempPath file{"uni_tektronix_invalid_header.r3f"};
    std::vector<unsigned char> bytes(UNI_TEKTRONIX_R3F_HEADER_SIZE, 0u);
    const char bad_id[] = "Not a Tektronix R3F file";

    {
        std::ofstream os(file.path, std::ios::binary);
        REQUIRE(os.good());
        os.write(reinterpret_cast<const char*>(bad_id), sizeof(bad_id) - 1);
        os.write(reinterpret_cast<const char*>(bytes.data() + sizeof(bad_id) - 1),
                 static_cast<std::streamsize>(bytes.size() - (sizeof(bad_id) - 1)));
        REQUIRE(os.good());
    }

    uni_tektronix_r3f_reader* reader = nullptr;
    CHECK(uni_tektronix_r3f_reader_open(file.path.string().c_str(), &reader) == UNI_TEKTRONIX_R3F_STATUS_INVALID_FORMAT);
    CHECK(reader == nullptr);
}

TEST_CASE("raw header helpers and raw pair API roundtrip", "[r3a][r3h][api]") {
    TempPath raw_data{"uni_tektronix_api_roundtrip.r3a"};
    TempPath raw_header{"uni_tektronix_api_roundtrip.r3h"};
    TempPath header_only{"uni_tektronix_header_only.r3h"};
    auto header = make_header();
    const auto source = make_sequence(1234u, 19);

    REQUIRE(uni_tektronix_r3f_header_set_raw_layout(&header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(header.frame_offset_bytes == 0);
    CHECK(header.frame_size_bytes == 0);
    CHECK(header.samples_per_frame == 0);
    CHECK(header.non_sample_size_bytes == 0);

    REQUIRE(uni_tektronix_r3f_header_write_file(header_only.path.string().c_str(), &header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    uni_tektronix_r3f_header parsed_header{};
    REQUIRE(uni_tektronix_r3f_header_read_file(header_only.path.string().c_str(), &parsed_header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(parsed_header.frame_size_bytes == 0);
    CHECK(parsed_header.samples_per_frame == 0);
    CHECK(parsed_header.sample_rate_sps == Catch::Approx(header.sample_rate_sps));
    CHECK(parsed_header.if_center_frequency_hz == Catch::Approx(header.if_center_frequency_hz));

    REQUIRE(uni_tektronix_r3f_header_set_framed_layout(&parsed_header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(parsed_header.frame_size_bytes == static_cast<int32_t>(UNI_TEKTRONIX_R3F_STANDARD_FRAME_SIZE));
    CHECK(parsed_header.samples_per_frame == static_cast<int32_t>(UNI_TEKTRONIX_R3F_STANDARD_SAMPLES_PER_FRAME));
    CHECK(parsed_header.non_sample_size_bytes == static_cast<int32_t>(UNI_TEKTRONIX_R3F_STANDARD_FOOTER_SIZE));

    uni_tektronix_r3a_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3a_writer_create(raw_data.path.string().c_str(), raw_header.path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3a_writer_append_samples_i16(writer, source.data(), source.size()) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3a_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uni_tektronix_r3a_reader* reader = nullptr;
    REQUIRE(uni_tektronix_r3a_reader_open(raw_header.path.string().c_str(), nullptr, &reader) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uint64_t sample_count = 0u;
    REQUIRE(uni_tektronix_r3a_reader_get_sample_count(reader, &sample_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(sample_count == static_cast<uint64_t>(source.size()));

    uni_tektronix_r3f_header reader_header{};
    REQUIRE(uni_tektronix_r3a_reader_get_header(reader, &reader_header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(reader_header.frame_size_bytes == 0);
    CHECK(reader_header.samples_per_frame == 0);
    CHECK(reader_header.start_sample_count == header.start_sample_count);

    std::vector<int16_t> recovered(source.size());
    size_t read_count = 0u;
    REQUIRE(uni_tektronix_r3a_reader_read_samples_i16(reader,
                                                      0u,
                                                      recovered.size(),
                                                      recovered.data(),
                                                      recovered.size(),
                                                      &read_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(read_count == recovered.size());
    CHECK(recovered == source);

    std::vector<int16_t> window(64u);
    REQUIRE(uni_tektronix_r3a_reader_read_samples_i16(reader,
                                                      77u,
                                                      window.size(),
                                                      window.data(),
                                                      window.size(),
                                                      &read_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(read_count == window.size());
    CHECK(window == std::vector<int16_t>(source.begin() + 77, source.begin() + 77 + 64));

    uni_tektronix_r3a_reader_close(reader);
}

TEST_CASE("CLI supports split/pack/extract/info/validate/export-footers", "[cli][tool]") {
    TempPath source_r3f{"uni_tektronix_cli_source.r3f"};
    TempPath split_r3a{"uni_tektronix_cli_split.r3a"};
    TempPath split_r3h{"uni_tektronix_cli_split.r3h"};
    TempPath repacked_r3f{"uni_tektronix_cli_repacked.r3f"};
    TempPath raw_i16{"uni_tektronix_cli_extract.i16"};
    TempPath csv{"uni_tektronix_cli_footers.csv"};

    const auto header = make_header();
    const auto spf = static_cast<std::size_t>(header.samples_per_frame);
    const auto samples = make_sequence(spf * 2u, 55);

    uni_tektronix_r3f_frame_footer footer0{};
    uni_tektronix_r3f_frame_footer footer1{};
    uni_tektronix_r3f_frame_footer_init_defaults(&footer0);
    uni_tektronix_r3f_frame_footer_init_defaults(&footer1);
    footer0.frame_id = 700u;
    footer0.timestamp = 10'000u;
    footer1.frame_id = 701u;
    footer1.timestamp = 20'000u;
    write_exact_frame_capture(source_r3f.path, header, samples, {footer0, footer1});

    REQUIRE(std::filesystem::exists(tool_path()));
    REQUIRE(run_tool({"validate", source_r3f.path.string()}) == 0);
    REQUIRE(run_tool({"info", source_r3f.path.string()}) == 0);
    REQUIRE(run_tool({"split", source_r3f.path.string(), split_r3a.path.string()}) == 0);
    REQUIRE(std::filesystem::exists(split_r3a.path));
    REQUIRE(std::filesystem::exists(split_r3h.path));

    uni_tektronix_r3a_reader* raw_reader = nullptr;
    REQUIRE(uni_tektronix_r3a_reader_open(split_r3h.path.string().c_str(), nullptr, &raw_reader) == UNI_TEKTRONIX_R3F_STATUS_OK);
    uni_tektronix_r3f_header raw_header{};
    REQUIRE(uni_tektronix_r3a_reader_get_header(raw_reader, &raw_header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(raw_header.frame_size_bytes == 0);
    CHECK(raw_header.samples_per_frame == 0);
    uint64_t raw_sample_count = 0u;
    REQUIRE(uni_tektronix_r3a_reader_get_sample_count(raw_reader, &raw_sample_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(raw_sample_count == static_cast<uint64_t>(samples.size()));
    uni_tektronix_r3a_reader_close(raw_reader);

    REQUIRE(run_tool({"pack", split_r3a.path.string(), repacked_r3f.path.string()}) == 0);
    CHECK(read_all_r3f_samples(repacked_r3f.path) == samples);

    REQUIRE(run_tool({"extract-raw",
                      source_r3f.path.string(),
                      raw_i16.path.string(),
                      "--start",
                      "5",
                      "--count",
                      "20"}) == 0);
    CHECK(read_i16_le_file(raw_i16.path) == std::vector<int16_t>(samples.begin() + 5, samples.begin() + 25));

    REQUIRE(run_tool({"export-footers", source_r3f.path.string(), csv.path.string()}) == 0);
    const auto csv_text = read_text(csv.path);
    CHECK(csv_text.find("frame_index,frame_id") != std::string::npos);
    CHECK(csv_text.find(",700,") != std::string::npos);
    CHECK(csv_text.find(",701,") != std::string::npos);
}

TEST_CASE("CLI trim and SDRSharp-compatible WAV export", "[cli][wav][trim]") {
    TempPath raw_data{"uni_tektronix_cli_wav_source.r3a"};
    TempPath raw_header{"uni_tektronix_cli_wav_source.r3h"};
    TempPath trimmed_r3f{"uni_tektronix_cli_trimmed.r3f"};
    TempPath trimmed_r3a{"uni_tektronix_cli_trimmed_pair.r3a"};
    TempPath trimmed_r3h{"uni_tektronix_cli_trimmed_pair.r3h"};
    TempPath wav{"uni_tektronix_cli_iq.wav"};

    auto header = make_header();
    header.sample_rate_sps = 48'000.0;
    header.if_center_frequency_hz = 12'000.0;
    header.bandwidth_hz = 8'000.0;
    header.start_sample_count = 5'000u;
    const auto source = make_if_tone(2'000u, header.sample_rate_sps, header.if_center_frequency_hz, 14'000.0);

    uni_tektronix_r3a_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3a_writer_create(raw_data.path.string().c_str(), raw_header.path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3a_writer_append_samples_i16(writer, source.data(), source.size()) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3a_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    REQUIRE(run_tool({"trim",
                      raw_data.path.string(),
                      trimmed_r3f.path.string(),
                      "--start",
                      "100",
                      "--count",
                      "500",
                      "--tail-mode",
                      "pad",
                      "--pad-value",
                      "-7"}) == 0);

    uni_tektronix_r3f_reader* trimmed_reader = nullptr;
    REQUIRE(uni_tektronix_r3f_reader_open(trimmed_r3f.path.string().c_str(), &trimmed_reader) == UNI_TEKTRONIX_R3F_STATUS_OK);
    uni_tektronix_r3f_header trimmed_header{};
    REQUIRE(uni_tektronix_r3f_reader_get_header(trimmed_reader, &trimmed_header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(trimmed_header.start_sample_count == header.start_sample_count + 100u);
    std::vector<int16_t> trimmed_samples(static_cast<std::size_t>(UNI_TEKTRONIX_R3F_STANDARD_SAMPLES_PER_FRAME));
    size_t trimmed_read = 0u;
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(trimmed_reader,
                                                      0u,
                                                      trimmed_samples.size(),
                                                      trimmed_samples.data(),
                                                      trimmed_samples.size(),
                                                      &trimmed_read) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(trimmed_read == trimmed_samples.size());
    CHECK(std::vector<int16_t>(trimmed_samples.begin(), trimmed_samples.begin() + 500) ==
          std::vector<int16_t>(source.begin() + 100, source.begin() + 600));
    for (std::size_t i = 500u; i < trimmed_samples.size(); ++i) {
        CHECK(trimmed_samples[i] == static_cast<int16_t>(-7));
    }
    uni_tektronix_r3f_reader_close(trimmed_reader);

    REQUIRE(run_tool({"trim",
                      raw_data.path.string(),
                      trimmed_r3a.path.string(),
                      "--start",
                      "100",
                      "--count",
                      "500"}) == 0);
    REQUIRE(std::filesystem::exists(trimmed_r3a.path));
    REQUIRE(std::filesystem::exists(trimmed_r3h.path));

    uni_tektronix_r3a_reader* trimmed_raw_reader = nullptr;
    REQUIRE(uni_tektronix_r3a_reader_open(trimmed_r3a.path.string().c_str(), nullptr, &trimmed_raw_reader) == UNI_TEKTRONIX_R3F_STATUS_OK);
    uint64_t trimmed_raw_count = 0u;
    REQUIRE(uni_tektronix_r3a_reader_get_sample_count(trimmed_raw_reader, &trimmed_raw_count) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(trimmed_raw_count == 500u);
    std::vector<int16_t> trimmed_raw_samples(500u);
    REQUIRE(uni_tektronix_r3a_reader_read_samples_i16(trimmed_raw_reader,
                                                      0u,
                                                      trimmed_raw_samples.size(),
                                                      trimmed_raw_samples.data(),
                                                      trimmed_raw_samples.size(),
                                                      &trimmed_read) == UNI_TEKTRONIX_R3F_STATUS_OK);
    CHECK(trimmed_raw_samples == std::vector<int16_t>(source.begin() + 100, source.begin() + 600));
    uni_tektronix_r3a_reader_close(trimmed_raw_reader);

    REQUIRE(run_tool({"to-wav",
                      raw_data.path.string(),
                      wav.path.string(),
                      "--count",
                      "1024",
                      "--decimate",
                      "4"}) == 0);

    const auto wav_bytes = read_binary(wav.path);
    REQUIRE(wav_bytes.size() > 44u);
    CHECK(std::string(reinterpret_cast<const char*>(wav_bytes.data()), 4) == "RIFF");
    CHECK(std::string(reinterpret_cast<const char*>(wav_bytes.data() + 8), 4) == "WAVE");
    CHECK(std::string(reinterpret_cast<const char*>(wav_bytes.data() + 12), 4) == "fmt ");
    CHECK(std::string(reinterpret_cast<const char*>(wav_bytes.data() + 36), 4) == "data");
    CHECK(read_u32_le(wav_bytes, 16u) == 16u);
    CHECK(read_u16_le(wav_bytes, 20u) == 1u);
    CHECK(read_u16_le(wav_bytes, 22u) == 2u);
    CHECK(read_u32_le(wav_bytes, 24u) == 12'000u);
    CHECK(read_u32_le(wav_bytes, 28u) == 48'000u);
    CHECK(read_u16_le(wav_bytes, 32u) == 4u);
    CHECK(read_u16_le(wav_bytes, 34u) == 16u);
    CHECK(read_u32_le(wav_bytes, 40u) == wav_bytes.size() - 44u);
    CHECK(((wav_bytes.size() - 44u) % 4u) == 0u);

    bool has_non_zero_payload = false;
    for (std::size_t i = 44u; i < wav_bytes.size(); ++i) {
        if (wav_bytes[i] != 0u) {
            has_non_zero_payload = true;
            break;
        }
    }
    CHECK(has_non_zero_payload);
}
