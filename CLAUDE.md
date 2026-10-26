# SERIALIZER — working notes

`reflect()`-driven serialization with a pluggable backend, plus the "SCT1" binary file format. Header-only, pure std.
Moved unchanged from the Scene Editor's `libs/sct-serial` (2026-10-26; GitHub: libs-CEREALIZER), so its target and namespace stay
`sct::serial`: the Scene Editor can fetch this repo in place of its copy with no code change. Behavior Kit's project
files (`.bkproj`) are written with it.

## Rules
- Consumers link `sct::serial` and include `<sct-serial/SctSerial.h>` / `<sct-serial/SctSerialFile.h>` only.
- A type lists its fields once in `reflect()`; the same list saves and loads. A key missing on load leaves the field
  at its default, so a field added later keeps older files opening: never rename or reuse a key.
- `roundTrip(x) == x` is the drift gate for every reflectable type a consumer adds.
