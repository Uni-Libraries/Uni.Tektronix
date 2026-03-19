# uni_tektronix

`uni_tektronix` is a C23 library and CLI for reading, generating, and converting Tektronix `R3F` / `R3A` / `R3H` files.

What the project includes:

- seekable reader: request a sample range by `start + count` without loading the entire file into memory;
- streaming writer: append samples incrementally while the library assembles complete `R3F` frames automatically;
- reading and generation of the 16 KB header block, frame footers, and correction tables;
- raw-pair API for `R3A` + `R3H`;
- CLI tool for conversions and basic capture operations;
- exported API via `GenerateExportHeader` from CMake;
- Catch2 unit tests.

## What the library supports

### R3F API

- `uni_tektronix_r3f_reader_*` — seekable `.r3f` reading;
- `uni_tektronix_r3f_writer_*` — streaming `.r3f` generation;
- `uni_tektronix_r3f_header_read_file()` / `uni_tektronix_r3f_header_write_file()` — reading/writing standalone header files;
- `uni_tektronix_r3f_header_set_framed_layout()` / `uni_tektronix_r3f_header_set_raw_layout()` — switch the header between framed and raw layouts.

### R3A/R3H API

- `uni_tektronix_r3a_reader_*` — reading a raw `.r3a` + `.r3h` pair;
- `uni_tektronix_r3a_writer_*` — generating a raw pair;
- the API can automatically infer the neighboring file (`capture.r3a` <-> `capture.r3h`).

## Where the format model comes from

The implementation is based on two sources:

1. the reference Python/Cython code from the attached archive (`r3x_reader.py`, `r3x_reader_p2.py`, `cython/r3x_converter.pyx`);
2. the public format description from the Tektronix RSA API Programmer Manual.

As a result:

- seekable sample reading follows the reference-code logic: the file consists of a 16 KB header and identical framed blocks, where data is taken from the sample region and the footer is skipped;
- additional version 1.1/1.2 header fields (`device_nomenclature`, `ref_time_source`, `start_sample_count`, `start_wall_time`) are taken from the official description and are also supported;
- the footer is parsed in the same form as in the reference code: frame counter, trigger indexes, status word, and timestamp;
- correction tables are exposed in the order defined by the official specification: `frequency -> amplitude -> phase`.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## CLI tool

After building, the `uni_tektronix_tool` executable will be available.

```bash
uni_tektronix_tool info capture.r3f
uni_tektronix_tool validate capture.r3f
uni_tektronix_tool split capture.r3f capture.r3a
uni_tektronix_tool pack capture.r3a repacked.r3f
uni_tektronix_tool extract-raw capture.r3f samples.i16 --start 1000 --count 8192
uni_tektronix_tool trim capture.r3f trimmed.r3f --start 5000 --count 100000 --tail-mode pad --pad-value 0
uni_tektronix_tool trim capture.r3f trimmed.r3a --start 5000 --count 100000
uni_tektronix_tool to-wav capture.r3f capture_iq.wav --decimate 4
uni_tektronix_tool export-footers capture.r3f footers.csv
```

Supported operations:

- `info` — print the main file metadata;
- `validate` — perform structural file validation;
- `split` / `to-raw` — convert `R3F -> R3A + R3H`;
- `pack` / `to-r3f` — convert `R3A + R3H -> R3F`;
- `extract-raw` / `to-i16` — export a sample range as flat little-endian `int16`;
- `trim` — cut a range into `R3F` or a raw pair;
- `to-wav` / `wav` — convert IF samples into stereo IQ WAV for SDR viewing;
- `export-footers` — write a CSV dump of `.r3f` frame footers.

### `tail-mode` when packing into `R3F`

`R3F` stores only complete frames, so the following modes are available for the final incomplete data:

- `pad` — pad the frame with the `--pad-value` value;
- `drop` — discard the tail;
- `error` — stop the operation with an error.

### WAV for SDRSharp

The `to-wav` command performs a basic digital downconversion pipeline:

1. real IF -> complex baseband;
2. FIR low-pass filtering;
3. optional integer decimation (`--decimate`);
4. write a standard stereo 16-bit PCM WAV (`I` in the left channel, `Q` in the right channel).

With `--decimate 1`, the original sample rate is preserved. When the sample rate is reduced, the CLI warns if the resulting rate is below the declared capture bandwidth.

## Quick reading example

```c
#include "uni_tektronix_r3f.h"
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uni_tektronix_r3f_reader* reader = NULL;
    if (uni_tektronix_r3f_reader_open("capture.r3f", &reader) != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return 1;
    }

    int16_t samples[1024];
    size_t read_count = 0;
    if (uni_tektronix_r3f_reader_read_samples_i16(reader, 10000u, 1024u, samples, 1024u, &read_count)
        == UNI_TEKTRONIX_R3F_STATUS_OK) {
        printf("read %zu samples\n", read_count);
    }

    uni_tektronix_r3f_reader_close(reader);
    return 0;
}
```

## Quick `R3F` generation example

```c
#include "uni_tektronix_r3f.h"
#include <stdint.h>

int main(void) {
    uni_tektronix_r3f_header header;
    uni_tektronix_r3f_writer* writer = NULL;
    int16_t chunk[4096] = {0};

    uni_tektronix_r3f_header_init_defaults(&header);
    header.rf_center_frequency_hz = 433.92e6;
    header.reference_level_dbm = -20.0;
    header.start_sample_count = 123456789ull;

    if (uni_tektronix_r3f_writer_create("out.r3f", &header, &writer) != UNI_TEKTRONIX_R3F_STATUS_OK) {
        return 1;
    }

    (void)uni_tektronix_r3f_writer_append_samples_i16(writer, chunk, 4096u);
    (void)uni_tektronix_r3f_writer_append_samples_i16(writer, chunk, 4096u);

    /* R3F stores only complete frames, so the tail must be padded explicitly. */
    (void)uni_tektronix_r3f_writer_flush_partial_frame(writer, 0, NULL);
    return uni_tektronix_r3f_writer_close(writer) == UNI_TEKTRONIX_R3F_STATUS_OK ? 0 : 1;
}
```

## Limitations

- the library is specifically focused on `16-bit IF samples` (`file_data_type = 161` / 2 bytes per sample);
- the writer generates structurally valid files, but the auto-generated footer does not try to reconstruct all hidden firmware semantics — trigger-related fields remain zero by default;
- `trim` updates `start_sample_count`, but does not attempt to calculate new wall-clock/UTC timestamps from the sample clock;
- CLI `to-wav` produces a simple compatible stereo PCM WAV without vendor-specific metadata chunks.
