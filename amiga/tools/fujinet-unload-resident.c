#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *device_name;
    struct Node *node;
    struct Node *verify_node;

    if (argc != 2) {
        fprintf(stderr, "Usage: fujinet-unload-resident DEVICE-NAME\n");
        return RETURN_ERROR;
    }
    device_name = argv[1];

    /* First critical section: FindName + RemDevice if found */
    Forbid();
    node = FindName(&SysBase->DeviceList, (CONST_STRPTR)device_name);
    if (node != NULL) {
        RemDevice((struct Device *)node);
    }
    Permit();

    if (node == NULL) {
        printf("Not resident: %s\n", device_name);
        return RETURN_FAIL;
    }

    /* Second critical section: fresh FindName to verify removal */
    Forbid();
    verify_node = FindName(&SysBase->DeviceList, (CONST_STRPTR)device_name);
    Permit();

    if (verify_node == NULL) {
        printf("Unloaded: %s\n", device_name);
        return RETURN_OK;
    } else {
        printf("Still resident: %s\n", device_name);
        return RETURN_FAIL;
    }
}
