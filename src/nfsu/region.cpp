// Version 1.4 executables of other regions: the code layout of the build the code was generated from, their
// data at other addresses. Their .rdata and .data are converted to the layout. Written by the region table
// tool from the executables.
#include "region.h"

#include <platform/sha256.h>
#include <cstring>

namespace nfsu { namespace region {

// Layout bytes [begin, end) are the file's bytes from begin + shift.
struct Move
{
    x86::reg32  begin, end;
    int8_t      shift;
};

// A data word that points at a moved address, and what to add to it.
struct Pointer
{
    x86::reg32  address;
    int8_t      delta;
};

// A variable the file initialises at source; the layout has it at address and restore at source.
struct MovedVariable
{
    x86::reg32  address, source, restore;
};

struct Region
{
    const char*     name;
    const char*     rdata;             // SHA-256 of the raw .rdata and .data of the file
    const char*     data;
    const char*     convertedRdata;    // and after the conversion
    const char*     convertedData;
    const Move*     moves;
    size_t          moveCount;
    const Pointer*  pointers;
    size_t          pointerCount;
    const MovedVariable* variables;
    size_t          variableCount;
};

// Raw .rdata and .data of the build the tables are for.
static const char* const s_targetRdata = "45bb01ac47bfd4c330d968d2793f9edce502f062253c063df59cf92720fcb4a7";
static const char* const s_targetData = "8a6b064b100372e5b3c097c855969c61a409e70857a9329a6fd76c854172a332";

static const x86::reg32 RdataAddress = 0x697000, RdataSize = 0x3d000, DataAddress = 0x6d4000, DataSize = 0x34000;

static const Move s_moves0[] = {
    { 0x6cce94, 0x6d3fc8, 4 },
};
static const Pointer s_pointers0[] = {
    { 0x697000, -4 }, { 0x697004, -4 }, { 0x697008, -4 }, { 0x69700c, -4 }, { 0x697014, -4 }, { 0x69701c, -4 },
    { 0x697024, -4 }, { 0x697034, -4 }, { 0x697038, -4 }, { 0x697040, -4 }, { 0x697044, -4 }, { 0x697048, -4 },
    { 0x69704c, -4 }, { 0x697050, -4 }, { 0x697054, -4 }, { 0x697058, -4 }, { 0x69705c, -4 }, { 0x697060, -4 },
    { 0x697064, -4 }, { 0x697068, -4 }, { 0x69706c, -4 }, { 0x697070, -4 }, { 0x697074, -4 }, { 0x697078, -4 },
    { 0x69707c, -4 }, { 0x697080, -4 }, { 0x697084, -4 }, { 0x697088, -4 }, { 0x69708c, -4 }, { 0x697090, -4 },
    { 0x697094, -4 }, { 0x697098, -4 }, { 0x69709c, -4 }, { 0x6970a0, -4 }, { 0x6970a4, -4 }, { 0x6970a8, -4 },
    { 0x6970ac, -4 }, { 0x6970b0, -4 }, { 0x6970b4, -4 }, { 0x6970b8, -4 }, { 0x6970bc, -4 }, { 0x6970c0, -4 },
    { 0x6970c4, -4 }, { 0x6970c8, -4 }, { 0x6970cc, -4 }, { 0x6970d0, -4 }, { 0x6970d4, -4 }, { 0x6970d8, -4 },
    { 0x6970dc, -4 }, { 0x6970e0, -4 }, { 0x6970e4, -4 }, { 0x6970e8, -4 }, { 0x6970ec, -4 }, { 0x6970f0, -4 },
    { 0x6970f4, -4 }, { 0x6970f8, -4 }, { 0x6970fc, -4 }, { 0x697100, -4 }, { 0x697104, -4 }, { 0x697108, -4 },
    { 0x69710c, -4 }, { 0x697110, -4 }, { 0x697114, -4 }, { 0x697118, -4 }, { 0x69711c, -4 }, { 0x697120, -4 },
    { 0x697124, -4 }, { 0x697128, -4 }, { 0x69712c, -4 }, { 0x697130, -4 }, { 0x697134, -4 }, { 0x697138, -4 },
    { 0x69713c, -4 }, { 0x697140, -4 }, { 0x697144, -4 }, { 0x697148, -4 }, { 0x69714c, -4 }, { 0x697150, -4 },
    { 0x697154, -4 }, { 0x697158, -4 }, { 0x69715c, -4 }, { 0x697160, -4 }, { 0x697164, -4 }, { 0x697168, -4 },
    { 0x69716c, -4 }, { 0x697170, -4 }, { 0x697174, -4 }, { 0x697178, -4 }, { 0x69717c, -4 }, { 0x697180, -4 },
    { 0x697184, -4 }, { 0x697188, -4 }, { 0x69718c, -4 }, { 0x697190, -4 }, { 0x697194, -4 }, { 0x697198, -4 },
    { 0x69719c, -4 }, { 0x6971a0, -4 }, { 0x6971a4, -4 }, { 0x6971a8, -4 }, { 0x6971ac, -4 }, { 0x6971b0, -4 },
    { 0x6971b4, -4 }, { 0x6971b8, -4 }, { 0x6971bc, -4 }, { 0x6971c0, -4 }, { 0x6971c4, -4 }, { 0x6971c8, -4 },
    { 0x6971cc, -4 }, { 0x6971d0, -4 }, { 0x6971d4, -4 }, { 0x6971d8, -4 }, { 0x6971dc, -4 }, { 0x6971e0, -4 },
    { 0x6971e4, -4 }, { 0x6971e8, -4 }, { 0x6971ec, -4 }, { 0x6971f0, -4 }, { 0x6971f4, -4 }, { 0x6971f8, -4 },
    { 0x6971fc, -4 }, { 0x697200, -4 }, { 0x697204, -4 }, { 0x697208, -4 }, { 0x69720c, -4 }, { 0x697210, -4 },
    { 0x697214, -4 }, { 0x697218, -4 }, { 0x69721c, -4 }, { 0x697220, -4 }, { 0x697224, -4 }, { 0x697228, -4 },
    { 0x69722c, -4 }, { 0x697230, -4 }, { 0x697234, -4 }, { 0x697238, -4 }, { 0x69723c, -4 }, { 0x697240, -4 },
    { 0x697244, -4 }, { 0x697248, -4 }, { 0x69724c, -4 }, { 0x697250, -4 }, { 0x697254, -4 }, { 0x697258, -4 },
    { 0x69725c, -4 }, { 0x697260, -4 }, { 0x697264, -4 }, { 0x697268, -4 }, { 0x69726c, -4 }, { 0x697270, -4 },
    { 0x697274, -4 }, { 0x697278, -4 }, { 0x69727c, -4 }, { 0x697280, -4 }, { 0x697284, -4 }, { 0x697288, -4 },
    { 0x69728c, -4 }, { 0x697294, -4 }, { 0x69729c, -4 }, { 0x6972a4, -4 }, { 0x6972ac, -4 }, { 0x6972b0, -4 },
    { 0x6972b4, -4 }, { 0x6972b8, -4 }, { 0x6972bc, -4 }, { 0x6972c0, -4 }, { 0x6972c4, -4 }, { 0x6972c8, -4 },
    { 0x6972cc, -4 }, { 0x6972d4, -4 }, { 0x6972d8, -4 }, { 0x6972dc, -4 }, { 0x6972e0, -4 }, { 0x6972e4, -4 },
    { 0x6972e8, -4 }, { 0x6972ec, -4 }, { 0x6972f0, -4 }, { 0x6972f4, -4 }, { 0x6972f8, -4 }, { 0x6972fc, -4 },
    { 0x697300, -4 }, { 0x697304, -4 }, { 0x697308, -4 }, { 0x69730c, -4 }, { 0x697310, -4 }, { 0x697314, -4 },
    { 0x697318, -4 }, { 0x69731c, -4 }, { 0x697320, -4 }, { 0x697324, -4 }, { 0x697328, -4 }, { 0x69732c, -4 },
    { 0x697330, -4 }, { 0x697334, -4 }, { 0x697338, -4 }, { 0x69733c, -4 }, { 0x697340, -4 }, { 0x697344, -4 },
    { 0x697348, -4 }, { 0x69734c, -4 }, { 0x697350, -4 }, { 0x697354, -4 }, { 0x697358, -4 }, { 0x69735c, -4 },
    { 0x697360, -4 }, { 0x697364, -4 }, { 0x697368, -4 }, { 0x69736c, -4 }, { 0x697370, -4 }, { 0x697374, -4 },
    { 0x697378, -4 }, { 0x69737c, -4 }, { 0x697380, -4 }, { 0x697384, -4 }, { 0x697388, -4 }, { 0x69738c, -4 },
    { 0x697390, -4 }, { 0x697394, -4 }, { 0x697398, -4 }, { 0x69739c, -4 }, { 0x6973a4, -4 }, { 0x6973a8, -4 },
    { 0x6973ac, -4 }, { 0x6973b0, -4 }, { 0x6973b4, -4 }, { 0x6973b8, -4 }, { 0x69740c, -4 }, { 0x69742c, -4 },
    { 0x6cced0, -4 }, { 0x6ccedc, -4 }, { 0x6ccee4, -4 }, { 0x6ccf00, -4 }, { 0x6ccf2c, -4 }, { 0x6ccf58, -4 },
    { 0x6ccf84, -4 }, { 0x6ccfa8, -4 }, { 0x6ccfcc, -4 }, { 0x6ccff8, -4 }, { 0x6cd024, -4 }, { 0x6cd048, -4 },
    { 0x6cd06c, -4 }, { 0x6cd108, -4 }, { 0x6cd12c, -4 }, { 0x6cd150, -4 }, { 0x6cd174, -4 }, { 0x6cd198, -4 },
    { 0x6cd1bc, -4 }, { 0x6cd1e0, -4 }, { 0x6cd204, -4 }, { 0x6cd228, -4 }, { 0x6cd24c, -4 }, { 0x6cd270, -4 },
    { 0x6cd294, -4 }, { 0x6cd2c0, -4 }, { 0x6cd2e4, -4 }, { 0x6cd310, -4 }, { 0x6cd33c, -4 }, { 0x6cd360, -4 },
    { 0x6cd384, -4 }, { 0x6cd3b8, -4 }, { 0x6cd3dc, -4 }, { 0x6cd400, -4 }, { 0x6cd424, -4 }, { 0x6cd450, -4 },
    { 0x6cd474, -4 }, { 0x6cd498, -4 }, { 0x6cd4bc, -4 }, { 0x6cd4e0, -4 }, { 0x6cd50c, -4 }, { 0x6cd530, -4 },
    { 0x6cd554, -4 }, { 0x6cd578, -4 }, { 0x6cd5c4, -4 }, { 0x6cd5f0, -4 }, { 0x6cd614, -4 }, { 0x6cd638, -4 },
    { 0x6cd664, -4 }, { 0x6cd688, -4 }, { 0x6cd6ac, -4 }, { 0x6cd6d0, -4 }, { 0x6cd6f4, -4 }, { 0x6cd738, -4 },
    { 0x6cd75c, -4 }, { 0x6cd780, -4 }, { 0x6cd7a4, -4 }, { 0x6cd7c8, -4 }, { 0x6cd7f4, -4 }, { 0x6cd818, -4 },
    { 0x6cd83c, -4 }, { 0x6cd860, -4 }, { 0x6cd884, -4 }, { 0x6cd8d0, -4 }, { 0x6cd904, -4 }, { 0x6cd928, -4 },
    { 0x6cd94c, -4 }, { 0x6cd970, -4 }, { 0x6cd994, -4 }, { 0x6cd9b8, -4 }, { 0x6cd9dc, -4 }, { 0x6cda00, -4 },
    { 0x6cda2c, -4 }, { 0x6cda50, -4 }, { 0x6cda74, -4 }, { 0x6cda98, -4 }, { 0x6cdabc, -4 }, { 0x6cdae0, -4 },
    { 0x6cdb04, -4 }, { 0x6cdb28, -4 }, { 0x6cdb4c, -4 }, { 0x6cdb70, -4 }, { 0x6cdbbc, -4 }, { 0x6cdbe0, -4 },
    { 0x6cdc54, -4 }, { 0x6cdc88, -4 }, { 0x6cdcb4, -4 }, { 0x6cdce0, -4 }, { 0x6cdd0c, -4 }, { 0x6cdd38, -4 },
    { 0x6cdd5c, -4 }, { 0x6cdd88, -4 }, { 0x6cddd4, -4 }, { 0x6cde00, -4 }, { 0x6cde2c, -4 }, { 0x6cde50, -4 },
    { 0x6cde74, -4 }, { 0x6cde98, -4 }, { 0x6cdec4, -4 }, { 0x6cdef0, -4 }, { 0x6cdf1c, -4 }, { 0x6cdf58, -4 },
    { 0x6cdf84, -4 }, { 0x6cdfa8, -4 }, { 0x6cdfcc, -4 }, { 0x6ce010, -4 }, { 0x6ce03c, -4 }, { 0x6ce060, -4 },
    { 0x6ce084, -4 }, { 0x6ce0b0, -4 }, { 0x6ce0dc, -4 }, { 0x6ce100, -4 }, { 0x6ce124, -4 }, { 0x6ce150, -4 },
    { 0x6ce174, -4 }, { 0x6ce1a8, -4 }, { 0x6ce1d4, -4 }, { 0x6ce200, -4 }, { 0x6ce224, -4 }, { 0x6ce2c0, -4 },
    { 0x6ce2e4, -4 }, { 0x6ce308, -4 }, { 0x6ce334, -4 }, { 0x6ce360, -4 }, { 0x6ce384, -4 }, { 0x6ce3a8, -4 },
    { 0x6ce3dc, -4 }, { 0x6ce408, -4 }, { 0x6ce434, -4 }, { 0x6ce458, -4 }, { 0x6ce47c, -4 }, { 0x6ce4a0, -4 },
    { 0x6ce4c4, -4 }, { 0x6ce4e8, -4 }, { 0x6ce50c, -4 }, { 0x6ce530, -4 }, { 0x6ce554, -4 }, { 0x6ce578, -4 },
    { 0x6ce59c, -4 }, { 0x6ce5c0, -4 }, { 0x6ce5e4, -4 }, { 0x6ce608, -4 }, { 0x6ce62c, -4 }, { 0x6ce658, -4 },
    { 0x6ce67c, -4 }, { 0x6ce6b8, -4 }, { 0x6ce6dc, -4 }, { 0x6ce700, -4 }, { 0x6ce73c, -4 }, { 0x6ce768, -4 },
    { 0x6ce78c, -4 }, { 0x6ce7b0, -4 }, { 0x6ce7d4, -4 }, { 0x6ce828, -4 }, { 0x6ce864, -4 }, { 0x6ce888, -4 },
    { 0x6ce8b4, -4 }, { 0x6ce8d8, -4 }, { 0x6ce8fc, -4 }, { 0x6ce920, -4 }, { 0x6ce954, -4 }, { 0x6ce988, -4 },
    { 0x6ce9ac, -4 }, { 0x6ce9d0, -4 }, { 0x6ce9f4, -4 }, { 0x6cea18, -4 }, { 0x6cea3c, -4 }, { 0x6cea68, -4 },
    { 0x6cea8c, -4 }, { 0x6ceab0, -4 }, { 0x6cead4, -4 }, { 0x6ceaf8, -4 }, { 0x6ceb24, -4 }, { 0x6ceb48, -4 },
    { 0x6ceb7c, -4 }, { 0x6ceba8, -4 }, { 0x6cebcc, -4 }, { 0x6cebf0, -4 }, { 0x6cec14, -4 }, { 0x6cec40, -4 },
    { 0x6cec94, -4 }, { 0x6cecb8, -4 }, { 0x6cecdc, -4 }, { 0x6ced00, -4 }, { 0x6ced24, -4 }, { 0x6ced50, -4 },
    { 0x6ced7c, -4 }, { 0x6ceda8, -4 }, { 0x6cedec, -4 }, { 0x6cee28, -4 }, { 0x6cee5c, -4 }, { 0x6ceea8, -4 },
    { 0x6ceecc, -4 }, { 0x6ceef0, -4 }, { 0x6cef3c, -4 }, { 0x6cef88, -4 }, { 0x6cefac, -4 }, { 0x6cefd0, -4 },
    { 0x6ceffc, -4 }, { 0x6cf020, -4 }, { 0x6cf04c, -4 }, { 0x6cf070, -4 }, { 0x6cf094, -4 }, { 0x6cf0b8, -4 },
    { 0x6cf0ec, -4 }, { 0x6cf110, -4 }, { 0x6cf134, -4 }, { 0x6cf160, -4 }, { 0x6cf18c, -4 }, { 0x6cf1b0, -4 },
    { 0x6cf1d4, -4 }, { 0x6cf218, -4 }, { 0x6cf23c, -4 }, { 0x6cf288, -4 }, { 0x6cf2ac, -4 }, { 0x6cf2d0, -4 },
    { 0x6cf2f4, -4 }, { 0x6cf320, -4 }, { 0x6cf344, -4 }, { 0x6cf368, -4 }, { 0x6cf38c, -4 }, { 0x6cf3b0, -4 },
    { 0x6cf3d4, -4 }, { 0x6cf3f8, -4 }, { 0x6cf41c, -4 }, { 0x6cf440, -4 }, { 0x6cf464, -4 }, { 0x6cf488, -4 },
    { 0x6cf4ac, -4 }, { 0x6cf4d0, -4 }, { 0x6cf524, -4 }, { 0x6cf550, -4 }, { 0x6cf574, -4 }, { 0x6cf598, -4 },
    { 0x6cf5bc, -4 }, { 0x6cf5e0, -4 }, { 0x6cf624, -4 }, { 0x6cf648, -4 }, { 0x6cf66c, -4 }, { 0x6cf690, -4 },
    { 0x6cf6bc, -4 }, { 0x6cf750, -4 }, { 0x6cf77c, -4 }, { 0x6cf7b0, -4 }, { 0x6cf7dc, -4 }, { 0x6cf818, -4 },
    { 0x6cf83c, -4 }, { 0x6cf870, -4 }, { 0x6cf894, -4 }, { 0x6cf8b8, -4 }, { 0x6cf8ec, -4 }, { 0x6cf920, -4 },
    { 0x6cf964, -4 }, { 0x6cf988, -4 }, { 0x6cf9b4, -4 }, { 0x6cf9d8, -4 }, { 0x6cfa8c, -4 }, { 0x6cfab0, -4 },
    { 0x6cfadc, -4 }, { 0x6cfb00, -4 }, { 0x6cfb2c, -4 }, { 0x6cfb50, -4 }, { 0x6cfb7c, -4 }, { 0x6cfbc0, -4 },
    { 0x6cfbe4, -4 }, { 0x6cfc20, -4 }, { 0x6cfc84, -4 }, { 0x6cfca8, -4 }, { 0x6cfccc, -4 }, { 0x6cfcf0, -4 },
    { 0x6cfd1c, -4 }, { 0x6cfd58, -4 }, { 0x6cfd7c, -4 }, { 0x6cfdc8, -4 }, { 0x6cfdec, -4 }, { 0x6cfe10, -4 },
    { 0x6cfe44, -4 }, { 0x6cfe68, -4 }, { 0x6cfe8c, -4 }, { 0x6cfeb0, -4 }, { 0x6cfedc, -4 }, { 0x6cff00, -4 },
    { 0x6cff24, -4 }, { 0x6cff48, -4 }, { 0x6cff6c, -4 }, { 0x6cff90, -4 }, { 0x6cffb4, -4 }, { 0x6cffd8, -4 },
    { 0x6cfffc, -4 }, { 0x6d0028, -4 }, { 0x6d004c, -4 }, { 0x6d0070, -4 }, { 0x6d0094, -4 }, { 0x6d00b8, -4 },
    { 0x6d00dc, -4 }, { 0x6d0100, -4 }, { 0x6d0124, -4 }, { 0x6d0158, -4 }, { 0x6d017c, -4 }, { 0x6d01a0, -4 },
    { 0x6d01c4, -4 }, { 0x6d01e8, -4 }, { 0x6d020c, -4 }, { 0x6d0230, -4 }, { 0x6d0254, -4 }, { 0x6d0278, -4 },
    { 0x6d02a4, -4 }, { 0x6d02d8, -4 }, { 0x6d0314, -4 }, { 0x6d0358, -4 }, { 0x6d03ac, -4 }, { 0x6d03d8, -4 },
    { 0x6d0424, -4 }, { 0x6d0448, -4 }, { 0x6d046c, -4 }, { 0x6d04d8, -4 }, { 0x6d04fc, -4 }, { 0x6d0540, -4 },
    { 0x6d056c, -4 }, { 0x6d0590, -4 }, { 0x6d05b4, -4 }, { 0x6d05d8, -4 }, { 0x6d05fc, -4 }, { 0x6d0620, -4 },
    { 0x6d0644, -4 }, { 0x6d0680, -4 }, { 0x6d06a4, -4 }, { 0x6d06d8, -4 }, { 0x6d06fc, -4 }, { 0x6d0720, -4 },
    { 0x6d0744, -4 }, { 0x6d0768, -4 }, { 0x6d078c, -4 }, { 0x6d07b8, -4 }, { 0x6d07dc, -4 }, { 0x6d0800, -4 },
    { 0x6d0824, -4 }, { 0x6d0848, -4 }, { 0x6d086c, -4 }, { 0x6d0890, -4 }, { 0x6d08b4, -4 }, { 0x6d08d8, -4 },
    { 0x6d08fc, -4 }, { 0x6d0920, -4 }, { 0x6d09cc, -4 }, { 0x6d09f0, -4 }, { 0x6d0a14, -4 }, { 0x6d0a60, -4 },
    { 0x6d0a84, -4 }, { 0x6d0aa8, -4 }, { 0x6d0acc, -4 }, { 0x6d0b38, -4 }, { 0x6d0b64, -4 }, { 0x6d0ba8, -4 },
    { 0x6d0bcc, -4 }, { 0x6d0bf8, -4 }, { 0x6d0c1c, -4 }, { 0x6d0c40, -4 }, { 0x6d0c64, -4 }, { 0x6d0c90, -4 },
    { 0x6d0cb4, -4 }, { 0x6d0cd8, -4 }, { 0x6d0cfc, -4 }, { 0x6d0d28, -4 }, { 0x6d0d4c, -4 }, { 0x6d0d88, -4 },
    { 0x6d0dac, -4 }, { 0x6d0df0, -4 }, { 0x6d0e14, -4 }, { 0x6d0ea8, -4 }, { 0x6d0ecc, -4 }, { 0x6d0ef0, -4 },
    { 0x6d0f14, -4 }, { 0x6d0f60, -4 }, { 0x6d0f84, -4 }, { 0x6d0fc0, -4 }, { 0x6d0ff4, -4 }, { 0x6d1018, -4 },
    { 0x6d103c, -4 }, { 0x6d1078, -4 }, { 0x6d109c, -4 }, { 0x6d10c0, -4 }, { 0x6d10e4, -4 }, { 0x6d1118, -4 },
    { 0x6d113c, -4 }, { 0x6d1170, -4 }, { 0x6d11a4, -4 }, { 0x6d11c8, -4 }, { 0x6d11ec, -4 }, { 0x6d1210, -4 },
    { 0x6d1244, -4 }, { 0x6d1268, -4 }, { 0x6d128c, -4 }, { 0x6d12b0, -4 }, { 0x6d12d4, -4 }, { 0x6d1318, -4 },
    { 0x6d133c, -4 }, { 0x6d1370, -4 }, { 0x6d1394, -4 }, { 0x6d13b8, -4 }, { 0x6d13f4, -4 }, { 0x6d1418, -4 },
    { 0x6d143c, -4 }, { 0x6d1460, -4 }, { 0x6d1484, -4 }, { 0x6d14c0, -4 }, { 0x6d14fc, -4 }, { 0x6d1520, -4 },
    { 0x6d1544, -4 }, { 0x6d1568, -4 }, { 0x6d158c, -4 }, { 0x6d15b0, -4 }, { 0x6d15d4, -4 }, { 0x6d15f8, -4 },
    { 0x6d161c, -4 }, { 0x6d1650, -4 }, { 0x6d1674, -4 }, { 0x6d1698, -4 }, { 0x6d16cc, -4 }, { 0x6d1718, -4 },
    { 0x6d174c, -4 }, { 0x6d1780, -4 }, { 0x6d17a4, -4 }, { 0x6d17d0, -4 }, { 0x6d1804, -4 }, { 0x6d1828, -4 },
    { 0x6d184c, -4 }, { 0x6d1870, -4 }, { 0x6d18b4, -4 }, { 0x6d18d8, -4 }, { 0x6d18fc, -4 }, { 0x6d1940, -4 },
    { 0x6d1974, -4 }, { 0x6d1998, -4 }, { 0x6d19bc, -4 }, { 0x6d19e0, -4 }, { 0x6d1a04, -4 }, { 0x6d1a28, -4 },
    { 0x6d1a4c, -4 }, { 0x6d1a70, -4 }, { 0x6d1a94, -4 }, { 0x6d1ab8, -4 }, { 0x6d1afc, -4 }, { 0x6d1b20, -4 },
    { 0x6d1b44, -4 }, { 0x6d1b68, -4 }, { 0x6d1b94, -4 }, { 0x6d1bb8, -4 }, { 0x6d1bdc, -4 }, { 0x6d1c00, -4 },
    { 0x6d1c44, -4 }, { 0x6d1cb0, -4 }, { 0x6d1cd4, -4 }, { 0x6d1cf8, -4 }, { 0x6d1d1c, -4 }, { 0x6d1d40, -4 },
    { 0x6d1d64, -4 }, { 0x6d1d90, -4 }, { 0x6d1db4, -4 }, { 0x6d1de0, -4 }, { 0x6d1e0c, -4 }, { 0x6d1e20, -4 },
    { 0x6d1e2c, -4 }, { 0x6d1e34, -4 }, { 0x6d1e40, -4 }, { 0x6d1e48, -4 }, { 0x6d1e54, -4 }, { 0x6d1e5c, -4 },
    { 0x6d1e68, -4 }, { 0x6d1e70, -4 }, { 0x6d1e7c, -4 }, { 0x6d1e84, -4 }, { 0x6d1e90, -4 }, { 0x6d1e98, -4 },
    { 0x6d1ea4, -4 }, { 0x6d1eac, -4 }, { 0x6d1eb8, -4 }, { 0x6d1ec0, -4 }, { 0x6d1ecc, -4 }, { 0x6d1ed4, -4 },
    { 0x6d1ee0, -4 }, { 0x6d1ee8, -4 }, { 0x6d1ef4, -4 }, { 0x6d1efc, -4 }, { 0x6d1f08, -4 }, { 0x6d1f10, -4 },
    { 0x6d1f1c, -4 }, { 0x6d1f24, -4 }, { 0x6d1f30, -4 }, { 0x6d1f38, -4 }, { 0x6d1f44, -4 }, { 0x6d1f60, -4 },
    { 0x6d1f64, -4 }, { 0x6d1f68, -4 }, { 0x6d1f6c, -4 }, { 0x6d1f74, -4 }, { 0x6d1f7c, -4 }, { 0x6d1f84, -4 },
    { 0x6d1f94, -4 }, { 0x6d1f98, -4 }, { 0x6d1fa0, -4 }, { 0x6d1fa4, -4 }, { 0x6d1fa8, -4 }, { 0x6d1fac, -4 },
    { 0x6d1fb0, -4 }, { 0x6d1fb4, -4 }, { 0x6d1fb8, -4 }, { 0x6d1fbc, -4 }, { 0x6d1fc0, -4 }, { 0x6d1fc4, -4 },
    { 0x6d1fc8, -4 }, { 0x6d1fcc, -4 }, { 0x6d1fd0, -4 }, { 0x6d1fd4, -4 }, { 0x6d1fd8, -4 }, { 0x6d1fdc, -4 },
    { 0x6d1fe0, -4 }, { 0x6d1fe4, -4 }, { 0x6d1fe8, -4 }, { 0x6d1fec, -4 }, { 0x6d1ff0, -4 }, { 0x6d1ff4, -4 },
    { 0x6d1ff8, -4 }, { 0x6d1ffc, -4 }, { 0x6d2000, -4 }, { 0x6d2004, -4 }, { 0x6d2008, -4 }, { 0x6d200c, -4 },
    { 0x6d2010, -4 }, { 0x6d2014, -4 }, { 0x6d2018, -4 }, { 0x6d201c, -4 }, { 0x6d2020, -4 }, { 0x6d2024, -4 },
    { 0x6d2028, -4 }, { 0x6d202c, -4 }, { 0x6d2030, -4 }, { 0x6d2034, -4 }, { 0x6d2038, -4 }, { 0x6d203c, -4 },
    { 0x6d2040, -4 }, { 0x6d2044, -4 }, { 0x6d2048, -4 }, { 0x6d204c, -4 }, { 0x6d2050, -4 }, { 0x6d2054, -4 },
    { 0x6d2058, -4 }, { 0x6d205c, -4 }, { 0x6d2060, -4 }, { 0x6d2064, -4 }, { 0x6d2068, -4 }, { 0x6d206c, -4 },
    { 0x6d2070, -4 }, { 0x6d2074, -4 }, { 0x6d2078, -4 }, { 0x6d207c, -4 }, { 0x6d2080, -4 }, { 0x6d2084, -4 },
    { 0x6d2088, -4 }, { 0x6d208c, -4 }, { 0x6d2090, -4 }, { 0x6d2094, -4 }, { 0x6d2098, -4 }, { 0x6d209c, -4 },
    { 0x6d20a0, -4 }, { 0x6d20a4, -4 }, { 0x6d20a8, -4 }, { 0x6d20ac, -4 }, { 0x6d20b0, -4 }, { 0x6d20b4, -4 },
    { 0x6d20b8, -4 }, { 0x6d20bc, -4 }, { 0x6d20c0, -4 }, { 0x6d20c4, -4 }, { 0x6d20c8, -4 }, { 0x6d20cc, -4 },
    { 0x6d20d0, -4 }, { 0x6d20d4, -4 }, { 0x6d20d8, -4 }, { 0x6d20dc, -4 }, { 0x6d20e0, -4 }, { 0x6d20e4, -4 },
    { 0x6d20e8, -4 }, { 0x6d20ec, -4 }, { 0x6d20f0, -4 }, { 0x6d20f4, -4 }, { 0x6d20f8, -4 }, { 0x6d20fc, -4 },
    { 0x6d2100, -4 }, { 0x6d2104, -4 }, { 0x6d2108, -4 }, { 0x6d210c, -4 }, { 0x6d2110, -4 }, { 0x6d2114, -4 },
    { 0x6d2118, -4 }, { 0x6d211c, -4 }, { 0x6d2120, -4 }, { 0x6d2124, -4 }, { 0x6d2128, -4 }, { 0x6d212c, -4 },
    { 0x6d2130, -4 }, { 0x6d2134, -4 }, { 0x6d2138, -4 }, { 0x6d213c, -4 }, { 0x6d2140, -4 }, { 0x6d2144, -4 },
    { 0x6d2148, -4 }, { 0x6d214c, -4 }, { 0x6d2150, -4 }, { 0x6d2154, -4 }, { 0x6d2158, -4 }, { 0x6d215c, -4 },
    { 0x6d2160, -4 }, { 0x6d2164, -4 }, { 0x6d2168, -4 }, { 0x6d216c, -4 }, { 0x6d2170, -4 }, { 0x6d2174, -4 },
    { 0x6d2178, -4 }, { 0x6d217c, -4 }, { 0x6d2180, -4 }, { 0x6d2184, -4 }, { 0x6d2188, -4 }, { 0x6d218c, -4 },
    { 0x6d2190, -4 }, { 0x6d2194, -4 }, { 0x6d2198, -4 }, { 0x6d219c, -4 }, { 0x6d21a0, -4 }, { 0x6d21a4, -4 },
    { 0x6d21a8, -4 }, { 0x6d21ac, -4 }, { 0x6d21b0, -4 }, { 0x6d21b4, -4 }, { 0x6d21b8, -4 }, { 0x6d21bc, -4 },
    { 0x6d21c0, -4 }, { 0x6d21c4, -4 }, { 0x6d21c8, -4 }, { 0x6d21cc, -4 }, { 0x6d21d0, -4 }, { 0x6d21d4, -4 },
    { 0x6d21d8, -4 }, { 0x6d21dc, -4 }, { 0x6d21e0, -4 }, { 0x6d21e4, -4 }, { 0x6d21e8, -4 }, { 0x6d21ec, -4 },
    { 0x6d21f4, -4 }, { 0x6d21fc, -4 }, { 0x6d2204, -4 }, { 0x6d220c, -4 }, { 0x6d2210, -4 }, { 0x6d2214, -4 },
    { 0x6d2218, -4 }, { 0x6d221c, -4 }, { 0x6d2220, -4 }, { 0x6d2224, -4 }, { 0x6d2228, -4 }, { 0x6d222c, -4 },
    { 0x6d2234, -4 }, { 0x6d2238, -4 }, { 0x6d223c, -4 }, { 0x6d2240, -4 }, { 0x6d2244, -4 }, { 0x6d2248, -4 },
    { 0x6d224c, -4 }, { 0x6d2250, -4 }, { 0x6d2254, -4 }, { 0x6d2258, -4 }, { 0x6d225c, -4 }, { 0x6d2260, -4 },
    { 0x6d2264, -4 }, { 0x6d2268, -4 }, { 0x6d226c, -4 }, { 0x6d2270, -4 }, { 0x6d2274, -4 }, { 0x6d2278, -4 },
    { 0x6d227c, -4 }, { 0x6d2280, -4 }, { 0x6d2284, -4 }, { 0x6d2288, -4 }, { 0x6d228c, -4 }, { 0x6d2290, -4 },
    { 0x6d2294, -4 }, { 0x6d2298, -4 }, { 0x6d229c, -4 }, { 0x6d22a0, -4 }, { 0x6d22a4, -4 }, { 0x6d22a8, -4 },
    { 0x6d22ac, -4 }, { 0x6d22b0, -4 }, { 0x6d22b4, -4 }, { 0x6d22b8, -4 }, { 0x6d22bc, -4 }, { 0x6d22c0, -4 },
    { 0x6d22c4, -4 }, { 0x6d22c8, -4 }, { 0x6d22cc, -4 }, { 0x6d22d0, -4 }, { 0x6d22d4, -4 }, { 0x6d22d8, -4 },
    { 0x6d22dc, -4 }, { 0x6d22e0, -4 }, { 0x6d22e4, -4 }, { 0x6d22e8, -4 }, { 0x6d22ec, -4 }, { 0x6d22f0, -4 },
    { 0x6d22f4, -4 }, { 0x6d22f8, -4 }, { 0x6d22fc, -4 }, { 0x6d2304, -4 }, { 0x6d2308, -4 }, { 0x6d230c, -4 },
    { 0x6d2310, -4 }, { 0x6d2314, -4 }, { 0x6d2318, -4 }, { 0x6d236c, -4 }, { 0x6d238c, -4 }, { 0x6eb1c0, 32 },
    { 0x6eb1c8, 32 }, { 0x6f03f8, 16 }, { 0x6f0728, 16 }, { 0x6f072c, 16 }, { 0x6f0730, 16 }, { 0x6f0734, 16 },
    { 0x6f3df0, 16 }, { 0x700edc, 8 }, { 0x7064f4, 16 },
};
static const MovedVariable s_variables0[] = {
    { 0x734998, 0x6f21ac, 0x0 },
};

static const Region s_regions[] = {
    { "European",
      "de09348e5dfecdc1cc9425212ee946963bca49719a529c185e518a5cd7a4eb2b",
      "918a0665bd52c6add801feae54ac2989985c2425657e0e0b617330254b4fa9ad",
      "63dc1bb38a2f0cc9cdf2d36aa2ec536cdb8676c1d5c2b25276672618d6b8c054",
      "8a6b064b100372e5b3c097c855969c61a409e70857a9329a6fd76c854172a332",
      s_moves0, 1, s_pointers0, 957, s_variables0, 1 },
};

bool convert(const char* targetRdata, const char* targetData, const char* rdata, const char* data,
             const uint8_t* rdataBytes, const uint8_t* dataBytes, Converted& out)
{
    if (strcmp(targetRdata, s_targetRdata) || strcmp(targetData, s_targetData))
        return false;
    for (const Region& r : s_regions)
    {
        if (strcmp(rdata, r.rdata) || strcmp(data, r.data))
            continue;
        out.name = r.name;
        out.rdata.assign(rdataBytes, rdataBytes + RdataSize);
        out.data.assign(dataBytes, dataBytes + DataSize);
        for (size_t i = 0; i < r.moveCount; ++i)
        {
            const Move& m = r.moves[i];
            memcpy(out.rdata.data() + (m.begin - RdataAddress), rdataBytes + (m.begin + m.shift - RdataAddress),
                   m.end - m.begin);
        }
        for (size_t i = 0; i < r.variableCount; ++i)
        {
            const MovedVariable& v = r.variables[i];
            x86::reg32 value;
            memcpy(&value, dataBytes + (v.source - DataAddress), 4);
            out.variables.push_back({ v.address, value });
            memcpy(out.data.data() + (v.source - DataAddress), &v.restore, 4);
        }
        for (size_t i = 0; i < r.pointerCount; ++i)
        {
            const Pointer& p = r.pointers[i];
            uint8_t* word = p.address < DataAddress ? out.rdata.data() + (p.address - RdataAddress)
                                                    : out.data.data() + (p.address - DataAddress);
            uint32_t value;
            memcpy(&value, word, 4);
            value += uint32_t(int32_t(p.delta));
            memcpy(word, &value, 4);
        }
        return pf::sha256Hex(out.rdata.data(), out.rdata.size()) == r.convertedRdata
            && pf::sha256Hex(out.data.data(), out.data.size()) == r.convertedData;
    }
    return false;
}

}}
