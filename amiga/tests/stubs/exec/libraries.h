#ifndef NATIVE_TEST_EXEC_LIBRARIES_H
#define NATIVE_TEST_EXEC_LIBRARIES_H

#include <exec/types.h>

#define LIBF_DELEXP 0x80U

struct Library { UWORD lib_OpenCnt; UBYTE lib_Flags; };
struct ExecBase { int unused; };

#endif
