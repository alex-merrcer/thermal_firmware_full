#include "app_platform.h"
#include "app_services.h"

void app_main(void)
{
    app_platform_init();
    app_drivers_init();
    app_services_init();
    app_services_start();
}
