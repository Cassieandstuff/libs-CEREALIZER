# sct-serial

`reflect()`-driven serialization with a pluggable backend, plus a compact,
validated binary file format. Header-only C++23, standard library only.

A type is serializable when it lists its fields once:

```cpp
struct Note {
    int         pitch = 60;
    std::string label;
    template <class V> void reflect(V& v) { v("pitch", pitch); v("label", label); }
};
```

The same list drives saving and loading, so a field can't be written but not
read. Keys match by name: a key missing on load leaves the field at its default,
so older files keep opening when fields are added.

| Header | What |
|---|---|
| `<sct-serial/SctSerial.h>` | `sct::serial::Writer` / `Reader` / `write` / `read`, the Backend concept, `MemoryTree` (a pure-std in-memory backend), `roundTrip()` (the drift gate: `roundTrip(x) == x` iff every field is in `reflect()` both ways) |
| `<sct-serial/SctSerialFile.h>` | `sct::serial::file::save` / `load` — the "SCT1" binary format (magic + size cap on read, bounds-checked parsing, atomic temp-file + rename write, bit-exact doubles) — and `dumpText` for diffing |

Supported field shapes: arithmetic, `bool`, enums, `std::string`, nested
reflectables, `std::vector<T>` and `std::map<K, V>` of any of these (nesting
included). A different store (JSON, YAML, …) is a Backend: two handle types and
eleven static functions, documented at the top of `SctSerial.h`.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug
```

As a subdirectory, link `sct::serial`; tests and install rules are off unless
this is the top-level project (`SCT_SERIAL_BUILD_TESTS`, `SCT_SERIAL_INSTALL`).
Installed, it is found with `find_package(sct-serial CONFIG REQUIRED)`.

## History

Split out of `sct-state` (Skyrim Content Tools). The model and the file format
are unchanged, so files written before the split load as-is.
