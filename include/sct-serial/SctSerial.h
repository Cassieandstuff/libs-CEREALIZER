#pragma once
// ============================================================================
// sct-serial — reflect()-driven serialization with a pluggable Backend.
//
// ONE reflect() field list per type, walked by a Writer/Reader pair, delegating
// leaf/child/list storage to a Backend. Header-only, pure std, no framework.
//
//   SctSerial.h      — the model (Writer / Reader / write / read), the Backend
//                      concept, MemoryTree (a pure-std reference backend) and
//                      roundTrip() — the serialization drift gate.
//   SctSerialFile.h  — the native file format over MemoryTree: a compact,
//                      validated little-endian binary file + a text dump.
//
// Split out of sct-state (whose snapshot undo core is unrelated to this); the
// model and the "SCT1" file format are unchanged.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sct
{

// ============================================================================
// The model — ONE reflect() field list per type, walked by a Writer/Reader
// pair, delegating leaf/child/list storage to a pluggable Backend. Because save
// and load share that one list (and it lives next to the fields), a field can
// never be written but not read; adding a field is one line. Keys match by NAME,
// so a key missing on load keeps the field's default → old documents keep
// opening when new fields appear. (Generalizes the DAW's serial:: visitors off
// juce::ValueTree onto any store.)
//
// A type is serializable if it has  template<class V> void reflect(V& v);  and
// calls  v("key", field);  per field. Field dispatch:
//   - arithmetic / bool     -> Backend scalar leaf
//   - enum                  -> Backend scalar leaf as its underlying integer
//   - std::string           -> Backend string leaf
//   - std::vector<T>         -> Backend list; T reflectable → child objects, T a
//                               leaf (scalar/string/enum) → items of {v: leaf}
//   - std::map<K, V>         -> Backend list of {k, v} items (K/V leaf or reflectable,
//                               V may itself be a vector or a nested map)
//   - anything else          -> assumed Reflectable -> nested child object
// (Reflectable-vector encoding is unchanged from the DAW's; scalar vectors and maps
// are the added forms.)
//
// ---- Backend concept -------------------------------------------------------
// A Backend binds this model to a concrete store (in-memory tree, JSON, YAML,
// juce::ValueTree, …). It provides two handle types and 11 static functions:
//
//   using Node;   // handle to an object node
//   using List;   // handle to a list node
//
//   // write (mutating)
//   static Node putObject(Node parent, const char* key);   // add child object, return it
//   static List putList  (Node parent, const char* key);   // add child list,   return it
//   static Node putItem  (List list);                      // append an object item, return it
//   template<class N> static void putScalar(Node, const char* key, N value);  // arithmetic/bool
//   static void putString(Node, const char* key, const std::string& value);
//
//   // read (missing key -> the provided default / std::nullopt; never mutates)
//   static std::optional<Node> getObject(Node parent, const char* key);
//   static std::optional<List> getList  (Node parent, const char* key);
//   static std::size_t         count    (List list);
//   static Node                item     (List list, std::size_t i);
//   template<class N> static N getScalar(Node, const char* key, N dflt);
//   static std::string getString(Node, const char* key, const std::string& dflt);
//
// The framework-agnostic MemoryTree backend below implements this and is what
// the round-trip gate uses; app-side adapters (JSON/YAML/ValueTree) implement
// the same concept.
// ============================================================================
namespace serial
{
    template <class> struct is_std_vector : std::false_type {};
    template <class T, class A> struct is_std_vector<std::vector<T, A>> : std::true_type {};
    template <class> struct is_std_map : std::false_type {};
    template <class K, class V, class C, class A> struct is_std_map<std::map<K, V, C, A>> : std::true_type {};

    // A "leaf" serializes to a single Backend scalar/string; anything else is a
    // nested reflectable. Used to wrap scalar/string/enum elements of a vector or
    // map (reflectable elements keep reflecting directly, unchanged).
    template <class T> inline constexpr bool is_leaf_v =
        std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>;

    // Save visitor: value.reflect(Writer{node}) writes into the backend node.
    template <class B>
    struct Writer
    {
        typename B::Node node;

        template <class T>
        void operator() (const char* key, T& value)
        {
            if constexpr (std::is_same_v<T, std::string>)
                B::putString (node, key, value);
            else if constexpr (std::is_enum_v<T>)
                B::putScalar (node, key, static_cast<std::underlying_type_t<T>> (value));
            else if constexpr (std::is_arithmetic_v<T>)
                B::putScalar (node, key, value);
            else if constexpr (is_std_vector<T>::value)
            {
                using E = typename T::value_type;
                typename B::List list = B::putList (node, key);
                for (auto& element : value)
                {
                    Writer<B> w { B::putItem (list) };
                    if constexpr (is_leaf_v<E>) w ("v", element);   // scalar/string/enum element
                    else                        element.reflect (w); // reflectable element (unchanged)
                }
            }
            else if constexpr (is_std_map<T>::value)
            {
                // A map serializes as a list of {k, v} item objects: backend-agnostic
                // (no key enumeration needed) and uniform for any key/value type —
                // scalar/reflectable/vector/nested-map values dispatch recursively via v.
                typename B::List list = B::putList (node, key);
                for (auto& entry : value)
                {
                    Writer<B> w { B::putItem (list) };
                    typename T::key_type k = entry.first;   // map key is const → copy to write
                    w ("k", k);
                    w ("v", entry.second);
                }
            }
            else
            {
                Writer<B> w { B::putObject (node, key) };
                value.reflect (w);                       // assumed reflectable
            }
        }
    };

    // Load visitor: value.reflect(Reader{node}) reads from the backend node,
    // leaving any field whose key is absent at its current (default) value.
    template <class B>
    struct Reader
    {
        typename B::Node node;

        template <class T>
        void operator() (const char* key, T& value)
        {
            if constexpr (std::is_same_v<T, std::string>)
                value = B::getString (node, key, value);
            else if constexpr (std::is_enum_v<T>)
                value = static_cast<T> (B::getScalar (node, key, static_cast<std::underlying_type_t<T>> (value)));
            else if constexpr (std::is_arithmetic_v<T>)
                value = B::getScalar (node, key, value);
            else if constexpr (is_std_vector<T>::value)
            {
                using E = typename T::value_type;
                auto list = B::getList (node, key);
                if (! list) return;                      // missing → keep default (e.g. non-empty)
                value.clear();                           // present → replace
                const std::size_t n = B::count (*list);
                for (std::size_t i = 0; i < n; ++i)
                {
                    E element {};
                    Reader<B> r { B::item (*list, i) };
                    if constexpr (is_leaf_v<E>) r ("v", element);
                    else                        element.reflect (r);
                    value.push_back (std::move (element));
                }
            }
            else if constexpr (is_std_map<T>::value)
            {
                auto list = B::getList (node, key);
                if (! list) return;                      // missing → keep default
                value.clear();                           // present → replace
                const std::size_t n = B::count (*list);
                for (std::size_t i = 0; i < n; ++i)
                {
                    Reader<B> r { B::item (*list, i) };
                    typename T::key_type    k {};
                    typename T::mapped_type v {};
                    r ("k", k);
                    r ("v", v);
                    value.emplace (std::move (k), std::move (v));
                }
            }
            else
            {
                auto child = B::getObject (node, key);
                if (! child) return;                     // missing → keep default
                Reader<B> r { *child };
                value.reflect (r);
            }
        }
    };

    // Serialize `obj` into an existing backend node / read it back out.
    template <class B, class T> void write (typename B::Node node, T& obj) { Writer<B> w { node }; obj.reflect (w); }
    template <class B, class T> void read  (typename B::Node node, T& obj) { Reader<B> r { node }; obj.reflect (r); }

    // ------------------------------------------------------------------------
    // MemoryTree — the pure-std reference Backend. Not a file format: an
    // in-memory object tree used to gate serialization (round-trip a Doc through
    // it and compare) and as a zero-dependency default. Scalars are stored in
    // their exact numeric width so the round-trip is bit-exact.
    // ------------------------------------------------------------------------
    struct MemoryTree
    {
        using Scalar = std::variant<std::int64_t, double, bool, std::string>;

        struct Obj
        {
            std::map<std::string, Scalar>            scalars;
            std::map<std::string, Obj>               objects;
            std::map<std::string, std::vector<Obj>>  lists;
        };

        using Node = Obj*;
        using List = std::vector<Obj>*;

        // write
        static Node putObject (Node p, const char* k) { return &p->objects[k]; }
        static List putList   (Node p, const char* k) { return &p->lists[k]; }
        static Node putItem   (List l) { l->emplace_back(); return &l->back(); }

        template <class N>
        static void putScalar (Node p, const char* k, N v)
        {
            if constexpr (std::is_same_v<N, bool>)          p->scalars[k] = Scalar { v };
            else if constexpr (std::is_floating_point_v<N>) p->scalars[k] = Scalar { static_cast<double> (v) };
            else                                            p->scalars[k] = Scalar { static_cast<std::int64_t> (v) };
        }
        static void putString (Node p, const char* k, const std::string& v) { p->scalars[k] = Scalar { v }; }

        // read (never mutates the tree)
        static std::optional<Node> getObject (Node p, const char* k)
        {
            auto it = p->objects.find (k);
            return it == p->objects.end() ? std::nullopt : std::optional<Node> (&it->second);
        }
        static std::optional<List> getList (Node p, const char* k)
        {
            auto it = p->lists.find (k);
            return it == p->lists.end() ? std::nullopt : std::optional<List> (&it->second);
        }
        static std::size_t count (List l)            { return l->size(); }
        static Node        item  (List l, std::size_t i) { return &(*l)[i]; }

        template <class N>
        static N getScalar (Node p, const char* k, N dflt)
        {
            auto it = p->scalars.find (k);
            if (it == p->scalars.end()) return dflt;
            return std::visit ([&] (auto&& stored) -> N
            {
                using S = std::decay_t<decltype (stored)>;
                if constexpr (std::is_same_v<S, std::string>) return dflt;               // type mismatch → default
                else if constexpr (std::is_same_v<N, bool>)   return static_cast<bool> (stored);
                else                                          return static_cast<N> (stored);
            }, it->second);
        }
        static std::string getString (Node p, const char* k, const std::string& dflt)
        {
            auto it = p->scalars.find (k);
            if (it == p->scalars.end()) return dflt;
            if (auto* s = std::get_if<std::string> (&it->second)) return *s;
            return dflt;                                                                 // type mismatch → default
        }
    };

    // Round-trip a reflectable value through MemoryTree (write then read into a
    // fresh value). The serialization drift gate: roundTrip(x) == x for a value
    // whose every field is set, iff every field is in reflect() both ways.
    template <class T>
    T roundTrip (T& obj)
    {
        MemoryTree::Obj root;
        write<MemoryTree> (&root, obj);
        T out {};
        read<MemoryTree> (&root, out);
        return out;
    }
} // namespace serial

} // namespace sct
