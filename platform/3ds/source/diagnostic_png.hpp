#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// write_diagnostic_rgb_png(): allocation-free, uncompressed RGB PNG writer for diagnostic
// captures. Used by diagnostics_3ds.cpp; host-tested by tests/diagnostic_png_test.cpp.

namespace starfox::platform_3ds {
namespace png_detail {
// PNG CRC-32 continued from `c`: start with 0xffffffff, invert the result.
inline std::uint32_t crc(std::uint32_t c, const std::uint8_t* bytes, std::size_t size) noexcept {
    for (std::size_t i=0; i<size; ++i) {
        c ^= bytes[i];
        for (unsigned bit=0; bit<8; ++bit)
            c=(c>>1)^((c&1U)?0xedb88320U:0U);
    }
    return c;
}
inline void big_endian(std::uint8_t* out, std::uint32_t n) noexcept {
    for (unsigned i=0;i<4;++i) out[i]=std::uint8_t(n>>(24U-8U*i));
}
// Writes one chunk: length, four-letter type, data, CRC. False on a short write.
inline bool chunk(FILE* file, const char* type, const std::uint8_t* bytes,
                  std::size_t size) noexcept {
    std::array<std::uint8_t,4> word{};
    big_endian(word.data(),std::uint32_t(size));
    if (std::fwrite(word.data(),1,4,file)!=4 || std::fwrite(type,1,4,file)!=4
        || (size && std::fwrite(bytes,1,size,file)!=size)) return false;
    auto checksum=crc(0xffffffffU,reinterpret_cast<const std::uint8_t*>(type),4);
    checksum=crc(checksum,bytes,size);
    big_endian(word.data(),~checksum);
    return std::fwrite(word.data(),1,4,file)==4;
}
}

// One stored DEFLATE block and IDAT chunk per row. No image-sized vectors,
// reallocations, or C++ heap allocations, including under memory pressure.
// IDAT chunks together form one zlib stream, with a single Adler checksum.
// Writes a width x height image (at most 400x240) of 8-bit RGB;
// pixel_at(x, y) returns the R, G, B bytes as rgb[0..2]. Returns false for a
// null file, a size out of range or a short write, which leaves the file
// incomplete.
template<class PixelAt>
bool write_diagnostic_rgb_png(FILE* file, unsigned width, unsigned height,
                              PixelAt pixel_at) {
    if (!file || !width || width>400U || !height || height>240U) return false;
    constexpr std::array<std::uint8_t,8> signature{137,80,78,71,13,10,26,10};
    std::array<std::uint8_t,13> header{};
    png_detail::big_endian(header.data(),width);
    png_detail::big_endian(header.data()+4,height);
    header[8]=8; header[9]=2; // 8-bit RGB, no interlace
    if (std::fwrite(signature.data(),1,signature.size(),file)!=signature.size()
        || !png_detail::chunk(file,"IHDR",header.data(),header.size())) return false;
    std::array<std::uint8_t,400U*3U+12U> row{};
    std::uint32_t a=1,b=0;
    const unsigned length=width*3U+1U;
    for (unsigned y=0;y<height;++y) {
        unsigned at=0;
        if (!y) {row[at++]=0x78;row[at++]=0x01;}
        row[at++]=(y+1U==height)?1U:0U;
        row[at++]=std::uint8_t(length);row[at++]=std::uint8_t(length>>8U);
        row[at++]=std::uint8_t(~length);row[at++]=std::uint8_t((~length)>>8U);
        const unsigned raw_begin=at;
        row[at++]=0; // PNG filter None
        for (unsigned x=0;x<width;++x) {
            const auto rgb=pixel_at(x,y);
            for (unsigned channel=0;channel<3;++channel) row[at++]=rgb[channel];
        }
        for (unsigned i=raw_begin;i<at;++i) {
            a+=row[i]; if (a>=65521U) a-=65521U;
            b+=a; if (b>=65521U) b-=65521U;
        }
        if (y+1U==height) {png_detail::big_endian(row.data()+at,(b<<16U)|a);at+=4U;}
        if (!png_detail::chunk(file,"IDAT",row.data(),at)) return false;
    }
    return png_detail::chunk(file,"IEND",nullptr,0);
}
}
