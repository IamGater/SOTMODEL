#pragma once
#include "core/Common.h"

// Bounds-checked little-endian reader over a memory block (the UE FArchive equivalent).
class Reader {
public:
    Reader() = default;
    Reader(const u8* data, size_t size, size_t pos = 0) : m_data(data), m_size(size), m_pos(pos) {}
    explicit Reader(const Bytes& b, size_t pos = 0) : m_data(b.data()), m_size(b.size()), m_pos(pos) {}

    size_t Pos() const { return m_pos; }
    size_t Size() const { return m_size; }
    size_t Remaining() const { return m_pos <= m_size ? m_size - m_pos : 0; }
    const u8* Data() const { return m_data; }
    const u8* Here() const { return m_data + m_pos; }
    bool AtEnd() const { return m_pos >= m_size; }

    void Seek(size_t p) {
        if (p > m_size) throw ParseError(Fmt("seek to %zu past end %zu", p, m_size));
        m_pos = p;
    }
    void Skip(i64 n) { Seek((size_t)((i64)m_pos + n)); }

    void Need(size_t n) const {
        if (n > Remaining()) throw ParseError(Fmt("read of %zu bytes at %zu past end %zu", n, m_pos, m_size));
    }

    template <typename T> T Read() {
        Need(sizeof(T));
        T v;
        memcpy(&v, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }
    template <typename T> T Peek() const {
        Need(sizeof(T));
        T v;
        memcpy(&v, m_data + m_pos, sizeof(T));
        return v;
    }

    u8 U8() { return Read<u8>(); }
    u16 U16() { return Read<u16>(); }
    u32 U32() { return Read<u32>(); }
    u64 U64() { return Read<u64>(); }
    i32 I32() { return Read<i32>(); }
    i64 I64() { return Read<i64>(); }
    float F32() { return Read<float>(); }
    bool Bool32() { return U32() != 0; }   // UE serializes bool as a 4-byte UBOOL

    void ReadInto(void* dst, size_t n) {
        Need(n);
        memcpy(dst, m_data + m_pos, n);
        m_pos += n;
    }
    Bytes ReadBytes(size_t n) {
        Need(n);
        Bytes b(m_data + m_pos, m_data + m_pos + n);
        m_pos += n;
        return b;
    }

    // FString: int32 length incl. terminator; negative = UTF-16
    std::string FString() {
        i32 n = I32();
        if (n == 0) return {};
        if (n > 0) {
            if ((size_t)n > Remaining() || n > (1 << 24)) throw ParseError(Fmt("bad FString length %d at %zu", n, m_pos - 4));
            std::string s((const char*)Here(), n - 1);
            m_pos += n;
            return s;
        }
        if (n == INT32_MIN || (size_t)(-n) * 2 > Remaining() || -n > (1 << 23)) throw ParseError(Fmt("bad FString length %d at %zu", n, m_pos - 4));
        std::wstring w((const wchar_t*)Here(), -n - 1);
        m_pos += (size_t)(-n) * 2;
        return Narrow(w);
    }

    // TArray<T> of trivially copyable elements
    template <typename T> std::vector<T> Array(size_t maxCount = 1u << 28) {
        i32 n = I32();
        if (n < 0 || (size_t)n > maxCount || (size_t)n * sizeof(T) > Remaining()) throw ParseError(Fmt("bad array count %d at %zu", n, m_pos - 4));
        std::vector<T> v(n);
        if (n) ReadInto(v.data(), (size_t)n * sizeof(T));
        return v;
    }
    i32 Count(size_t minElemSize = 1) {
        i32 n = I32();
        if (n < 0 || (size_t)n * (minElemSize ? minElemSize : 1) > Remaining()) throw ParseError(Fmt("bad count %d at %zu", n, m_pos - 4));
        return n;
    }

private:
    const u8* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
};
