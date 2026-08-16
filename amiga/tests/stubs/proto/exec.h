#ifndef NATIVE_TEST_PROTO_EXEC_H
#define NATIVE_TEST_PROTO_EXEC_H

#include <exec/io.h>

void Disable(void);
void Enable(void);
void ReplyMsg(struct Message *message);
void Cause(struct Interrupt *interrupt);
APTR AllocMem(ULONG bytes, ULONG flags);
void FreeMem(APTR memory, ULONG bytes);
void Remove(struct Node *node);
void AddTail(struct List *list, struct Node *node);

#endif
