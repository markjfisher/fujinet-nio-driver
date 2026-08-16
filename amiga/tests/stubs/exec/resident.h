#ifndef NATIVE_TEST_EXEC_RESIDENT_H
#define NATIVE_TEST_EXEC_RESIDENT_H

#include <exec/types.h>

struct Resident { UWORD rt_MatchWord; struct Resident *rt_MatchTag; APTR rt_EndSkip; UBYTE rt_Flags; UBYTE rt_Version; UBYTE rt_Type; BYTE rt_Pri; char *rt_Name; char *rt_IdString; APTR rt_Init; };
#define RTC_MATCHWORD 0x4afcU
#define RTF_AUTOINIT 0x80U

#endif
