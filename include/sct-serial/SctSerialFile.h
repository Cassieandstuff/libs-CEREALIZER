#pragma once
// ============================================================================
// sct-serial file layer — the NATIVE file format for the reflect() serializer.
// Serializes a MemoryTree::Obj (what serial::write builds) to a compact
// little-endian binary file (the ship format), plus a write-only indented text
// dump for diffing. Pure std, no framework/format dependency. Safe-I/O patterns:
// a magic tag + size cap on read (reject arbitrary/hostile files before
// parsing), length validation against the remaining buffer (no trust in embedded
// counts), an atomic temp-file + rename write (a crash mid-save can't corrupt the
// prior good file), and bit-exact doubles.
//
//   Save:  sct::serial::file::save(path, reflectableObj)
//   Load:  sct::serial::file::load(path, reflectableObj)   // false → left at defaults
//
// The format ("SCT1", version 1) is unchanged from sct-state's file layer, so files
// written before the split load as-is.
// ============================================================================
#include <sct-serial/SctSerial.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace sct::serial::file
{
    constexpr std::uint32_t kMagic    = 0x53435431u;          // "SCT1"
    constexpr std::uint32_t kVersion  = 1u;
    constexpr std::uint64_t kMaxBytes = 512ull * 1024 * 1024; // sane upper bound

    // ── write helpers (append LE to a byte buffer) ───────────────────────────
    inline void putU32 (std::vector<std::uint8_t>& b, std::uint32_t v)
    {
        b.push_back (std::uint8_t (v));         b.push_back (std::uint8_t (v >> 8));
        b.push_back (std::uint8_t (v >> 16));   b.push_back (std::uint8_t (v >> 24));
    }
    inline void putU64 (std::vector<std::uint8_t>& b, std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) b.push_back (std::uint8_t (v >> (8 * i)));
    }
    inline void putStr (std::vector<std::uint8_t>& b, const std::string& s)
    {
        putU32 (b, static_cast<std::uint32_t> (s.size()));
        b.insert (b.end(), s.begin(), s.end());
    }
    inline void putScalar (std::vector<std::uint8_t>& b, const MemoryTree::Scalar& s)
    {
        if (auto* p = std::get_if<std::int64_t> (&s))     { b.push_back (0); putU64 (b, static_cast<std::uint64_t> (*p)); }
        else if (auto* d = std::get_if<double> (&s))      { std::uint64_t u; std::memcpy (&u, d, 8); b.push_back (1); putU64 (b, u); }
        else if (auto* q = std::get_if<bool> (&s))        { b.push_back (2); b.push_back (*q ? 1 : 0); }
        else if (auto* t = std::get_if<std::string> (&s)) { b.push_back (3); putStr (b, *t); }
    }
    inline void putObj (std::vector<std::uint8_t>& b, const MemoryTree::Obj& o)
    {
        putU32 (b, static_cast<std::uint32_t> (o.scalars.size()));
        for (auto& [k, v] : o.scalars) { putStr (b, k); putScalar (b, v); }
        putU32 (b, static_cast<std::uint32_t> (o.objects.size()));
        for (auto& [k, c] : o.objects) { putStr (b, k); putObj (b, c); }
        putU32 (b, static_cast<std::uint32_t> (o.lists.size()));
        for (auto& [k, items] : o.lists)
        {
            putStr (b, k);
            putU32 (b, static_cast<std::uint32_t> (items.size()));
            for (auto& it : items) putObj (b, it);
        }
    }

    // ── read helpers (bounds-checked cursor; any overrun trips `ok`) ──────────
    struct Cur { const std::uint8_t* p; const std::uint8_t* end; bool ok = true; };

    inline std::uint32_t getU32 (Cur& c)
    {
        if (c.end - c.p < 4) { c.ok = false; return 0; }
        std::uint32_t v = std::uint32_t (c.p[0]) | (std::uint32_t (c.p[1]) << 8)
                        | (std::uint32_t (c.p[2]) << 16) | (std::uint32_t (c.p[3]) << 24);
        c.p += 4; return v;
    }
    inline std::uint64_t getU64 (Cur& c)
    {
        if (c.end - c.p < 8) { c.ok = false; return 0; }
        std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= std::uint64_t (c.p[i]) << (8 * i);
        c.p += 8; return v;
    }
    inline std::string getStr (Cur& c)
    {
        std::uint32_t n = getU32 (c);
        if (! c.ok || static_cast<std::uint64_t> (c.end - c.p) < n) { c.ok = false; return {}; }
        std::string s (reinterpret_cast<const char*> (c.p), n); c.p += n; return s;
    }
    inline MemoryTree::Scalar getScalar (Cur& c)
    {
        if (c.p >= c.end) { c.ok = false; return {}; }
        std::uint8_t tag = *c.p++;
        switch (tag)
        {
            case 0: return MemoryTree::Scalar { static_cast<std::int64_t> (getU64 (c)) };
            case 1: { std::uint64_t u = getU64 (c); double d; std::memcpy (&d, &u, 8); return MemoryTree::Scalar { d }; }
            case 2: { if (c.p >= c.end) { c.ok = false; return {}; } return MemoryTree::Scalar { static_cast<bool> (*c.p++ != 0) }; }
            case 3: return MemoryTree::Scalar { getStr (c) };
            default: c.ok = false; return {};
        }
    }
    inline void getObj (Cur& c, MemoryTree::Obj& o)
    {
        std::uint32_t ns = getU32 (c);
        for (std::uint32_t i = 0; i < ns && c.ok; ++i) { std::string k = getStr (c); if (! c.ok) break; o.scalars[k] = getScalar (c); }
        std::uint32_t no = getU32 (c);
        for (std::uint32_t i = 0; i < no && c.ok; ++i) { std::string k = getStr (c); if (! c.ok) break; getObj (c, o.objects[k]); }
        std::uint32_t nl = getU32 (c);
        for (std::uint32_t i = 0; i < nl && c.ok; ++i)
        {
            std::string k = getStr (c); if (! c.ok) break;
            std::uint32_t ni = getU32 (c);
            auto& items = o.lists[k];
            for (std::uint32_t j = 0; j < ni && c.ok; ++j) { items.emplace_back(); getObj (c, items.back()); }
        }
    }

    // ── bytes ↔ Obj ──────────────────────────────────────────────────────────
    inline std::vector<std::uint8_t> toBytes (const MemoryTree::Obj& root)
    {
        std::vector<std::uint8_t> b;
        putU32 (b, kMagic); putU32 (b, kVersion);
        putObj (b, root);
        return b;
    }
    inline bool fromBytes (const std::uint8_t* data, std::size_t n, MemoryTree::Obj& out)
    {
        if (n < 8 || n > kMaxBytes) return false;
        Cur c { data, data + n };
        if (getU32 (c) != kMagic) return false;     // not our file → reject before parsing
        (void) getU32 (c);                          // version (accepted; future migrations branch here)
        getObj (c, out);
        return c.ok;
    }

    // ── safe file I/O (atomic write, guarded read) ───────────────────────────
    inline bool writeFile (const std::string& path, const MemoryTree::Obj& root)
    {
        std::vector<std::uint8_t> bytes = toBytes (root);
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
            if (! out) return false;
            out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
            out.flush();
            if (! out) return false;                // write/flush failed → leave prior good file untouched
        }
        std::error_code ec;
        std::filesystem::rename (tmp, path, ec);    // atomic replace
        if (ec) { std::filesystem::remove (tmp, ec); return false; }
        return true;
    }
    inline bool readFile (const std::string& path, MemoryTree::Obj& out)
    {
        std::ifstream in (path, std::ios::binary | std::ios::ate);
        if (! in) return false;
        const std::streamoff sz = in.tellg();
        if (sz < 8 || static_cast<std::uint64_t> (sz) > kMaxBytes) return false;
        in.seekg (0);
        std::vector<std::uint8_t> buf (static_cast<std::size_t> (sz));
        in.read (reinterpret_cast<char*> (buf.data()), sz);
        if (! in) return false;
        return fromBytes (buf.data(), buf.size(), out);
    }

    // ── reflectable ↔ file ───────────────────────────────────────────────────
    template <class T> bool save (const std::string& path, T& obj)
    {
        MemoryTree::Obj root; write<MemoryTree> (&root, obj);
        return writeFile (path, root);
    }
    template <class T> bool load (const std::string& path, T& obj)
    {
        MemoryTree::Obj root;
        if (! readFile (path, root)) return false;  // missing/invalid → caller keeps defaults
        read<MemoryTree> (&root, obj);
        return true;
    }

    // ── write-only indented text dump (for diffing a saved project) ───────────
    inline void dumpText (const MemoryTree::Obj& o, std::ostream& os, int indent = 0)
    {
        const std::string pad (static_cast<std::size_t> (indent) * 2, ' ');
        for (auto& [k, v] : o.scalars)
        {
            os << pad << k << " = ";
            std::visit ([&] (auto&& s) { os << s; }, v);
            os << '\n';
        }
        for (auto& [k, c] : o.objects) { os << pad << k << ":\n"; dumpText (c, os, indent + 1); }
        for (auto& [k, items] : o.lists)
        {
            os << pad << k << "[" << items.size() << "]:\n";
            for (auto& it : items) { os << pad << "  -\n"; dumpText (it, os, indent + 2); }
        }
    }
    template <class T> void dumpText (T& obj, std::ostream& os)
    {
        MemoryTree::Obj root; write<MemoryTree> (&root, obj); dumpText (root, os, 0);
    }
} // namespace sct::serial::file
