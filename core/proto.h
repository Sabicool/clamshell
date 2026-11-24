// Minimal protobuf wire-format reader/writer. Anki 2.1.50+ stores decks, deck
// presets and note types as protobuf blobs; we decode the fields we need and,
// when writing, patch fields in place so unknown fields survive untouched.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace anki::pb {

enum WireType : uint8_t { VARINT = 0, I64 = 1, LEN = 2, I32 = 5 };

struct Field {
    uint32_t number;
    WireType type;
    uint64_t varint = 0;   // VARINT, I64, I32 raw bits
    std::string bytes;     // LEN
};

// Parses a message into its fields in wire order. Returns false on malformed input.
bool parse(const std::string& data, std::vector<Field>& out);

struct Message {
    std::vector<Field> fields;
    bool ok = true;
    explicit Message(const std::string& data) { ok = parse(data, fields); }
    Message() = default;

    const Field* get(uint32_t n) const;  // last occurrence (proto3 semantics)
    uint64_t u64(uint32_t n, uint64_t def = 0) const;
    int64_t i64(uint32_t n, int64_t def = 0) const;
    int32_t sint32(uint32_t n, int32_t def = 0) const;  // zigzag
    bool boolean(uint32_t n, bool def = false) const { return u64(n, def) != 0; }
    float f32(uint32_t n, float def = 0) const;
    bool has(uint32_t n) const { return get(n) != nullptr; }
    std::string str(uint32_t n) const;
    Message sub(uint32_t n) const;
    std::vector<float> packed_f32(uint32_t n) const;  // repeated float (packed or not)
    std::vector<Message> repeated_sub(uint32_t n) const;

    // Writers: replace (or append) a field, keeping all others.
    void set_varint(uint32_t n, uint64_t v);
    void set_sint32(uint32_t n, int32_t v);
    void set_f32(uint32_t n, float v);
    void set_bytes(uint32_t n, const std::string& v);
    void set_packed_f32(uint32_t n, const std::vector<float>& v);
    void clear(uint32_t n);
    std::string encode() const;
};

void put_varint(std::string& out, uint64_t v);

}  // namespace anki::pb
