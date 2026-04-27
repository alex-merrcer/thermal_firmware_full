#include "app_pm_registry.h"

#include <string.h>

#define APP_PM_REGISTRY_MAX_CLIENTS 16U

static const app_pm_client_t *s_clients[APP_PM_REGISTRY_MAX_CLIENTS];
static uint8_t s_client_count = 0U;

static uint8_t app_pm_client_name_equal(const char *lhs, const char *rhs)
{
    if (lhs == 0 || rhs == 0)
    {
        return 0U;
    }

    return (strcmp(lhs, rhs) == 0) ? 1U : 0U;
}

uint8_t app_pm_register_client(const app_pm_client_t *client)
{
    uint8_t i = 0U;

    if (client == 0 || client->name == 0)
    {
        return 0U;
    }

    for (i = 0U; i < s_client_count; ++i)
    {
        if (s_clients[i] == client ||
            app_pm_client_name_equal(s_clients[i]->name, client->name) != 0U)
        {
            return 1U;
        }
    }

    if (s_client_count >= APP_PM_REGISTRY_MAX_CLIENTS)
    {
        return 0U;
    }

    s_clients[s_client_count++] = client;
    return 1U;
}

uint8_t app_pm_can_enter_stop(void)
{
    uint8_t i = 0U;

    for (i = 0U; i < s_client_count; ++i)
    {
        if (s_clients[i]->can_sleep != 0 &&
            s_clients[i]->can_sleep() == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

void app_pm_prepare_stop(void)
{
    uint8_t i = 0U;

    for (i = 0U; i < s_client_count; ++i)
    {
        if (s_clients[i]->prepare_stop != 0)
        {
            s_clients[i]->prepare_stop();
        }
    }
}

void app_pm_restore_stop(void)
{
    uint8_t i = 0U;

    for (i = 0U; i < s_client_count; ++i)
    {
        if (s_clients[i]->restore_stop != 0)
        {
            s_clients[i]->restore_stop();
        }
    }
}

void app_pm_prepare_standby(void)
{
    uint8_t i = 0U;

    for (i = 0U; i < s_client_count; ++i)
    {
        if (s_clients[i]->prepare_standby != 0)
        {
            s_clients[i]->prepare_standby();
        }
    }
}
