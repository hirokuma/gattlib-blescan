/*
 *
 *  GattLib - GATT Library
 *
 *  Copyright (C) 2021-2024  Olivier Martin <olivier@labapart.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#define _POSIX_C_SOURCE (200809L)   // for strdup()

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "gattlib.h"

#define BLE_SCAN_TIMEOUT    (10)
#define CONNECT_DEVICE_NAME "Local" // この名前の機器にだけ接続する

static const char *adapter_name;

// We use a mutex to make the BLE connections synchronous
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

LIST_HEAD(listhead, connection_t) g_ble_connections;
struct connection_t {
    pthread_t thread;
    gattlib_adapter_t *adapter;
    char *addr;
    LIST_ENTRY(connection_t) entries;
    pthread_mutex_t done_mutex;
    pthread_cond_t done_cond;
    bool is_done;
};

static void on_device_connect(gattlib_adapter_t *adapter, const char *dst, gattlib_connection_t *conn, int error, void *user_data)
{
    gattlib_primary_service_t *services;
    gattlib_characteristic_t *characteristics;
    int services_count, characteristics_count;
    char uuid_str[MAX_LEN_UUID_STR + 1];
    int ret, i;
    struct connection_t *connection = (struct connection_t *)user_data;

    ret = gattlib_discover_primary(conn, &services, &services_count);
    if (ret != 0) {
        GATTLIB_LOG(GATTLIB_ERROR, "Fail to discover primary services.");
        goto disconnect_exit;
    }

    for (i = 0; i < services_count; i++) {
        gattlib_uuid_to_string(&services[i].uuid, uuid_str, sizeof(uuid_str));

        printf("service[%d] start_handle:%02x end_handle:%02x uuid:%s\n", i,
               services[i].attr_handle_start, services[i].attr_handle_end,
               uuid_str);
    }
    free(services);

    ret = gattlib_discover_char(conn, &characteristics, &characteristics_count);
    if (ret != 0) {
        GATTLIB_LOG(GATTLIB_ERROR, "Fail to discover characteristics.");
        goto disconnect_exit;
    }
    for (i = 0; i < characteristics_count; i++) {
        gattlib_uuid_to_string(&characteristics[i].uuid, uuid_str, sizeof(uuid_str));

        printf("characteristic[%d] properties:%02x value_handle:%04x uuid:%s\n", i,
               characteristics[i].properties, characteristics[i].value_handle,
               uuid_str);
    }
    free(characteristics);

disconnect_exit:
    ret = gattlib_disconnect(conn, true /* wait_disconnection */);
    if (ret != GATTLIB_SUCCESS) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failed to disconnect from the bluetooth device '%s'(ret=%d)", connection->addr, ret);
    }

    // Signal that we're done
    pthread_mutex_lock(&connection->done_mutex);
    connection->is_done = true;
    pthread_cond_signal(&connection->done_cond);
    pthread_mutex_unlock(&connection->done_mutex);
}

static void *ble_connect_device(void *arg)
{
    struct connection_t *connection = arg;
    char *addr = connection->addr;
    int ret;

    pthread_mutex_lock(&g_mutex);
    printf("------------START %s ---------------\n", addr);

    ret = gattlib_connect(connection->adapter, connection->addr, GATTLIB_CONNECTION_OPTIONS_NONE, on_device_connect, connection);
    if (ret != GATTLIB_SUCCESS) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failed to connect to the bluetooth device '%s'(ret=%d)", connection->addr, ret);
    }

    // Wait for the on_device_connect to complete
    pthread_mutex_lock(&connection->done_mutex);
    while (!connection->is_done) {
        pthread_cond_wait(&connection->done_cond, &connection->done_mutex);
    }
    pthread_mutex_unlock(&connection->done_mutex);

    printf("------------DONE %s ---------------\n", addr);
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static void ble_discovered_device(gattlib_adapter_t *adapter, const char *addr, const char *name, void *user_data)
{
    struct connection_t *connection;
    int ret;

    if (name) {
        printf("Discovered %s - '%s'\n", addr, name);
#ifdef CONNECT_DEVICE_NAME
        if (strcmp(name, CONNECT_DEVICE_NAME) != 0) {
            return;
        }
#else
        return;
#endif
    } else {
        printf("Discovered %s\n", addr);
        return;
    }

    connection = calloc(sizeof(struct connection_t), 1);
    if (connection == NULL) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failt to allocate connection.");
        return;
    }
    connection->addr = strdup(addr);
    connection->adapter = adapter;
    connection->is_done = false;
    pthread_mutex_init(&connection->done_mutex, NULL);
    pthread_cond_init(&connection->done_cond, NULL);

    ret = pthread_create(&connection->thread, NULL,	ble_connect_device, connection);
    if (ret != 0) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failt to create BLE connection thread.");
        free(connection);
        return;
    }
    LIST_INSERT_HEAD(&g_ble_connections, connection, entries);
}

static void *ble_task(void *arg)
{
    gattlib_adapter_t *adapter;
    int ret;

    ret = gattlib_adapter_open(adapter_name, &adapter);
    if (ret) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failed to open adapter.");
        return NULL;
    }

    pthread_mutex_lock(&g_mutex);
    ret = gattlib_adapter_scan_enable(adapter, ble_discovered_device, BLE_SCAN_TIMEOUT, NULL /* user_data */);
    if (ret) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failed to scan.");
        goto EXIT;
    }

    gattlib_adapter_scan_disable(adapter);

    puts("Scan completed");
    pthread_mutex_unlock(&g_mutex);

    // Wait for the thread to complete
    while (g_ble_connections.lh_first != NULL) {
        struct connection_t *connection = g_ble_connections.lh_first;
        pthread_join(connection->thread, NULL);
        LIST_REMOVE(g_ble_connections.lh_first, entries);
        free(connection->addr);
        pthread_mutex_destroy(&connection->done_mutex);
        pthread_cond_destroy(&connection->done_cond);
        free(connection);
    }

EXIT:
    gattlib_adapter_close(adapter);
    return NULL;
}

int main(int argc, const char *argv[])
{
    int ret;

    if (argc == 1) {
        adapter_name = NULL;
    } else if (argc == 2) {
        adapter_name = argv[1];
    } else {
        printf("%s [<bluetooth-adapter>]\n", argv[0]);
        return 1;
    }

    LIST_INIT(&g_ble_connections);

    ret = gattlib_mainloop(ble_task, NULL);
    if (ret != GATTLIB_SUCCESS) {
        GATTLIB_LOG(GATTLIB_ERROR, "Failed to create gattlib mainloop");
    }

    return ret;
}
