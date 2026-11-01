#ifndef NFSU_REGION_H_
#define NFSU_REGION_H_

// Version 1.4 executables of other regions converted to the layout of the build the code was generated from
// (region.cpp).

#include <x86.h>
#include <cstdint>
#include <vector>

namespace nfsu { namespace region {

struct Variable
{
    x86::reg32  address;
    x86::reg32  value;
};

struct Converted
{
    const char*             name = nullptr;
    std::vector<uint8_t>    rdata;
    std::vector<uint8_t>    data;
    std::vector<Variable>   variables;  // beyond the raw data
};

// Converts the raw .rdata and .data of another region's executable. targetRdata and targetData are the layout's
// SHA-256s, rdata and data the file's. False when no region matches or the result is not the expected one.
bool convert(const char* targetRdata, const char* targetData, const char* rdata, const char* data,
             const uint8_t* rdataBytes, const uint8_t* dataBytes, Converted& out);

}}

#endif
