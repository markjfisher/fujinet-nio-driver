#ifndef NATIVE_TEST_ALIB_PROTOS_H
#define NATIVE_TEST_ALIB_PROTOS_H
#include <exec/ports.h>
struct MsgPort *CreatePort(CONST_STRPTR name, LONG priority);
void DeletePort(struct MsgPort *port);
#endif
