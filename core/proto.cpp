#include "proto.h"

#include <cstring>

namespace anki::pb {

static bool read_varint(const std::string& d, size_t& pos, uint64_t& out) {
    out = 0;
    int shift = 0;
    while (pos < d.size() && shift < 64) {
        uint8_t b = (uint8_t)d[pos++];
        out |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}

void put_varint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back((char)((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back((char)v);
}

bool parse(const std::string& d, std::vector<Field>& out) {
    size_t pos = 0;
    while (pos < d.size()) {
        uint64_t key;
        if (!read_varint(d, pos, key)) return false;
        Field f;
        f.number = (uint32_t)(key >> 3);
        f.type = (WireType)(key & 7);
        switch (f.type) {
            case VARINT:
                if (!read_varint(d, pos, f.varint)) return false;
                break;
            case I64:
                if (pos + 8 > d.size()) return false;
                memcpy(&f.varint, d.data() + pos, 8);
                pos += 8;
                break;
            case I32: {
                if (pos + 4 > d.size()) return false;
                uint32_t v;
                memcpy(&v, d.data() + pos, 4);
                f.varint = v;
                pos += 4;
                break;
            }
            case LEN: {
                uint64_t len;
                if (!read_varint(d, pos, len) || pos + len > d.size()) return false;
                f.bytes = d.substr(pos, (size_t)len);
                pos += (size_t)len;
                break;
            }
            default:
                return false;
        }
        out.push_back(std::move(f));
    }
    return true;
}

const Field* Message::get(uint32_t n) const {
    const Field* r = nullptr;
    for (const auto& f : fields)
        if (f.number == n) r = &f;
    return r;
}

uint64_t Message::u64(uint32_t n, uint64_t def) const {
    const Field* f = get(n);
    return f ? f->varint : def;
}

int64_t Message::i64(uint32_t n, int64_t def) const { return (int64_t)u64(n, (uint64_t)def); }

int32_t Message::sint32(uint32_t n, int32_t def) const {
    const Field* f = get(n);
    if (!f) return def;
    uint32_t v = (uint32_t)f->varint;
    return (int32_t)((v >> 1) ^ (~(v & 1) + 1));
}

float Message::f32(uint32_t n, float def) const {
    const Field* f = get(n);
    if (!f || f->type != I32) return def;
    uint32_t bits = (uint32_t)f->varint;
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

std::string Message::str(uint32_t n) const {
    const Field* f = get(n);
    return f ? f->bytes : std::string();
}

Message Message::sub(uint32_t n) const {
    const Field* f = get(n);
    return f ? Message(f->bytes) : Message();
}

std::vector<float> Message::packed_f32(uint32_t n) const {
    std::vector<float> out;
    for (const auto& f : fields) {
        if (f.number != n) continue;
        if (f.type == LEN) {
            for (size_t i = 0; i + 4 <= f.bytes.size(); i += 4) {
                float v;
                memcpy(&v, f.bytes.data() + i, 4);
                out.push_back(v);
            }
        } else if (f.type == I32) {
            uint32_t bits = (uint32_t)f.varint;
            float v;
            memcpy(&v, &bits, 4);
            out.push_back(v);
        }
    }
    return out;
}

std::vector<Message> Message::repeated_sub(uint32_t n) const {
    std::vector<Message> out;
    for (const auto& f : fields)
        if (f.number == n) out.emplace_back(f.bytes);
    return out;
}

static Field* find_or_add(std::vector<Field>& fields, uint32_t n, WireType t) {
    Field* last = nullptr;
    for (auto& f : fields)
        if (f.number == n) last = &f;
    if (last) {
        last->type = t;
        return last;
    }
    // Keep fields ordered by number, as prost emits them.
    size_t pos = fields.size();
    for (size_t i = 0; i < fields.size(); i++)
        if (fields[i].number > n) { pos = i; break; }
    Field f;
    f.number = n;
    f.type = t;
    fields.insert(fields.begin() + pos, f);
    return &fields[pos];
}

void Message::set_varint(uint32_t n, uint64_t v) {
    if (v == 0) { clear(n); return; }  // proto3 omits defaults
    find_or_add(fields, n, VARINT)->varint = v;
}

void Message::set_sint32(uint32_t n, int32_t v) {
    uint32_t z = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
    set_varint(n, z);
}

void Message::set_f32(uint32_t n, float v) {
    if (v == 0.0f) { clear(n); return; }
    uint32_t bits;
    memcpy(&bits, &v, 4);
    find_or_add(fields, n, I32)->varint = bits;
}

void Message::set_bytes(uint32_t n, const std::string& v) {
    if (v.empty()) { clear(n); return; }
    Field* f = find_or_add(fields, n, LEN);
    f->bytes = v;
    f->varint = 0;
}

void Message::set_packed_f32(uint32_t n, const std::vector<float>& v) {
    clear(n);
    if (v.empty()) return;
    std::string b(v.size() * 4, '\0');
    memcpy(&b[0], v.data(), v.size() * 4);
    find_or_add(fields, n, LEN)->bytes = b;
}

void Message::clear(uint32_t n) {
    std::vector<Field> keep;
    for (auto& f : fields)
        if (f.number != n) keep.push_back(std::move(f));
    fields.swap(keep);
}

std::string Message::encode() const {
    std::string out;
    for (const auto& f : fields) {
        put_varint(out, ((uint64_t)f.number << 3) | f.type);
        switch (f.type) {
            case VARINT: put_varint(out, f.varint); break;
            case I64: out.append((const char*)&f.varint, 8); break;
            case I32: { uint32_t v = (uint32_t)f.varint; out.append((const char*)&v, 4); break; }
            case LEN: put_varint(out, f.bytes.size()); out += f.bytes; break;
        }
    }
    return out;
}

}  // namespace anki::pb
