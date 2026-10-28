#if __has_include(<catch2/catch_all.hpp>)
#include <catch2/catch_all.hpp>
#elif __has_include(<catch_amalgamated.hpp>)
#include <catch_amalgamated.hpp>
#else
#error "Catch2 headers not found: install Catch2 3 or enable the CMake amalgamated fallback."
#endif

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include "uni_tektronix_r3f.h"
}

/*
 * Robustness tests for the readers: files come from disk and are untrusted, so every header field
 * and the file length are mutated with a fixed seed. The readers must either reject the file or
 * serve exactly the advertised number of samples; AddressSanitizer/UBSan builds catch the rest.
 */

namespace {

using Bytes = std::vector<std::uint8_t>;

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

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

void put_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4u; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8u * i));
    }
}

std::uint32_t interesting_u32(std::mt19937& rng) {
    static constexpr std::uint32_t values[] = {
        0u, 1u, 2u, 27u, 28u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 16356u, 16384u, 16385u, 8178u, 8179u, 32768u,
    };
    if (rng() % 3u == 0u) {
        return static_cast<std::uint32_t>(rng());
    }
    return values[rng() % (sizeof(values) / sizeof(values[0]))];
}

/* Mutate the header block: layout descriptors, data type, correction count, or random bytes. */
void mutate_header(Bytes& bytes, std::mt19937& rng) {
    const std::uint32_t mutations = 1u + rng() % 3u;
    for (std::uint32_t m = 0; m < mutations; ++m) {
        switch (rng() % 5u) {
            case 0:
            case 1:
                /* one of the seven int32 layout fields at 2048..2072 */
                put_u32(bytes, 2048u + 4u * (rng() % 7u), interesting_u32(rng));
                break;
            case 2:
                put_u32(bytes, 4352u, interesting_u32(rng)); /* correction table entry count */
                break;
            case 3:
                bytes[rng() % UNI_TEKTRONIX_R3F_HEADER_SIZE] ^= static_cast<std::uint8_t>(1u << (rng() % 8u));
                break;
            default:
                bytes[rng() % 640u] = static_cast<std::uint8_t>(rng()); /* file id, endian, strings */
                break;
        }
    }
}

void mutate_length(Bytes& bytes, std::mt19937& rng) {
    switch (rng() % 4u) {
        case 0:
            bytes.resize(rng() % (bytes.size() + 1u));
            break;
        case 1:
            bytes.resize(bytes.size() + 1u + rng() % 64u, 0x5au);
            break;
        default:
            break;
    }
}

void exercise_r3f(const std::filesystem::path& path, std::mt19937& rng) {
    uni_tektronix_r3f_reader* reader = nullptr;
    if (uni_tektronix_r3f_reader_open(path.string().c_str(), &reader) != UNI_TEKTRONIX_R3F_STATUS_OK) {
        REQUIRE(reader == nullptr);
        return;
    }
    REQUIRE(reader != nullptr);

    uni_tektronix_r3f_header header{};
    std::uint64_t frames = 0u;
    std::uint64_t samples = 0u;
    REQUIRE(uni_tektronix_r3f_reader_get_header(reader, &header) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_reader_get_frame_count(reader, &frames) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_reader_get_sample_count(reader, &samples) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(header.samples_per_frame > 0);
    REQUIRE(header.correction.table_entry_count <= UNI_TEKTRONIX_R3F_MAX_CORRECTION_TABLE_ENTRIES);
    REQUIRE(samples == frames * static_cast<std::uint64_t>(header.samples_per_frame));

    /* sequential read of everything in random chunk sizes */
    std::vector<int16_t> buffer(20000u);
    std::uint64_t cursor = 0u;
    while (cursor < samples) {
        const std::size_t want = 1u + rng() % buffer.size();
        std::size_t got = 0u;
        REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader, cursor, want, buffer.data(), want, &got) ==
                UNI_TEKTRONIX_R3F_STATUS_OK);
        REQUIRE(got > 0u);
        REQUIRE(got <= want);
        cursor += got;
    }
    REQUIRE(cursor == samples);

    std::size_t got = 0u;
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader, samples, 16u, buffer.data(), 16u, &got) ==
            UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(got == 0u);
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader, samples + 1u, 16u, buffer.data(), 16u, &got) ==
            UNI_TEKTRONIX_R3F_STATUS_OUT_OF_RANGE);

    uni_tektronix_r3f_frame_footer footer{};
    for (std::uint64_t frame = 0u; frame < frames; ++frame) {
        REQUIRE(uni_tektronix_r3f_reader_read_frame_footer(reader, frame, &footer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    }
    REQUIRE(uni_tektronix_r3f_reader_read_frame_footer(reader, frames, &footer) == UNI_TEKTRONIX_R3F_STATUS_OUT_OF_RANGE);

    uni_tektronix_r3f_reader_close(reader);
}

void exercise_r3a(const std::filesystem::path& data_path, const std::filesystem::path& header_path, std::mt19937& rng) {
    uni_tektronix_r3a_reader* reader = nullptr;
    if (uni_tektronix_r3a_reader_open(data_path.string().c_str(), header_path.string().c_str(), &reader) !=
        UNI_TEKTRONIX_R3F_STATUS_OK) {
        REQUIRE(reader == nullptr);
        return;
    }
    REQUIRE(reader != nullptr);

    std::uint64_t samples = 0u;
    REQUIRE(uni_tektronix_r3a_reader_get_sample_count(reader, &samples) == UNI_TEKTRONIX_R3F_STATUS_OK);

    std::vector<int16_t> buffer(10000u);
    std::uint64_t cursor = 0u;
    while (cursor < samples) {
        const std::size_t want = 1u + rng() % buffer.size();
        std::size_t got = 0u;
        REQUIRE(uni_tektronix_r3a_reader_read_samples_i16(reader, cursor, want, buffer.data(), want, &got) ==
                UNI_TEKTRONIX_R3F_STATUS_OK);
        REQUIRE(got > 0u);
        cursor += got;
    }
    REQUIRE(cursor == samples);
    uni_tektronix_r3a_reader_close(reader);
}

std::vector<int16_t> ramp(std::size_t count) {
    std::vector<int16_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<int16_t>(static_cast<std::uint16_t>(i * 7u + 3u));
    }
    return values;
}

} // namespace

TEST_CASE("null reader handles are rejected before use", "[r3f][r3a][api]") {
    uni_tektronix_r3f_frame_footer footer{};
    REQUIRE(uni_tektronix_r3f_reader_read_frame_footer(nullptr, 0u, &footer) == UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT);

    int16_t samples[4]{};
    std::size_t got = 99u;
    REQUIRE(uni_tektronix_r3a_reader_read_samples_i16(nullptr, 0u, 4u, samples, 4u, &got) ==
            UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT);
    REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(nullptr, 0u, 4u, samples, 4u, &got) ==
            UNI_TEKTRONIX_R3F_STATUS_INVALID_ARGUMENT);
}

TEST_CASE("reader serves frames larger than its read chunk", "[r3f][reader]") {
    /* 20000 samples per frame exceeds the internal read chunk, so frames are read in pieces */
    TempPath file("uni_tektronix_fuzz_large_frames.r3f");
    uni_tektronix_r3f_header header{};
    uni_tektronix_r3f_header_init_defaults(&header);
    header.samples_per_frame = 20000;
    header.sample_offset_bytes = 4;
    header.non_sample_offset_bytes = 4 + 2 * 20000;
    header.frame_size_bytes = header.non_sample_offset_bytes + 28;

    const auto values = ramp(3u * 20000u);
    uni_tektronix_r3f_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3f_writer_create(file.path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_append_samples_i16(writer, values.data(), values.size()) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    uni_tektronix_r3f_reader* reader = nullptr;
    REQUIRE(uni_tektronix_r3f_reader_open(file.path.string().c_str(), &reader) == UNI_TEKTRONIX_R3F_STATUS_OK);

    std::mt19937 rng(0x52334655u);
    std::vector<int16_t> buffer(values.size());
    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::size_t start = rng() % values.size();
        const std::size_t want = 1u + rng() % (values.size() - start);
        std::size_t got = 0u;
        REQUIRE(uni_tektronix_r3f_reader_read_samples_i16(reader, start, want, buffer.data(), buffer.size(), &got) ==
                UNI_TEKTRONIX_R3F_STATUS_OK);
        REQUIRE(got == want);
        REQUIRE(std::memcmp(buffer.data(), values.data() + start, want * sizeof(int16_t)) == 0);
    }
    uni_tektronix_r3f_reader_close(reader);
}

TEST_CASE("header declaring a huge frame layout with no frames opens cheaply", "[r3f][reader][validation]") {
    TempPath file("uni_tektronix_fuzz_huge_layout.r3f");
    uni_tektronix_r3f_header header{};
    uni_tektronix_r3f_header_init_defaults(&header);
    TempPath header_file("uni_tektronix_fuzz_huge_layout.r3h");
    REQUIRE(uni_tektronix_r3f_header_write_file(header_file.path.string().c_str(), &header) == UNI_TEKTRONIX_R3F_STATUS_OK);

    Bytes bytes = read_file(header_file.path);
    REQUIRE(bytes.size() == UNI_TEKTRONIX_R3F_HEADER_SIZE);
    put_u32(bytes, 2048u + 4u, UNI_TEKTRONIX_R3F_HEADER_SIZE); /* frame offset */
    put_u32(bytes, 2048u + 8u, 0x7fffffffu);                   /* frame size */
    put_u32(bytes, 2048u + 12u, 0u);                           /* sample offset */
    put_u32(bytes, 2048u + 16u, 0x3fffff00u);                  /* samples per frame */
    put_u32(bytes, 2048u + 20u, 0x7fffffffu - 28u);            /* footer offset */
    put_u32(bytes, 2048u + 24u, 28u);                          /* footer size */
    write_file(file.path, bytes);

    uni_tektronix_r3f_reader* reader = nullptr;
    REQUIRE(uni_tektronix_r3f_reader_open(file.path.string().c_str(), &reader) == UNI_TEKTRONIX_R3F_STATUS_OK);
    std::uint64_t samples = 1u;
    REQUIRE(uni_tektronix_r3f_reader_get_sample_count(reader, &samples) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(samples == 0u);
    uni_tektronix_r3f_reader_close(reader);
}

TEST_CASE("R3F reader survives mutated headers and lengths", "[r3f][reader][fuzz]") {
    TempPath original("uni_tektronix_fuzz_original.r3f");
    TempPath mutated("uni_tektronix_fuzz_mutated.r3f");

    uni_tektronix_r3f_header header{};
    uni_tektronix_r3f_header_init_defaults(&header);
    header.correction.table_entry_count = 3u;
    const auto values = ramp(3u * UNI_TEKTRONIX_R3F_STANDARD_SAMPLES_PER_FRAME);
    uni_tektronix_r3f_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3f_writer_create(original.path.string().c_str(), &header, &writer) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_append_samples_i16(writer, values.data(), values.size()) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    const Bytes pristine = read_file(original.path);
    REQUIRE(pristine.size() == UNI_TEKTRONIX_R3F_HEADER_SIZE + 3u * UNI_TEKTRONIX_R3F_STANDARD_FRAME_SIZE);
    std::mt19937 rng(0x54454B52u);
    exercise_r3f(original.path, rng);

    for (int iteration = 0; iteration < 300; ++iteration) {
        Bytes bytes = pristine;
        mutate_header(bytes, rng);
        mutate_length(bytes, rng);
        write_file(mutated.path, bytes);
        exercise_r3f(mutated.path, rng);
    }
}

TEST_CASE("R3A reader survives mutated headers and lengths", "[r3a][reader][fuzz]") {
    TempPath data("uni_tektronix_fuzz.r3a");
    TempPath header_path("uni_tektronix_fuzz.r3h");
    TempPath mutated_header("uni_tektronix_fuzz_mutated.r3h");

    uni_tektronix_r3f_header header{};
    uni_tektronix_r3f_header_init_defaults(&header);
    const auto values = ramp(5000u);
    uni_tektronix_r3a_writer* writer = nullptr;
    REQUIRE(uni_tektronix_r3a_writer_create(data.path.string().c_str(), header_path.path.string().c_str(), &header, &writer) ==
            UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3a_writer_append_samples_i16(writer, values.data(), values.size()) == UNI_TEKTRONIX_R3F_STATUS_OK);
    REQUIRE(uni_tektronix_r3a_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK);

    const Bytes pristine_header = read_file(header_path.path);
    const Bytes pristine_data = read_file(data.path);

    std::mt19937 rng(0x52334148u);
    for (int iteration = 0; iteration < 200; ++iteration) {
        Bytes header_bytes = pristine_header;
        Bytes data_bytes = pristine_data;
        mutate_header(header_bytes, rng);
        if (rng() % 2u == 0u) {
            mutate_length(header_bytes, rng);
        }
        mutate_length(data_bytes, rng);
        write_file(mutated_header.path, header_bytes);
        write_file(data.path, data_bytes);
        exercise_r3a(data.path, mutated_header.path, rng);
    }
}
