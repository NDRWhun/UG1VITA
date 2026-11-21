#ifndef NFSU_COLL_H_
#define NFSU_COLL_H_

#include <cstdint>

namespace nfsu { namespace coll
{

// A journal of the stores the native topology queries (0x440a40, 0x4407b0) make, on the calling thread only.
// journalBegin starts an empty one; journalEnd stops it and forgets it. journalRewind puts every logged store back
// in reverse order (memory as at journalBegin), keeps the values it replaced, goes on logging from empty and returns the number of stores put back.
// journalRollback puts back the stores logged since (or since journalBegin), then the values journalRewind
// replaced, ends the journal and returns the number of queries that ran unlogged since journalBegin (guest or
// shadow path).
void journalBegin();
void journalEnd();
uint32_t journalRewind();
uint32_t journalRollback();

}}

#endif
