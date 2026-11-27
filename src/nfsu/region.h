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

// A copy-protected 1.4 executable of the layout's own build or of a region here: the protection encrypts its code
// and .data, but its .rdata stays readable apart from the imports. rdataBytes are that .rdata, layoutRdata and
// layoutData the raw sections of the layout's build (the embedded executable). The build whose conversion makes the
// .rdata closest to the layout's is taken; share is the fraction of bytes then equal (0 to 1). Returns the build's
// name, or nullptr with error set when nothing reaches 90%. For the layout's own build out stays without data (the
// embedded sections are used as they are). For a region out gets the file's .rdata converted, and the layout's
// .data, which is what the region's .data converts to, with the region's moved variables at their recorded values.
const char* convertProtected(const uint8_t* rdataBytes, size_t rdataSize, const uint8_t* layoutRdata,
                             const uint8_t* layoutData, Converted& out, double& share, const char*& error);

}}

#endif
