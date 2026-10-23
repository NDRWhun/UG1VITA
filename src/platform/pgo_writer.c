/* Writes the raw profile with its value profiles to a file. */
#include <stdio.h>
#include "profile/InstrProfiling.h"
#include "profile/InstrProfilingInternal.h"

static uint32_t fileWrite(ProfDataWriter* writer, ProfDataIOVec* iovecs, uint32_t count)
{
    FILE* file = (FILE*)writer->WriterCtx;
    static const char zeros[64];
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t size = iovecs[i].ElmSize * iovecs[i].NumElm;
        if (iovecs[i].Data)
        {
            if (fwrite(iovecs[i].Data, 1, size, file) != size)
                return 1;
        }
        else if (iovecs[i].UseZeroPadding)
        {
            for (size_t left = size; left;)
            {
                const size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
                if (fwrite(zeros, 1, chunk, file) != chunk)
                    return 1;
                left -= chunk;
            }
        }
        else if (fseek(file, (long)size, SEEK_CUR) != 0)
            return 1;
    }
    return 0;
}

int nfsu_profile_write(FILE* file)
{
    ProfDataWriter writer = { fileWrite, file };
    return lprofWriteData(&writer, lprofGetVPDataReader(), 0);
}
