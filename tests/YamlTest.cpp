// ============================================================================
// sct-serial YAML test — the gate for SctSerialYaml.h.
//
// Every field shape round-trips through YAML text, a save → load → save is
// byte-identical, the text is the YAML a person would write (maps as mappings,
// leaf vectors as scalar sequences, strings quoted only when YAML would misread
// them), hand-written YAML (comments, flow collections, a sequence at its key's
// indent, quoted keys) loads, and what isn't modelled is refused with its line.
// Pure std, no deps. Console app; exit 0 = all passed.
// ============================================================================

#include <sct-serial/SctSerialYaml.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void check (const char* label, bool ok)
    {
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << label << '\n';
        if (! ok) ++failures;
    }

    enum class Mode { major, minor };

    struct Vec
    {
        float x = 0, y = 0, z = 0;
        template <class V> void reflect (V& v) { v ("x", x); v ("y", y); v ("z", z); }
        bool operator== (const Vec&) const = default;
    };

    struct Role
    {
        std::string name;
        std::string actor;
        Vec         offset;
        template <class V> void reflect (V& v) { v ("name", name); v ("actor", actor); v ("offset", offset); }
        bool operator== (const Role&) const = default;
    };

    struct Doc
    {
        int                                        version = 1;
        bool                                       loop    = false;
        Mode                                       mode    = Mode::major;
        double                                     length  = 0;
        std::uint8_t                               small   = 0;
        std::int64_t                               big     = 0;
        std::string                                name;
        Vec                                        anchor;
        std::vector<std::string>                   tags;
        std::vector<int>                           counts;
        std::vector<Role>                          list;
        std::map<std::string, Role>                roles;
        std::map<int, std::string>                 byNumber;
        std::map<std::string, std::vector<double>> curves;
        std::map<std::string, std::map<std::string, int>> nested;
        template <class V> void reflect (V& v)
        {
            v ("version", version);
            v ("loop", loop);
            v ("mode", mode);
            v ("length", length);
            v ("small", small);
            v ("big", big);
            v ("name", name);
            v ("anchor", anchor);
            v ("tags", tags);
            v ("counts", counts);
            v ("list", list);
            v ("roles", roles);
            v ("byNumber", byNumber);
            v ("curves", curves);
            v ("nested", nested);
        }
        bool operator== (const Doc&) const = default;
    };

    Doc full()
    {
        Doc d;
        d.version  = 3;
        d.loop     = true;
        d.mode     = Mode::minor;
        d.length   = 12.5;
        d.small    = 200;
        d.big      = -9007199254740993LL;
        d.name     = "Arrival: the gate";
        d.anchor   = { 0.1f, -2.5f, 1e30f };
        d.tags     = { "0001", "true", "", " padded", "-dash", "#hash", "null", "line\nbreak", "tab\there", "quote\"s",
                       "Skyrim.esm|0x0F80FB", "NPC Head [Head]", "a, b", "caf\xC3\xA9", "plain words" };
        d.counts   = { 0, -1, 42 };
        d.list     = { { "Hulda", "013BA3:Skyrim.esm", { 1, 2, 3 } }, {} };
        d.roles    = { { "0001", { "Hero", "player", {} } }, { "two words", { "B", "", { 0, 0, 7 } } } };
        d.byNumber = { { 5, "five" }, { -1, "minus" } };
        d.curves   = { { "fov", { 0.0, 0.5, 1.0 } }, { "empty", {} } };
        d.nested   = { { "a", { { "x", 1 } } }, { "b", {} } };
        return d;
    }

    template <class T> bool roundTrips (const T& in, std::string* textOut = nullptr)
    {
        T          copy = in;
        const auto text = sct::serial::yaml::save (copy);
        if (textOut) *textOut = text;
        T    out {};
        auto r = sct::serial::yaml::load (text, out);
        if (! r)
        {
            std::cout << "  load failed: " << r.error().what() << "\n--- text ---\n" << text << "------------\n";
            return false;
        }
        if (! (out == in))
        {
            std::cout << "--- text ---\n" << text << "------------\n";
            return false;
        }
        return sct::serial::yaml::save (out) == text;   // byte-identical second save
    }

    template <class T> std::string errorOf (std::string_view text)
    {
        T    out {};
        auto r = sct::serial::yaml::load (text, out);
        return r ? std::string() : r.error().what();
    }
} // namespace

int main()
{
    using namespace sct::serial;

    // ── round trip ───────────────────────────────────────────────────────────
    std::string text;
    check ("every field shape round-trips, and a second save is byte-identical", roundTrips (full(), &text));
    check ("a default value round-trips", roundTrips (Doc {}));

    // ── the text a person reads ──────────────────────────────────────────────
    {
        Vec         v { 1, 0.5f, -2 };
        std::string out = yaml::save (v);
        check ("an object is a block mapping in reflect() order", out == "x: 1\ny: 0.5\nz: -2\n");

        Role r { "Hero", "player", {} };
        check ("a nested object indents by two", yaml::save (r) == "name: Hero\nactor: player\noffset:\n  x: 0\n  y: 0\n  z: 0\n");
    }
    check ("a string map is a mapping with quoted numeric keys", text.find ("roles:\n  \"0001\":\n    name: Hero\n") != std::string::npos);
    check ("a key with spaces stays plain", text.find ("\n  two words:\n") != std::string::npos);
    check ("a leaf vector is a scalar sequence", text.find ("counts:\n  - 0\n  - -1\n  - 42\n") != std::string::npos);
    check ("a vector of objects is a sequence of mappings", text.find ("list:\n  - name: Hulda\n    actor: 013BA3:Skyrim.esm\n") != std::string::npos);
    check ("an empty string is \"\"", text.find ("  - name: \"\"\n") != std::string::npos);
    check ("an empty list is []", text.find ("empty: []\n") != std::string::npos);
    check ("an empty map is [] too (the model can't tell them apart)", text.find ("  b: []\n") != std::string::npos);
    check ("an int-keyed map quotes its keys", text.find ("byNumber:\n  \"-1\": minus\n  \"5\": five\n") != std::string::npos);
    check ("strings YAML would misread are quoted", text.find ("  - \"0001\"\n  - \"true\"\n  - \"\"\n  - \" padded\"\n  - \"-dash\"\n  - \"#hash\"\n  - \"null\"\n") != std::string::npos);
    check ("control characters are escaped", text.find ("  - \"line\\nbreak\"\n  - \"tab\\there\"\n") != std::string::npos);
    check ("ordinary text stays plain", text.find ("  - Skyrim.esm|0x0F80FB\n") != std::string::npos &&
                                            text.find ("  - plain words\n") != std::string::npos &&
                                            text.find ("  - caf\xC3\xA9\n") != std::string::npos &&
                                            text.find ("    actor: 013BA3:Skyrim.esm\n") != std::string::npos);
    check ("brackets and commas are quoted", text.find ("  - \"NPC Head [Head]\"\n  - \"a, b\"\n") != std::string::npos);
    check ("an enum is its number, a bool is true/false", text.find ("loop: true\nmode: 1\n") != std::string::npos);
    check ("a float is its shortest form", text.find ("  x: 0.1\n") != std::string::npos && text.find ("  z: 1e+30\n") != std::string::npos);

    // ── special numbers ──────────────────────────────────────────────────────
    {
        Vec v { std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() };
        const std::string out = yaml::save (v);
        Vec               back {};
        const bool        ok = yaml::load (out, back).has_value();
        check ("inf and nan are .inf / -.inf / .nan", out == "x: .inf\ny: -.inf\nz: .nan\n" && ok && std::isinf (back.x) && back.y < 0 &&
                                                         std::isnan (back.z));
        Vec neg { -0.0f, 0, 0 };
        check ("negative zero round-trips", roundTrips (neg) && std::signbit (neg.x));
    }

    // ── what a person writes ─────────────────────────────────────────────────
    {
        const char* hand = R"(---
# a hand-written file
version: 7          # trailing comment
name: 'it''s here'
anchor: {x: 1, y: 2.5, z: -3}
tags: [a, "b c", 'd']
counts:
- 1
- 2
list:
  - name: Hulda   # a role
    offset:
      z: 4
  -
    name: Second
roles:
  "0001": {name: Hero, actor: player}
  plain:
    name: P
byNumber:
  7: seven
loop: True
)";
        Doc  d;
        auto r = yaml::load (hand, d);
        if (! r) std::cout << "  " << r.error().what() << '\n';
        check ("hand-written YAML loads", r.has_value());
        check ("comments, quotes and flow values read", d.version == 7 && d.name == "it's here" && d.anchor == Vec { 1, 2.5f, -3 } &&
                                                             d.tags == std::vector<std::string> { "a", "b c", "d" } && d.loop);
        check ("a sequence may sit at its key's indent", d.counts == std::vector<int> { 1, 2 });
        check ("sequence items read as objects, '-' alone included", d.list.size() == 2 && d.list[0].name == "Hulda" && d.list[0].offset.z == 4 &&
                                                                          d.list[1].name == "Second");
        check ("a mapping reads into a std::map", d.roles.size() == 2 && d.roles["0001"].actor == "player" && d.roles["plain"].name == "P" &&
                                                     d.byNumber == std::map<int, std::string> { { 7, "seven" } });
        check ("a key the file lacks keeps its default", d.mode == Mode::major && d.length == 0);
        check ("a map written as {k, v} items still reads", [] {
            Doc d2;
            return yaml::load ("byNumber:\n  - k: 3\n    v: three\n", d2).has_value() && d2.byNumber.at (3) == "three";
        }());
    }

    // ── what it refuses ──────────────────────────────────────────────────────
    check ("a tab in the indentation is refused, with its line", errorOf<Doc> ("version: 1\nanchor:\n\tx: 1\n").starts_with ("line 3:"));
    check ("a word where a number goes is refused", errorOf<Doc> ("version: seven\n").starts_with ("line 1:"));
    check ("a number out of range is refused", ! errorOf<Doc> ("small: 300\n").empty());
    check ("a duplicate key is refused", errorOf<Doc> ("name: a\nname: b\n").starts_with ("line 2:"));
    check ("a block scalar is refused", ! errorOf<Doc> ("name: |\n  text\n").empty());
    check ("an anchor is refused", ! errorOf<Doc> ("name: &a x\n").empty());
    check ("a list where an object goes is refused", ! errorOf<Doc> ("anchor: [1, 2]\n").empty());
    check ("an unclosed quote is refused", ! errorOf<Doc> ("name: \"open\n").empty());
    check ("a top-level sequence is refused", ! errorOf<Doc> ("- a\n- b\n").empty());
    check ("a bad indent is refused", ! errorOf<Doc> ("anchor:\n    x: 1\n  y: 2\n").empty());
    check ("an empty document is an empty object", [] {
        Doc d;
        return yaml::load ("", d).has_value() && yaml::load ("# nothing\n", d).has_value() && yaml::load ("{}\n", d).has_value();
    }());

    // ── files ────────────────────────────────────────────────────────────────
    {
        const auto path = std::filesystem::temp_directory_path() / "sct-serial-yaml-test.yaml";
        Doc        in   = full();
        Doc        out;
        const bool ok = yaml::saveFile (path, in).has_value() && yaml::loadFile (path, out).has_value() && out == in &&
                        ! std::filesystem::exists (std::filesystem::path (path).concat (".tmp"));
        check ("saveFile / loadFile round-trip and leave no temp file", ok);
        std::error_code ec;
        std::filesystem::remove (path, ec);
        check ("loadFile names a missing file", ! yaml::loadFile (path, out).has_value());
    }

    std::cout << (failures ? "FAILED: " : "all passed") << (failures ? std::to_string (failures) : std::string()) << '\n';
    return failures ? 1 : 0;
}
