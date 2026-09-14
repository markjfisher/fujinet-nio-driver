#ifndef NATIVE_TEST_EXEC_DEVICES_H
#define NATIVE_TEST_EXEC_DEVICES_H

#include <exec/libraries.h>

struct Node { struct Node *ln_Succ; struct Node *ln_Pred; UBYTE ln_Type; };
struct MsgPort { unsigned native_replies; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; UWORD mn_Length; };
struct Unit { ULONG flags; };
struct Device { struct Library dd_Library; };
struct List { struct Node *lh_Head; struct Node *lh_Tail; struct Node *lh_TailPred; };
struct Interrupt { int unused; };

#define NT_MESSAGE 5
#define NT_DEVICE 3

#endif
