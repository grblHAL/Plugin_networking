//
// mqtt.c - MQTT client API for grblHAL
//
// v0.1 / 2023-02-12 / Io Engineering / Terje
//

/*

Copyright (c) 2023, Terje Io
Copyright (c) 2017, Erich Styger - some code lifted from https://dzone.com/articles/mqtt-with-lwip-and-the-nxp-frdm-k64f

All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its contributors may
be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifdef ARDUINO
#include "../driver.h"
#else
#include "driver.h"
#endif

#if MQTT_ENABLE

#include <assert.h>
#include <string.h>

#include "networking.h"

static uint32_t retries = 0;
static bool connecting = false;
static mqtt_client_t *client;
static struct mqtt_connect_client_info_t client_info = {0};
static mqtt_settings_t *cfg;

mqtt_events_t mqtt_events;

static struct {
    char topic[31];
    char *payload, *target;
    size_t payload_length, received_length;
    bool overflow;
} mqtt_message = {0};

static bool do_connect (void);

static void incoming_publish_callback (void *arg, const char *topic, u32_t tot_len)
{
    if(mqtt_message.payload) {
        free(mqtt_message.payload);
        mqtt_message.payload = NULL;
    }

    if(!(mqtt_message.overflow = (mqtt_message.payload = mqtt_message.target = malloc(tot_len + 1)) == NULL)) {
        strcpy(mqtt_message.topic, topic);
        mqtt_message.payload_length = tot_len;
        mqtt_message.received_length = 0;
    }
}

static void incoming_data_callback (void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    if(mqtt_message.payload == NULL)
        return;

    if(flags & MQTT_DATA_FLAG_LAST) {

        if(len > 0 && !mqtt_message.overflow && !(len > mqtt_message.payload_length - mqtt_message.received_length)) {
            memcpy(mqtt_message.target, data, len);
            mqtt_message.received_length += len;
        }

        if(mqtt_message.payload_length == mqtt_message.received_length) {

            mqtt_message.payload[mqtt_message.payload_length] = '\0';

            if(arg != NULL)
                ((on_mqtt_message_received_ptr)arg)(mqtt_message.topic, (void *)mqtt_message.payload, mqtt_message.payload_length);
            else if(mqtt_events.on_message_received)
                mqtt_events.on_message_received(mqtt_message.topic, (void *)mqtt_message.payload, mqtt_message.payload_length);
        }

        free(mqtt_message.payload);
        mqtt_message.payload = NULL;

    } else if(len > 0 && !mqtt_message.overflow && !(mqtt_message.overflow = len > mqtt_message.payload_length - mqtt_message.received_length)) {
        memcpy(mqtt_message.target, data, len);
        mqtt_message.target += len;
        mqtt_message.received_length += len;
    } // else error?
}

static void sub_request_callback (void *arg, err_t result)
{
/* Just print the result code here for simplicity,
   normal behaviour would be to take some action if subscribe fails like
   notifying user, retry subscribe or disconnect from server
   printf("Subscribe result: %d\n", result);
*/
}

static void connection_callback (mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    switch(status) {

        case MQTT_CONNECT_ACCEPTED:
            retries = 0;
            mqtt_set_inpub_callback(client, incoming_publish_callback, incoming_data_callback, arg);
            if(mqtt_events.on_client_connected)
                mqtt_events.on_client_connected(true);
            break;

        case MQTT_CONNECT_DISCONNECTED:
        case MQTT_CONNECT_TIMEOUT:
            retries++;
            connecting = false;
            if(mqtt_events.on_client_connected)
                mqtt_events.on_client_connected(false);
            if(retries < 10) // TODO: retry after delay...
                do_connect();
            break;

        default:
            retries++;
            connecting = false;
            if(mqtt_events.on_client_connected)
                mqtt_events.on_client_connected(false);
            break;
    }
}

static bool do_connect (void)
{
    if(client == NULL)
        client = mqtt_client_new();

    if(!connecting && client && mqtt_client_connect(client, (ip_addr_t *)&cfg->ip, cfg->port, connection_callback, NULL, &client_info) != ERR_OK)
        connecting = true;

    return connecting;
}

bool mqtt_subscribe_topic (const char *topic, uint8_t qos, on_mqtt_message_received_ptr on_message_received)
{
    return mqtt_subscribe(client, topic, (u8_t)qos, sub_request_callback, on_message_received) == ERR_OK;
}

bool mqtt_unsubscribe_topic (const char *topic, on_mqtt_message_received_ptr on_message_received)
{
    return mqtt_unsubscribe(client, topic, sub_request_callback, on_message_received) == ERR_OK;
}

bool mqtt_publish_message (const char *topic, const void *payload, size_t payload_length, uint8_t qos, bool retain)
{
    return mqtt_publish(client, topic, payload, (u16_t)payload_length, (u8_t)qos, (u8_t)retain, NULL, NULL) == ERR_OK;
}

bool mqtt_connect (mqtt_settings_t *settings, const char *client_id)
{
    cfg = settings;
    client_info.client_id = client_id;
    client_info.client_user = cfg->user;
    client_info.client_pass = cfg->password;

    if(!connecting && cfg->port > 0 && !networking_ismemnull(cfg->ip, sizeof(cfg->ip)))
        connecting = do_connect();

    return connecting;
}

#endif
