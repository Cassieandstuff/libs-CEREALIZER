// ============================================================================
// sct-serial unit test — the gate for the reflect() serializer.
//
// Round-trips every field shape (arithmetic, bool, enum, string, nested object,
// vector<object>, scalar/string vectors, maps of scalars / reflectables /
// vectors, nested maps) through MemoryTree, checks the missing-key-keeps-default
// rule, and gates the "SCT1" binary file format: round-trip, header bytes, and
// rejection of a foreign file. Pure std, no deps. Console app; exit 0 = all passed.
// ============================================================================

#include <sct-serial/SctSerial.h>
#include <sct-serial/SctSerialFile.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    int failures = 0;

    void check(const char* label, bool ok)
    {
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << label << '\n';
        if (!ok) ++failures;
    }

    // Reflectable types for the serialization round-trip (exercise every leaf
    // kind: arithmetic, bool, enum, string, nested object, vector<object>).
    enum class Mode { major, minor, chromatic };

    struct Note
    {
        int         pitch = 60;
        double      start = 0.0;
        std::string label;
        bool operator==(const Note& o) const { return pitch == o.pitch && start == o.start && label == o.label; }
        template <class V> void reflect(V& v) { v("pitch", pitch); v("start", start); v("label", label); }
    };

    struct Song
    {
        double            tempo = 120.0;
        bool              loop  = false;
        Mode              mode  = Mode::major;
        std::string       name;
        std::vector<Note> notes;
        bool operator==(const Song& o) const
        {
            return tempo == o.tempo && loop == o.loop && mode == o.mode
                && name == o.name && notes == o.notes;
        }
        template <class V> void reflect(V& v)
        {
            v("tempo", tempo); v("loop", loop); v("mode", mode);
            v("name", name);   v("notes", notes);
        }
    };

    // Map + scalar-vector coverage — the shapes the Scene Editor model needs:
    // vector<string>/vector<int>, map<string,scalar>, map<*,reflectable>,
    // map<string,vector<int>> (beatPhysicsStaged), nested map (scenePlacements).
    struct Model
    {
        std::vector<std::string>                   tags;        // string vector
        std::vector<int>                           ids;         // scalar vector
        std::map<std::string, float>               weights;     // string → scalar
        std::map<std::string, Note>                byName;      // string → reflectable
        std::map<int, Note>                        byId;        // int → reflectable
        std::map<std::string, std::vector<int>>    staged;      // string → scalar vector
        std::map<std::string, std::map<int, Note>> placements;  // nested map
        bool operator==(const Model& o) const
        {
            return tags == o.tags && ids == o.ids && weights == o.weights
                && byName == o.byName && byId == o.byId && staged == o.staged
                && placements == o.placements;
        }
        template <class V> void reflect(V& v)
        {
            v("tags", tags); v("ids", ids); v("weights", weights);
            v("byName", byName); v("byId", byId); v("staged", staged);
            v("placements", placements);
        }
    };
}

int main()
{
    using namespace sct;

    // --- serialization round-trip (backend abstraction via MemoryTree) -------
    {
        Song s;
        s.tempo = 140.5;
        s.loop  = true;
        s.mode  = Mode::minor;
        s.name  = "Dragon Break";
        s.notes = { { 62, 0.0, "a" }, { 65, 1.5, "b" }, { 69, 2.25, "" } };

        Song rt = serial::roundTrip(s);
        check("serial: full round-trip equal", rt == s);
        check("serial: enum preserved",        rt.mode == Mode::minor);
        check("serial: bool preserved",        rt.loop == true);
        check("serial: string preserved",      rt.name == "Dragon Break");
        check("serial: vector size",           rt.notes.size() == 3);
        check("serial: nested fields",         rt.notes.size() == 3
                                               && rt.notes[1].pitch == 65
                                               && rt.notes[1].start == 1.5
                                               && rt.notes[2].label.empty());

        // Forward-compat: reading into a default-constructed target from an empty
        // tree leaves every field at its default (missing key → keep default).
        Song def;
        serial::MemoryTree::Obj empty;
        Song fromEmpty;
        serial::read<serial::MemoryTree>(&empty, fromEmpty);
        check("serial: empty tree keeps defaults", fromEmpty == def);
    }

    // --- map + scalar-vector round-trip (the Scene Editor model shapes) -------
    {
        Model m;
        m.tags    = { "act1", "throne", "escape" };
        m.ids     = { 3, 1, 4, 1, 5 };
        m.weights = { { "jawOpen", 0.5f }, { "eyeBlinkLeft", 1.0f } };
        m.byName  = { { "lydia", { 60, 0.0, "L" } }, { "player", { 62, 1.0, "P" } } };
        m.byId    = { { 7, { 64, 2.0, "x" } }, { 42, { 65, 3.0, "y" } } };
        m.staged  = { { "sceneA/1", { 1, 2, 3 } }, { "sceneA/2", {} } };
        m.placements = { { "sceneA", { { 0, { 60, 0.0, "a" } }, { 1, { 61, 0.0, "b" } } } },
                         { "sceneB", { { 5, { 62, 0.0, "c" } } } } };

        Model rt = serial::roundTrip(m);
        check("serial: string-vector round-trip",  rt.tags == m.tags);
        check("serial: scalar-vector round-trip",  rt.ids == m.ids);
        check("serial: map<string,scalar>",        rt.weights == m.weights);
        check("serial: map<string,reflectable>",   rt.byName == m.byName);
        check("serial: map<int,reflectable>",      rt.byId == m.byId);
        check("serial: map<string,vector<int>>",   rt.staged == m.staged);
        check("serial: nested map",                rt.placements == m.placements);
        check("serial: model full equal",          rt == m);
    }

    // --- binary file round-trip (SctSerialFile.h) -------------------
    {
        Model m;
        m.tags    = { "act1", "throne" };
        m.ids     = { 9, 8, 7 };
        m.weights = { { "jawOpen", 0.25f } };
        m.byName  = { { "lydia", { 60, 0.0, "L" } } };
        m.byId    = { { 3, { 61, 1.0, "z" } } };
        m.staged  = { { "sceneA/1", { 4, 5 } } };
        m.placements = { { "sceneA", { { 0, { 62, 0.0, "p" } } } } };

        const std::string path =
            (std::filesystem::temp_directory_path() / "sct_serial_file_test.bin").string();

        const bool wrote  = serial::file::save(path, m);
        Model back;
        const bool readOk = serial::file::load(path, back);
        check("file: save ok",            wrote);
        check("file: load ok",            readOk);
        check("file: round-trip equal",   back == m);

        // A non-magic file is rejected before parsing → target left at defaults.
        { std::ofstream junk(path, std::ios::binary | std::ios::trunc); const char* g = "NOPE1234"; junk.write(g, 8); }
        Model j;
        check("file: junk rejected",      !serial::file::load(path, j));
        check("file: junk keeps default", j == Model{});

        std::error_code ec; std::filesystem::remove(path, ec);
    }

    // --- the file header is the "SCT1" magic + version 1 (format compatibility) -
    {
        Song s;
        s.name = "header";
        serial::MemoryTree::Obj root;
        serial::write<serial::MemoryTree>(&root, s);
        const auto bytes = serial::file::toBytes(root);
        check("file: header magic is SCT1", bytes.size() >= 8 && bytes[0] == 0x31 && bytes[1] == 0x54
                                            && bytes[2] == 0x43 && bytes[3] == 0x53);
        check("file: header version is 1",  bytes.size() >= 8 && bytes[4] == 1 && bytes[5] == 0
                                            && bytes[6] == 0 && bytes[7] == 0);
    }

    std::cout << (failures == 0 ? "\nALL PASSED\n" : "\nFAILURES\n");
    return failures;
}
