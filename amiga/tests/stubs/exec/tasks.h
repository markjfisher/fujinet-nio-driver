#ifndef NATIVE_TEST_EXEC_TASKS_H
#define NATIVE_TEST_EXEC_TASKS_H

#include <exec/devices.h>

struct Task {
    struct Node tc_Node;
    APTR tc_SPReg;
    APTR tc_SPLower;
    APTR tc_SPUpper;
    APTR tc_UserData;
};

#endif
