#include "fujinet_disk_driver.h"

int main(void)
{
    return fujinet_nio_disk_client.init != NULL &&
           fujinet_nio_disk_client.mount != NULL &&
           fujinet_nio_disk_client.info != NULL &&
           fujinet_nio_disk_client.read_sector != NULL ? 0 : 1;
}
