#ifndef NFSU_BANKCODE_H_
#define NFSU_BANKCODE_H_

// Runs the x86 routines inside loaded EA audio banks (bankcode.cpp).

namespace nfsu { namespace bankcode {

// Lets calls through pointers reach the routines of loaded banks; once at start-up.
void install();

}}

#endif /* !NFSU_BANKCODE_H_ */
