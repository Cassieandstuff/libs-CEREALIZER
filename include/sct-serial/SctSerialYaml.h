#pragma once
// ============================================================================
// sct-serial YAML — the reflect() model as YAML text, for files a person reads
// and edits by hand. Header-only, pure std (no YAML library).
//
//   sct::serial::yaml::save (obj)          -> std::string
//   sct::serial::yaml::load (text, obj)    -> std::expected<void, Error>
//   sct::serial::yaml::saveFile / loadFile    (atomic write; read whole file)
//
// What it writes, from the same reflect() list the other backends use:
//   - an object is a block mapping, its keys in reflect() order;
//   - a std::map is a mapping from its keys to its values (not the {k, v} item
//     list the model uses), when every key is a leaf and the keys are distinct;
//   - a std::vector of leaves is a sequence of scalars; of objects, a sequence
//     of mappings;
//   - a string is plain when YAML would read it back as that same string, and
//     double-quoted otherwise ("0001", "true", "a: b", "", leading spaces …);
//   - a float is the shortest text that reads back to the same value, so a
//     save → load → save is byte-identical.
//
// What it reads: the block YAML a person writes — block mappings and sequences
// (a sequence may sit at its key's indent), plain / 'single' / "double" quoted
// scalars, flow [a, b] and {k: v} collections, # comments, and a leading "---".
// It refuses, with the line, what it doesn't model: anchors, aliases, tags,
// block scalars (| >), multi-line quoted or plain scalars, tabs in
// indentation, duplicate keys. A value that doesn't fit its field (a word where
// a number goes, a list where an object goes) is an error too, not a silent
// default: these files are hand-edited. A key the file lacks still keeps the
// field's default, as in every backend.
// ============================================================================

#include <sct-serial/SctSerial.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace sct::serial::yaml
{
    // ── the document tree ─────────────────────────────────────────────────────
    struct Field;

    struct Value
    {
        enum class Kind { Null, Scalar, Map, Seq };

        Kind               kind   = Kind::Null;
        std::string        text;              ///< a scalar's text, unquoted and unescaped
        bool               string = false;    ///< written: a string (quoted if YAML would misread it); read: it was quoted
        std::size_t        line   = 0;        ///< 1-based source line; 0 = made in memory
        std::vector<Field> map;               ///< Kind::Map, in order
        std::vector<Value> seq;               ///< Kind::Seq

        [[nodiscard]] const Value* find (std::string_view key) const;
        [[nodiscard]] Value*       find (std::string_view key);
    };

    struct Field
    {
        std::string key;
        Value       value;
    };

    inline const Value* Value::find (std::string_view key) const
    {
        for (const Field& f : map)
            if (f.key == key) return &f.value;
        return nullptr;
    }
    inline Value* Value::find (std::string_view key)
    {
        for (Field& f : map)
            if (f.key == key) return &f.value;
        return nullptr;
    }

    struct Error
    {
        std::size_t line = 0;   ///< 1-based; 0 = not tied to a line
        std::string message;

        [[nodiscard]] std::string what() const
        {
            return line ? "line " + std::to_string (line) + ": " + message : message;
        }
    };

    namespace detail
    {
        // ── scalars ───────────────────────────────────────────────────────────
        inline bool isNullWord (std::string_view s) { return s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL"; }

        /// The words a YAML 1.1 or 1.2 reader takes for a boolean, quoted so they read back as text. The single letters YAML
        /// 1.1 also lists (y, n) are left plain: the readers in use (PyYAML, ruamel, rapidyaml, yaml-cpp) read them as text,
        /// and quoting them would quote every `y:` key of a vector.
        inline bool isBoolWord (std::string_view s)
        {
            for (std::string_view w : { "true", "True", "TRUE", "false", "False", "FALSE", "yes", "Yes", "YES", "no", "No", "NO",
                                        "on", "On", "ON", "off", "Off", "OFF" })
                if (s == w) return true;
            return false;
        }

        /// Whether a plain scalar `s` would read back as something other than the string `s`.
        inline bool looksLikeNumber (std::string_view s)
        {
            if (s.empty()) return false;
            if (s == ".inf" || s == "-.inf" || s == "+.inf" || s == ".nan" || s == ".Inf" || s == "-.Inf" || s == ".NaN" || s == ".NAN" ||
                s == ".INF" || s == "-.INF")
                return true;
            // Conservative: anything a YAML 1.1 or 1.2 reader might take for a number (1, -2.5, 1e3, 0x1F, 1_000,
            // 1:30) is quoted. Text with other letters in it ("013BA3:Skyrim.esm") is not a number.
            std::size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
            if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'o' || s[i + 1] == 'b')) return true;
            const std::size_t start = i;
            if (i < s.size() && s[i] == '.') ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            for (i = start; i < s.size(); ++i)
            {
                const char c = s[i];
                if (! ((c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == 'e' || c == 'E' || c == '+' || c == '-'))
                    return false;
            }
            return true;
        }

        /// Whether `s` can be written plain (unquoted) and read back as exactly `s`.
        inline bool plainSafe (std::string_view s)
        {
            if (s.empty() || isNullWord (s) || isBoolWord (s) || looksLikeNumber (s)) return false;
            if (s.front() == ' ' || s.back() == ' ' || s.back() == ':') return false;
            switch (s.front())
            {
                case '-': case '?': case ':': case ',': case '[': case ']': case '{': case '}': case '#': case '&':
                case '*': case '!': case '|': case '>': case '\'': case '"': case '%': case '@': case '`':
                    return false;
                default: break;
            }
            if (s == "---" || s == "...") return false;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char> (s[i]);
                if (c < 0x20 || c == 0x7f) return false;
                if (c == ':' && i + 1 < s.size() && s[i + 1] == ' ') return false;
                if (c == '#' && i > 0 && s[i - 1] == ' ') return false;
                // In a flow collection these end the scalar; quote them so a value reads the same anywhere.
                if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}') return false;
            }
            return true;
        }

        inline std::string doubleQuoted (std::string_view s)
        {
            std::string out = "\"";
            for (const char ch : s)
            {
                const unsigned char c = static_cast<unsigned char> (ch);
                switch (c)
                {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    case '\r': out += "\\r"; break;
                    default:
                        if (c < 0x20 || c == 0x7f)
                        {
                            static constexpr char hex[] = "0123456789ABCDEF";
                            out += "\\x";
                            out += hex[c >> 4];
                            out += hex[c & 15];
                        }
                        else
                            out += ch;
                }
            }
            out += '"';
            return out;
        }

        inline std::string scalarText (const Value& v)
        {
            if (v.kind == Value::Kind::Null) return "null";
            if (! v.string) return v.text;
            return plainSafe (v.text) ? v.text : doubleQuoted (v.text);
        }

        inline std::string keyText (const std::string& k) { return plainSafe (k) ? k : doubleQuoted (k); }

        template <class N> std::string numberText (N value)
        {
            if constexpr (std::is_same_v<N, bool>)
                return value ? "true" : "false";
            else if constexpr (std::is_floating_point_v<N>)
            {
                if (std::isnan (value)) return ".nan";
                if (std::isinf (value)) return value > 0 ? ".inf" : "-.inf";
                char buf[64];
                const auto r = std::to_chars (buf, buf + sizeof buf, value);   // shortest round-trip form
                return std::string (buf, r.ptr);
            }
            else if constexpr (std::is_signed_v<N>)
            {
                char buf[32];
                const auto r = std::to_chars (buf, buf + sizeof buf, static_cast<long long> (value));
                return std::string (buf, r.ptr);
            }
            else
            {
                char buf[32];
                const auto r = std::to_chars (buf, buf + sizeof buf, static_cast<unsigned long long> (value));
                return std::string (buf, r.ptr);
            }
        }

        /// Parse `s` as an N. False when it isn't one (or doesn't fit).
        template <class N> bool parseNumber (std::string_view s, N& out)
        {
            if constexpr (std::is_same_v<N, bool>)
            {
                if (s == "true" || s == "True" || s == "TRUE") { out = true; return true; }
                if (s == "false" || s == "False" || s == "FALSE") { out = false; return true; }
                return false;
            }
            else if constexpr (std::is_floating_point_v<N>)
            {
                if (s == ".nan" || s == ".NaN" || s == ".NAN") { out = std::numeric_limits<N>::quiet_NaN(); return true; }
                if (s == ".inf" || s == "+.inf" || s == ".Inf" || s == ".INF") { out = std::numeric_limits<N>::infinity(); return true; }
                if (s == "-.inf" || s == "-.Inf" || s == "-.INF") { out = -std::numeric_limits<N>::infinity(); return true; }
                if (! s.empty() && s.front() == '+') s.remove_prefix (1);
                if (s.empty()) return false;
                const auto r = std::from_chars (s.data(), s.data() + s.size(), out);
                return r.ec == std::errc{} && r.ptr == s.data() + s.size();
            }
            else
            {
                if (! s.empty() && s.front() == '+') s.remove_prefix (1);
                if (s.empty()) return false;
                using Wide = std::conditional_t<std::is_signed_v<N>, long long, unsigned long long>;
                Wide w {};
                const auto r = std::from_chars (s.data(), s.data() + s.size(), w);
                if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
                if constexpr (std::is_signed_v<N>)
                {
                    if (w < static_cast<Wide> (std::numeric_limits<N>::min()) || w > static_cast<Wide> (std::numeric_limits<N>::max()))
                        return false;
                }
                else if (w > static_cast<Wide> (std::numeric_limits<N>::max()))
                    return false;
                out = static_cast<N> (w);
                return true;
            }
        }

        // ── emitting ──────────────────────────────────────────────────────────
        inline bool isLeaf (const Value& v) { return v.kind == Value::Kind::Scalar || v.kind == Value::Kind::Null; }

        /// The model writes a std::map as a list of {k, v} items and a vector of leaves as a list of {v} items. Fold
        /// those back into the YAML a person would write: a mapping, and a sequence of scalars. Load unfolds them.
        inline Value fold (const Value& in)
        {
            Value out;
            out.kind   = in.kind;
            out.text   = in.text;
            out.string = in.string;
            if (in.kind == Value::Kind::Map)
            {
                out.map.reserve (in.map.size());
                for (const Field& f : in.map) out.map.push_back ({ f.key, fold (f.value) });
                return out;
            }
            if (in.kind != Value::Kind::Seq) return out;

            const auto isEntry = [] (const Value& item)
            {
                return item.kind == Value::Kind::Map && item.map.size() == 2 && item.map[0].key == "k" && item.map[1].key == "v" &&
                       item.map[0].value.kind == Value::Kind::Scalar;
            };
            const auto isWrapped = [] (const Value& item)
            {
                return item.kind == Value::Kind::Map && item.map.size() == 1 && item.map[0].key == "v" && isLeaf (item.map[0].value);
            };
            bool entries = ! in.seq.empty(), wrapped = ! in.seq.empty();
            for (const Value& item : in.seq)
            {
                entries = entries && isEntry (item);
                wrapped = wrapped && isWrapped (item);
            }
            if (entries)
            {
                for (std::size_t i = 0; i < in.seq.size() && entries; ++i)
                    for (std::size_t j = 0; j < i; ++j)
                        if (in.seq[i].map[0].value.text == in.seq[j].map[0].value.text) { entries = false; break; }
            }
            if (entries)
            {
                out.kind = Value::Kind::Map;
                for (const Value& item : in.seq) out.map.push_back ({ item.map[0].value.text, fold (item.map[1].value) });
                return out;
            }
            for (const Value& item : in.seq) out.seq.push_back (wrapped ? item.map[0].value : fold (item));
            return out;
        }

        inline void emitNode (const Value& v, std::size_t indent, std::string& out);

        /// The text after "key:" or "-": a scalar or an empty collection on the same line, else a newline and the block.
        inline void emitAfter (const Value& v, std::size_t indent, std::string& out)
        {
            if (isLeaf (v)) { out += ' '; out += scalarText (v); out += '\n'; return; }
            if (v.kind == Value::Kind::Map && v.map.empty()) { out += " {}\n"; return; }
            if (v.kind == Value::Kind::Seq && v.seq.empty()) { out += " []\n"; return; }
            out += '\n';
            emitNode (v, indent, out);
        }

        /// A mapping's fields at `indent`; the first one's line starts with `firstPrefix` instead of the indent.
        inline void emitFields (const Value& v, std::size_t indent, std::string& out, const std::string* firstPrefix = nullptr)
        {
            bool first = true;
            for (const Field& f : v.map)
            {
                out += (first && firstPrefix) ? *firstPrefix : std::string (indent, ' ');
                first = false;
                out += keyText (f.key);
                out += ':';
                emitAfter (f.value, indent + 2, out);
            }
        }

        inline void emitNode (const Value& v, std::size_t indent, std::string& out)
        {
            if (v.kind == Value::Kind::Map) { emitFields (v, indent, out); return; }
            for (const Value& item : v.seq)
            {
                const std::string dash = std::string (indent, ' ') + "- ";
                if (item.kind == Value::Kind::Map && ! item.map.empty())
                    emitFields (item, indent + 2, out, &dash);
                else if (item.kind == Value::Kind::Seq && ! item.seq.empty())
                {
                    out += std::string (indent, ' ') + "-\n";
                    emitNode (item, indent + 2, out);
                }
                else
                {
                    out += std::string (indent, ' ') + "-";
                    emitAfter (item, indent + 2, out);
                }
            }
        }

        // ── parsing ───────────────────────────────────────────────────────────
        struct Line
        {
            std::size_t      number = 0;
            std::size_t      indent = 0;
            std::string      text;   ///< without the indent, the comment or trailing spaces
        };

        /// Cut a trailing comment (a '#' after a space, outside quotes) and trailing spaces.
        inline std::string stripComment (std::string_view s)
        {
            char quote = 0;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (quote)
                {
                    if (quote == '"' && c == '\\') { ++i; continue; }
                    if (c == quote)
                    {
                        if (quote == '\'' && i + 1 < s.size() && s[i + 1] == '\'') { ++i; continue; }
                        quote = 0;
                    }
                    continue;
                }
                if ((c == '"' || c == '\'') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '[' || s[i - 1] == '{' || s[i - 1] == ',' ||
                                                s[i - 1] == ':' || s[i - 1] == '-'))
                    quote = c;
                else if (c == '#' && (i == 0 || s[i - 1] == ' '))
                {
                    s = s.substr (0, i);
                    break;
                }
            }
            while (! s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix (1);
            return std::string (s);
        }

        class Parser
        {
        public:
            explicit Parser (std::string_view text) { split (text); }

            std::expected<Value, Error> run()
            {
                if (m_error) return std::unexpected (*m_error);
                if (m_lines.empty())
                {
                    Value root;
                    root.kind = Value::Kind::Map;
                    return root;
                }
                const Line& first = m_lines.front();
                if (m_lines.size() == 1 && (first.text.starts_with ("{") || first.text.starts_with ("[")))
                {
                    Value whole = inlineValue (first.text, first.number);   // a one-line flow document: "{}"
                    if (m_error) return std::unexpected (*m_error);
                    return whole;
                }
                Value root = block (first.indent);
                if (! m_error && m_at < m_lines.size())
                    fail (m_lines[m_at].number, "this line's indentation doesn't match the lines above it");
                if (m_error) return std::unexpected (*m_error);
                return root;
            }

        private:
            void split (std::string_view text)
            {
                if (text.size() >= 3 && static_cast<unsigned char> (text[0]) == 0xEF && static_cast<unsigned char> (text[1]) == 0xBB &&
                    static_cast<unsigned char> (text[2]) == 0xBF)
                    text.remove_prefix (3);   // UTF-8 BOM
                std::size_t number = 0;
                while (! text.empty())
                {
                    const std::size_t nl = text.find ('\n');
                    std::string_view raw = text.substr (0, nl);
                    text                 = nl == std::string_view::npos ? std::string_view {} : text.substr (nl + 1);
                    ++number;
                    std::size_t indent = 0;
                    while (indent < raw.size() && raw[indent] == ' ') ++indent;
                    if (indent < raw.size() && raw[indent] == '\t')
                    {
                        const std::string rest = stripComment (raw.substr (indent));
                        if (! rest.empty()) { fail (number, "a tab in the indentation (YAML indents with spaces)"); return; }
                    }
                    std::string body = stripComment (raw.substr (indent));
                    if (body.empty()) continue;
                    if (body == "---" || body == "...")
                    {
                        if (! m_lines.empty() && body == "---") { fail (number, "a second document; one file holds one"); return; }
                        continue;
                    }
                    if (body.starts_with ("%")) { fail (number, "YAML directives aren't supported"); return; }
                    m_lines.push_back ({ number, indent, std::move (body) });
                }
            }

            void fail (std::size_t line, std::string message)
            {
                if (! m_error) m_error = Error { line, std::move (message) };
            }

            static bool isDash (const std::string& t) { return t == "-" || t.starts_with ("- "); }

            /// The block starting at m_at, all at `indent`.
            Value block (std::size_t indent)
            {
                if (isDash (m_lines[m_at].text)) return sequence (indent);
                return mapping (indent);
            }

            Value sequence (std::size_t indent)
            {
                Value seq;
                seq.kind = Value::Kind::Seq;
                seq.line = m_lines[m_at].number;
                while (! m_error && m_at < m_lines.size() && m_lines[m_at].indent == indent && isDash (m_lines[m_at].text))
                {
                    Line&       line = m_lines[m_at];
                    std::size_t off  = 1;
                    while (off < line.text.size() && line.text[off] == ' ') ++off;
                    if (off >= line.text.size())
                    {
                        // "-" alone: the item is the block below, or null.
                        ++m_at;
                        if (m_at < m_lines.size() && m_lines[m_at].indent > indent)
                            seq.seq.push_back (block (m_lines[m_at].indent));
                        else
                        {
                            Value nil;
                            nil.line = line.number;
                            seq.seq.push_back (nil);
                        }
                        continue;
                    }
                    const std::string rest = line.text.substr (off);
                    if (isDash (rest) || keyEnd (rest) != std::string::npos)
                    {
                        // "- key: v" or "- - x": the item is a block starting at the column after the dash.
                        line.indent += off;
                        line.text = rest;
                        seq.seq.push_back (block (line.indent));
                        continue;
                    }
                    ++m_at;
                    seq.seq.push_back (inlineValue (rest, line.number));
                }
                return seq;
            }

            /// Where a "key:" ends in `t` (the index of the ':'), or npos when `t` isn't a mapping entry.
            static std::size_t keyEnd (const std::string& t)
            {
                if (t.empty()) return std::string::npos;
                if (t[0] == '"' || t[0] == '\'')
                {
                    const char  q = t[0];
                    std::size_t i = 1;
                    for (; i < t.size(); ++i)
                    {
                        if (q == '"' && t[i] == '\\') { ++i; continue; }
                        if (t[i] == q)
                        {
                            if (q == '\'' && i + 1 < t.size() && t[i + 1] == '\'') { ++i; continue; }
                            break;
                        }
                    }
                    if (i + 1 < t.size() && t[i + 1] == ':' && (i + 2 == t.size() || t[i + 2] == ' ')) return i + 1;
                    return std::string::npos;
                }
                if (t[0] == '[' || t[0] == '{') return std::string::npos;
                for (std::size_t i = 0; i < t.size(); ++i)
                    if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ')) return i;
                return std::string::npos;
            }

            Value mapping (std::size_t indent)
            {
                Value map;
                map.kind = Value::Kind::Map;
                map.line = m_lines[m_at].number;
                while (! m_error && m_at < m_lines.size() && m_lines[m_at].indent == indent && ! isDash (m_lines[m_at].text))
                {
                    const Line        line = m_lines[m_at];
                    const std::size_t end  = keyEnd (line.text);
                    if (end == std::string::npos)
                    {
                        fail (line.number, "expected 'key: value', got '" + line.text + "'");
                        break;
                    }
                    std::string key;
                    {
                        const std::string raw = line.text.substr (0, end);
                        if (raw.starts_with ("\"") || raw.starts_with ("'"))
                        {
                            std::optional<std::string> k = quoted (raw, line.number);
                            if (! k) break;
                            key = std::move (*k);
                        }
                        else
                        {
                            key = raw;
                            while (! key.empty() && key.back() == ' ') key.pop_back();
                            if (key.starts_with ("?") || key.starts_with ("&") || key.starts_with ("*") || key.starts_with ("!"))
                            {
                                fail (line.number, "complex keys, anchors, aliases and tags aren't supported");
                                break;
                            }
                        }
                    }
                    if (map.find (key))
                    {
                        fail (line.number, "the key '" + key + "' appears twice");
                        break;
                    }
                    std::size_t vs = end + 1;
                    while (vs < line.text.size() && line.text[vs] == ' ') ++vs;
                    const std::string rest = line.text.substr (vs);
                    ++m_at;
                    Value value;
                    value.line = line.number;
                    if (! rest.empty())
                        value = inlineValue (rest, line.number);
                    else if (m_at < m_lines.size() && m_lines[m_at].indent > indent)
                        value = block (m_lines[m_at].indent);
                    else if (m_at < m_lines.size() && m_lines[m_at].indent == indent && isDash (m_lines[m_at].text))
                        value = sequence (indent);   // a sequence may sit at its key's indent
                    map.map.push_back ({ std::move (key), std::move (value) });
                }
                return map;
            }

            /// A value written on one line: a quoted or plain scalar, or a flow collection.
            Value inlineValue (const std::string& s, std::size_t line)
            {
                Value v;
                v.line = line;
                if (s.empty()) return v;
                switch (s[0])
                {
                    case '|': case '>':
                        fail (line, "block scalars (| and >) aren't supported; write the text in double quotes with \\n");
                        return v;
                    case '&': case '*': case '!':
                        fail (line, "anchors, aliases and tags aren't supported");
                        return v;
                    case '[': case '{':
                    {
                        std::size_t at = 0;
                        v              = flow (s, at, line);
                        while (at < s.size() && s[at] == ' ') ++at;
                        if (! m_error && at != s.size()) fail (line, "unexpected text after the closing bracket");
                        return v;
                    }
                    case '"': case '\'':
                    {
                        std::optional<std::string> text = quoted (s, line);
                        if (! text) return v;
                        v.kind   = Value::Kind::Scalar;
                        v.text   = std::move (*text);
                        v.string = true;
                        return v;
                    }
                    default: break;
                }
                if (! isNullWord (s))
                {
                    v.kind = Value::Kind::Scalar;
                    v.text = s;
                }
                return v;
            }

            /// A whole quoted scalar `s` ("..." or '...'), unescaped; nullopt (and an error) when malformed.
            std::optional<std::string> quoted (const std::string& s, std::size_t line)
            {
                std::size_t at = 0;
                std::optional<std::string> out = quotedAt (s, at, line);
                if (out && at != s.size()) { fail (line, "unexpected text after the closing quote"); return std::nullopt; }
                return out;
            }

            std::optional<std::string> quotedAt (const std::string& s, std::size_t& at, std::size_t line)
            {
                const char  q = s[at++];
                std::string out;
                while (at < s.size())
                {
                    const char c = s[at++];
                    if (c == q)
                    {
                        if (q == '\'' && at < s.size() && s[at] == '\'') { out += '\''; ++at; continue; }
                        return out;
                    }
                    if (q == '"' && c == '\\')
                    {
                        if (at >= s.size()) break;
                        const char e = s[at++];
                        switch (e)
                        {
                            case '"': out += '"'; break;
                            case '\\': out += '\\'; break;
                            case '/': out += '/'; break;
                            case 'n': out += '\n'; break;
                            case 't': out += '\t'; break;
                            case 'r': out += '\r'; break;
                            case '0': out += '\0'; break;
                            case ' ': out += ' '; break;
                            case 'x': case 'u': case 'U':
                            {
                                const std::size_t digits = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                                if (at + digits > s.size()) { fail (line, "a short \\" + std::string (1, e) + " escape"); return std::nullopt; }
                                std::uint32_t cp = 0;
                                const auto    r  = std::from_chars (s.data() + at, s.data() + at + digits, cp, 16);
                                if (r.ec != std::errc{} || r.ptr != s.data() + at + digits)
                                {
                                    fail (line, "a malformed \\" + std::string (1, e) + " escape");
                                    return std::nullopt;
                                }
                                at += digits;
                                appendUtf8 (out, cp, e == 'x');
                                break;
                            }
                            default:
                                fail (line, std::string ("an unknown escape \\") + e);
                                return std::nullopt;
                        }
                        continue;
                    }
                    out += c;
                }
                fail (line, "a quoted scalar that doesn't close on its line (multi-line quoted text isn't supported)");
                return std::nullopt;
            }

            static void appendUtf8 (std::string& out, std::uint32_t cp, bool rawByte)
            {
                if (rawByte || cp < 0x80) { out += static_cast<char> (cp); return; }
                if (cp < 0x800)
                {
                    out += static_cast<char> (0xC0 | (cp >> 6));
                    out += static_cast<char> (0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    out += static_cast<char> (0xE0 | (cp >> 12));
                    out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char> (0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char> (0xF0 | (cp >> 18));
                    out += static_cast<char> (0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char> (0x80 | (cp & 0x3F));
                }
            }

            static void skipSpaces (const std::string& s, std::size_t& at)
            {
                while (at < s.size() && s[at] == ' ') ++at;
            }

            /// A flow scalar at `at`: quoted, or plain up to , ] } (and, for a key, ": ").
            Value flowScalar (const std::string& s, std::size_t& at, std::size_t line, bool key)
            {
                Value v;
                v.line = line;
                if (s[at] == '"' || s[at] == '\'')
                {
                    std::optional<std::string> text = quotedAt (s, at, line);
                    if (! text) return v;
                    v.kind   = Value::Kind::Scalar;
                    v.text   = std::move (*text);
                    v.string = true;
                    return v;
                }
                const std::size_t start = at;
                while (at < s.size() && s[at] != ',' && s[at] != ']' && s[at] != '}' &&
                       ! (key && s[at] == ':' && (at + 1 == s.size() || s[at + 1] == ' ' || s[at + 1] == ',' || s[at + 1] == '}')))
                    ++at;
                std::string text = s.substr (start, at - start);
                while (! text.empty() && text.back() == ' ') text.pop_back();
                if (! isNullWord (text))
                {
                    v.kind = Value::Kind::Scalar;
                    v.text = std::move (text);
                }
                return v;
            }

            Value flowValue (const std::string& s, std::size_t& at, std::size_t line)
            {
                skipSpaces (s, at);
                if (at >= s.size()) { fail (line, "a flow collection that doesn't close on its line"); return {}; }
                if (s[at] == '[' || s[at] == '{') return flow (s, at, line);
                if (s[at] == '&' || s[at] == '*' || s[at] == '!') { fail (line, "anchors, aliases and tags aren't supported"); return {}; }
                return flowScalar (s, at, line, false);
            }

            /// A flow collection starting at s[at] ('[' or '{'); `at` ends after its closing bracket.
            Value flow (const std::string& s, std::size_t& at, std::size_t line)
            {
                Value      v;
                const char open  = s[at++];
                const char close = open == '[' ? ']' : '}';
                v.kind           = open == '[' ? Value::Kind::Seq : Value::Kind::Map;
                v.line           = line;
                skipSpaces (s, at);
                if (at < s.size() && s[at] == close) { ++at; return v; }
                while (! m_error)
                {
                    skipSpaces (s, at);
                    if (at >= s.size()) break;
                    if (open == '[')
                        v.seq.push_back (flowValue (s, at, line));
                    else
                    {
                        Value k = flowScalar (s, at, line, true);
                        if (m_error) return v;
                        if (at >= s.size() || s[at] != ':') { fail (line, "expected ':' after a key in {...}"); return v; }
                        ++at;
                        if (v.find (k.text)) { fail (line, "the key '" + k.text + "' appears twice"); return v; }
                        v.map.push_back ({ k.text, flowValue (s, at, line) });
                    }
                    if (m_error) return v;
                    skipSpaces (s, at);
                    if (at < s.size() && s[at] == ',') { ++at; continue; }
                    if (at < s.size() && s[at] == close) { ++at; return v; }
                    break;
                }
                fail (line, std::string ("expected ',' or '") + close + "'");
                return v;
            }

            std::vector<Line>    m_lines;
            std::size_t          m_at = 0;
            std::optional<Error> m_error;
        };
    } // namespace detail

    /// @brief Parse YAML text into a tree. The document's top level must be a mapping (or empty).
    inline std::expected<Value, Error> parse (std::string_view text)
    {
        auto root = detail::Parser (text).run();
        if (root && root->kind != Value::Kind::Map)
            return std::unexpected (Error { root->line, "the document's top level must be a mapping (key: value lines)" });
        return root;
    }

    /// @brief A tree as YAML text: block style, two-space indent, a final newline. `root` is a mapping.
    inline std::string emit (const Value& root)
    {
        const Value folded = detail::fold (root);
        std::string out;
        if (folded.kind == Value::Kind::Map && folded.map.empty()) return "{}\n";
        detail::emitNode (folded, 0, out);
        return out;
    }

    // ── the Backend ───────────────────────────────────────────────────────────
    /// The sct::serial Backend over a yaml::Value tree. Its handles carry the run's first error, since a backend
    /// function can't return one.
    struct Tree
    {
        struct Handle
        {
            Value*                value = nullptr;
            std::optional<Error>* error = nullptr;
        };
        using Node = Handle;
        using List = Handle;

        // write
        static Node putObject (Node p, const char* k)
        {
            p.value->map.push_back ({ k, Value { Value::Kind::Map } });
            return { &p.value->map.back().value, p.error };
        }
        static List putList (Node p, const char* k)
        {
            p.value->map.push_back ({ k, Value { Value::Kind::Seq } });
            return { &p.value->map.back().value, p.error };
        }
        static Node putItem (List l)
        {
            l.value->seq.push_back (Value { Value::Kind::Map });
            return { &l.value->seq.back(), l.error };
        }
        template <class N> static void putScalar (Node p, const char* k, N v)
        {
            Value s { Value::Kind::Scalar };
            s.text = detail::numberText (v);
            p.value->map.push_back ({ k, std::move (s) });
        }
        static void putString (Node p, const char* k, const std::string& v)
        {
            Value s { Value::Kind::Scalar };
            s.text   = v;
            s.string = true;
            p.value->map.push_back ({ k, std::move (s) });
        }

        // read
        static std::optional<Node> getObject (Node p, const char* k)
        {
            Value* v = p.value->find (k);
            if (! v || v->kind == Value::Kind::Null) return std::nullopt;
            if (v->kind != Value::Kind::Map)
            {
                fail (p, *v, std::string ("'") + k + "' should be a mapping (key: value lines)");
                return std::nullopt;
            }
            return Node { v, p.error };
        }
        static std::optional<List> getList (Node p, const char* k)
        {
            Value* v = p.value->find (k);
            if (! v || v->kind == Value::Kind::Null) return std::nullopt;
            if (v->kind == Value::Kind::Scalar)
            {
                fail (p, *v, std::string ("'") + k + "' should be a list or a mapping, not '" + v->text + "'");
                return std::nullopt;
            }
            unfold (*v);
            return List { v, p.error };
        }
        static std::size_t count (List l) { return l.value->seq.size(); }
        static Node        item (List l, std::size_t i) { return { &l.value->seq[i], l.error }; }

        template <class N> static N getScalar (Node p, const char* k, N dflt)
        {
            const Value* v = p.value->find (k);
            if (! v || v->kind == Value::Kind::Null) return dflt;
            if (v->kind != Value::Kind::Scalar)
            {
                fail (p, *v, std::string ("'") + k + "' should be a single value");
                return dflt;
            }
            N out {};
            if (detail::parseNumber (v->text, out)) return out;
            fail (p, *v, std::string ("'") + k + "' is '" + v->text + "', which isn't " +
                             (std::is_same_v<N, bool> ? "true or false" : std::is_floating_point_v<N> ? "a number" : "a whole number in range"));
            return dflt;
        }
        static std::string getString (Node p, const char* k, const std::string& dflt)
        {
            const Value* v = p.value->find (k);
            if (! v) return dflt;
            if (v->kind == Value::Kind::Null) return {};
            if (v->kind != Value::Kind::Scalar)
            {
                fail (p, *v, std::string ("'") + k + "' should be a single value");
                return dflt;
            }
            return v->text;
        }

    private:
        static void fail (Handle h, const Value& at, std::string message)
        {
            if (h.error && ! *h.error) *h.error = Error { at.line, std::move (message) };
        }

        /// Read a list the way the model wrote it: a mapping becomes {k, v} items, a scalar item becomes a {v} item.
        static void unfold (Value& v)
        {
            if (v.kind == Value::Kind::Map)
            {
                std::vector<Field> fields = std::move (v.map);
                v.map.clear();
                v.kind = Value::Kind::Seq;
                v.seq.clear();
                v.seq.reserve (fields.size());
                for (Field& f : fields)
                {
                    Value key { Value::Kind::Scalar };
                    key.text = f.key;
                    key.line = f.value.line;
                    Value entry { Value::Kind::Map };
                    entry.line = f.value.line;
                    entry.map.push_back ({ "k", std::move (key) });
                    entry.map.push_back ({ "v", std::move (f.value) });
                    v.seq.push_back (std::move (entry));
                }
                return;
            }
            for (Value& item : v.seq)
            {
                if (item.kind == Value::Kind::Map) continue;
                Value wrapped { Value::Kind::Map };
                wrapped.line = item.line;
                wrapped.map.push_back ({ "v", std::move (item) });
                item = std::move (wrapped);
            }
        }
    };

    // ── reflectable ↔ text / file ─────────────────────────────────────────────
    /// @brief `obj` as YAML text.
    template <class T> std::string save (T& obj)
    {
        Value                root { Value::Kind::Map };
        std::optional<Error> unused;
        serial::write<Tree> (Tree::Handle { &root, &unused }, obj);
        return emit (root);
    }

    /// @brief Read `text` into `obj`. Keys the text lacks keep `obj`'s values; the first malformed line or misfit
    ///        value is the error (and `obj` may then be partly read: load into a fresh value).
    template <class T> std::expected<void, Error> load (std::string_view text, T& obj)
    {
        auto root = parse (text);
        if (! root) return std::unexpected (root.error());
        std::optional<Error> error;
        serial::read<Tree> (Tree::Handle { &*root, &error }, obj);
        if (error) return std::unexpected (*error);
        return {};
    }

    /// @brief Write `obj` to `path` as YAML: a temp file beside it, then renamed over it, so a failed write leaves the
    ///        old file.
    template <class T> std::expected<void, Error> saveFile (const std::filesystem::path& path, T& obj)
    {
        const std::string     text = save (obj);
        std::filesystem::path tmp  = path;
        tmp += ".tmp";
        {
            std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
            if (! out) return std::unexpected (Error { 0, "can't write " + tmp.string() });
            out.write (text.data(), static_cast<std::streamsize> (text.size()));
            out.flush();
            if (! out) return std::unexpected (Error { 0, "can't write " + tmp.string() });
        }
        std::error_code ec;
        std::filesystem::rename (tmp, path, ec);
        if (ec)
        {
            std::filesystem::remove (tmp, ec);
            return std::unexpected (Error { 0, "can't replace " + path.string() });
        }
        return {};
    }

    /// @brief Read the YAML file at `path` into `obj`.
    template <class T> std::expected<void, Error> loadFile (const std::filesystem::path& path, T& obj)
    {
        std::ifstream in (path, std::ios::binary);
        if (! in) return std::unexpected (Error { 0, "can't read " + path.string() });
        std::ostringstream text;
        text << in.rdbuf();
        auto loaded = load (text.str(), obj);
        if (! loaded) return std::unexpected (Error { 0, path.string() + ": " + loaded.error().what() });
        return {};
    }
} // namespace sct::serial::yaml
