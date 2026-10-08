// Wire formats shared by every handler: the 4-byte message type, the 56-byte
// tank packet header, and Proton's VariantList encoding.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>

enum MessageType : uint32_t {
    MSG_SERVER_HELLO = 1,
    MSG_GENERIC_TEXT = 2,
    MSG_GAME_MESSAGE = 3,
    MSG_GAME_PACKET = 4,
};

// Layout read off the client at 0x43c590/0x43c5b0: 0x38 bytes, extended data
// follows when flags & 8, its length at +0x34.
#pragma pack(push, 1)
struct TankPacket {
    uint8_t type = 0;
    uint8_t pad1 = 0, pad2 = 0, pad3 = 0;
    int32_t netID = 0;
    int32_t item = 0;       // +0x08
    uint32_t flags = 0;     // +0x0c
    float floatVar = 0;     // +0x10
    int32_t intData = 0;    // +0x14 (delay for function calls, -1 = now)
    float posX = 0;         // +0x18
    float posY = 0;         // +0x1c
    float speedX = 0;       // +0x20
    float speedY = 0;       // +0x24
    int32_t unk28 = 0;      // +0x28
    int32_t tileX = 0;      // +0x2c
    int32_t tileY = 0;      // +0x30
    uint32_t extSize = 0;   // +0x34
};
#pragma pack(pop)
static_assert(sizeof(TankPacket) == 0x38, "tank packet header is 0x38 bytes");

constexpr uint32_t TANK_FLAG_EXTENDED = 8;

class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    // u16 length + bytes, the format of Proton's string Serialize (0x456380).
    void str16(const std::string& s) {
        u16(static_cast<uint16_t>(s.size()));
        raw(s.data(), s.size());
    }
    size_t size() const { return buf.size(); }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    size_t left() const { return ok_ ? n_ - pos_ : 0; }
    template <class T> T get() {
        T v{};
        if (pos_ + sizeof(T) > n_) { ok_ = false; return v; }
        memcpy(&v, p_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::string str16() {
        uint16_t len = get<uint16_t>();
        if (!ok_ || pos_ + len > n_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return s;
    }
private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// VariantList as the client reads it (0x4576b0): count byte, then per entry an
// index byte, a type byte and the value.
class VariantList {
public:
    explicit VariantList(const std::string& fn) { str(fn); }
    VariantList& str(const std::string& s) {
        begin(2);
        w_.u32(static_cast<uint32_t>(s.size()));
        w_.raw(s.data(), s.size());
        return *this;
    }
    VariantList& f(float v) { begin(1); w_.f32(v); return *this; }
    VariantList& vec2(float x, float y) { begin(3); w_.f32(x); w_.f32(y); return *this; }
    VariantList& vec3(float x, float y, float z) { begin(4); w_.f32(x); w_.f32(y); w_.f32(z); return *this; }
    VariantList& u(uint32_t v) { begin(5); w_.u32(v); return *this; }
    VariantList& i(int32_t v) { begin(9); w_.i32(v); return *this; }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out;
        out.push_back(count_);
        out.insert(out.end(), w_.buf.begin(), w_.buf.end());
        return out;
    }
private:
    void begin(uint8_t type) { w_.u8(count_++); w_.u8(type); }
    Writer w_;
    uint8_t count_ = 0;
};

// "key|value\n" text as sent in message types 2 and 3.
inline std::map<std::string, std::string> ParseText(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t bar = line.find('|');
        if (bar != std::string::npos) out[line.substr(0, bar)] = line.substr(bar + 1);
        else if (!line.empty()) out[line] = "";
        start = end + 1;
    }
    return out;
}
