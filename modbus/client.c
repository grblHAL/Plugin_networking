/*

  modbus/client.c - a lightweight ModBus TCP client implementation

  Part of grblHAL

  Copyright (c) 2023 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifdef ARDUINO
#include "../../driver.h"
#else
#include "driver.h"
#endif

#if MODBUS_ENABLE & MODBUS_TCP_ENABLED

#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include "../../grbl/protocol.h"
#include "../../grbl/settings.h"
#include "../../grbl/nvs_buffer.h"
#include "../../grbl/state_machine.h"
#else
#include "grbl/protocol.h"
#include "grbl/settings.h"
#include "grbl/nvs_buffer.h"
#include "grbl/state_machine.h"
#endif

#include "../networking.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/def.h"

#define MODBUS_CLIENT_POLL_INTERVAL 4

#ifndef MODBUS_N_CLIENTS
#define MODBUS_N_CLIENTS 4
#endif
#if MODBUS_N_CLIENTS > 8
#undef MODBUS_N_CLIENTS
#define MODBUS_N_CLIENTS
#endif

typedef struct queue_entry {
    volatile bool sync;
    uint32_t timeout;
    void *context;
    modbus_callbacks_t callbacks;
    struct queue_entry *next;
    uint32_t msg_length;
    modbus_tcp_adu_t adu;
} queue_entry_t;

typedef struct {
    struct altcp_pcb *pcb;
    bool connected;
    bool tx_busy;
    modbus_tcp_settings_t *settings;
} modbus_session_t;

static uint16_t tid = 0;
static queue_entry_t *queue = NULL;
static modbus_session_t session[MODBUS_N_CLIENTS];
static modbus_tcp_settings_t modbus[MODBUS_N_CLIENTS];
static volatile bool spin_lock = false, is_up = false;

static driver_reset_ptr driver_reset;
static nvs_address_t nvs_address;

static void modbus_process (void *arg, struct altcp_pcb *pcb, struct pbuf *p);
static err_t modbus_client_connect (modbus_session_t *session);
static void modbus_tcp_flush_queue (void);

modbus_tcp_pdu_t test = {
   .length = 6,
   .uid = 1,
   .code = ModBus_WriteRegister,
   .data[0] = 0,
   .data[1] = 0,
   .data[2] = 0,
   .data[3] = 0x0c
};

static queue_entry_t *unlink_msg (queue_entry_t *qd)
{
    queue_entry_t *q = queue, *qp = NULL;

    if(q) do {
        if(q == qd) {
            if(qp)
                qp->next = q->next;
            else
                queue = NULL;
            free(qd);
            q = NULL;
        } else
            qp = q;
    } while (q && (q = q->next));

    return qp;
}

bool modbus_tcp_send (modbus_tcp_pdu_t *pdu, const modbus_callbacks_t *callbacks, void *context, bool block)
{
    modbus_session_t *s = NULL;
    uint_fast8_t idx = MODBUS_N_CLIENTS;
    do {
        idx--;
        if(session[idx].settings->id == pdu->uid)
            s = &session[idx];
    } while(idx && s == NULL);

    if(pdu->uid == 0 || s == NULL)
        return false;

    if(!s->connected)
        modbus_client_connect(s);

    uint32_t msg_length = offsetof(modbus_tcp_adu_t, pdu.uid) + pdu->length;
    queue_entry_t *q = calloc(sizeof(queue_entry_t) + msg_length, 1);

    q->context = context;
    q->msg_length = msg_length;
    q->timeout = 0;
    q->next = NULL;
    q->adu.tid = ++tid;
    q->adu.pid = 0;
    q->sync = block;
    memcpy((uint8_t *)&q->adu.pdu, pdu, pdu->length + 2);

    q->adu.tid = lwip_htons(q->adu.tid);
    q->adu.pid = lwip_htons(q->adu.pid);
    q->adu.pdu.length = lwip_htons(q->adu.pdu.length);

    if(callbacks)
        memcpy(&q->callbacks, callbacks, sizeof(modbus_callbacks_t));
    else {
        q->callbacks.on_rx_packet = NULL;
        q->callbacks.on_rx_exception = NULL;
    }

    if(queue == NULL)
        queue = q;
    else {

        queue_entry_t *qa = queue;

        while(qa->next)
            qa = qa->next;

        qa->next = q;
    }

    if(block) {

        uint32_t ms = hal.get_elapsed_ticks();

        modbus_process(s, s->pcb, NULL);

        while(q->sync && !(ms - q->timeout >= 50)) {
            grbl.on_execute_realtime(state_get());
            ms = hal.get_elapsed_ticks();
        }

        unlink_msg(q);
    }

    return true;
}

static bool modbus_rtu_send (modbus_message_t *msg, const modbus_callbacks_t *callbacks, bool block)
{
    bool ok;

    if((ok = msg->adu[0] == modbus[0].id)) {

        modbus_tcp_pdu_t pdu;

        pdu.uid = msg->adu[0];
        pdu.code = msg->adu[1];
        pdu.length = msg->tx_length - 2;
        memcpy(pdu.data, &msg->adu[2], pdu.length - 2);

        modbus_tcp_send(&pdu, callbacks, msg->context, block);
    }

    return ok;
}

static void modbus_reset (void)
{
    while(spin_lock);

    modbus_tcp_flush_queue();

    driver_reset();
}

static void modbus_process (void *arg, struct altcp_pcb *pcb, struct pbuf *p)
{
    modbus_session_t *s = (modbus_session_t *)arg;

    if(arg == NULL) {
        /* already closed connection */
        if(p != NULL) {
            LWIP_DEBUGF(MODBUS_DEBUG_TRACE, ("Received %d bytes after closing: %s\n", p->tot_len, modbus_client_pbuf_str(p)));
            pbuf_free(p);
        }
        return;
    }

    queue_entry_t *q = queue;

    if(p) {

        modbus_tcp_adu_t adu;

        if(p->tot_len >= offsetof(modbus_tcp_adu_t, pdu.data)) {

            memcpy(&adu, p->payload, offsetof(modbus_tcp_adu_t, pdu.data) + 1);
            adu.pdu.length = lwip_htons(adu.pdu.length);

            if(q) do {
                if(q->adu.tid == adu.tid) {

                    if(q->adu.pdu.code != adu.pdu.code) {
                        if(q->callbacks.on_rx_exception) {
                            adu.pdu.code = lwip_htons(adu.pdu.code);
                            q->callbacks.on_rx_exception((adu.pdu.code & 0x80) ? adu.pdu.data[0] : -1, q->context);
                        }
                    } else if(q->callbacks.on_rx_packet) {

                        modbus_message_t msg;

                        msg.context = q->context;
                        msg.rx_length = adu.pdu.length;
                        msg.adu[0] = adu.pdu.uid;
                        msg.adu[1] = adu.pdu.code;
                        memcpy(&msg.adu[2], &((uint8_t *)p->payload)[offsetof(modbus_tcp_adu_t, pdu.data)], adu.pdu.length - 2);

                        q->callbacks.on_rx_packet(&msg);
                    }

                    if(!q->sync)
                        unlink_msg(q);
                    else
                        q->sync = false;
                    q = NULL;
                }
            } while(q && (q = q->next));

//            altcp_recved(s->pcb, adu.pdu.length - 2 + offsetof(modbus_tcp_adu_t, pdu.data));
        }

        pbuf_free(p);

    } else if((q = queue)) {

        uint32_t ms = hal.get_elapsed_ticks();

        if(!s->tx_busy) do {
            if(s->settings->id == q->adu.pdu.uid) {
                if(q->timeout == 0) {
                    if(altcp_write(s->pcb, &q->adu, q->msg_length, TCP_WRITE_FLAG_COPY) == ERR_OK) {
                        q->timeout = ms;
                        s->tx_busy = true;
                        altcp_output(s->pcb);
                        break;
                    }
                } else if(ms - q->timeout >= 50) {
                    if(q->callbacks.on_rx_exception)
                        q->callbacks.on_rx_exception(0, q->context);
                    q = unlink_msg(q);
                }
            }
        } while(q && (q = q->next));
    }
}

static void modbus_tcp_flush_queue (void)
{
    while(spin_lock);

    queue_entry_t *q = queue, *qn = NULL;

    if(q) do {
        qn = q->next;
        free(q);
    } while((q = qn));

    queue = NULL;
}

static void modbus_tcp_close (modbus_session_t *s, struct altcp_pcb *pcb, u8_t result, u16_t srv_err, err_t err)
{
    if(pcb != NULL) {
        altcp_arg(pcb, NULL);
        if(altcp_close(pcb) == ERR_OK) {
            s->connected = false;
            if(s != NULL) {
            //    modbus_client_free(s, result, srv_err, err);
            }
        } else {
            /* close failed, set back arg */
            altcp_arg(pcb, s);
        }
    } else {
        s->connected = false;
        if(s != NULL) {
    //        modbus_client_free(s, result, srv_err, err);
        }
    }
}

static void modbus_tcp_err (void *arg, err_t err)
{
    if(arg) {
        ((modbus_session_t *)arg)->connected = false;
        LWIP_DEBUGF(MODBUS_DEBUG_WARN_STATE, ("modbus_tcp_err: connection reset by remote host\n"));
    }
}

/*
static err_t modbus_tcp_poll (void *arg, struct altcp_pcb *pcb)
{

    modbus_process(arg, pcb, NULL);

    return ERR_OK;
}
*/

static err_t modbus_tcp_sent (void *arg, struct altcp_pcb *pcb, u16_t len)
{
    LWIP_UNUSED_ARG(len);

    ((modbus_session_t *)arg)->tx_busy = false;

    modbus_process(arg, pcb, NULL);

    return ERR_OK;
}

static err_t modbus_tcp_recv (void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    LWIP_UNUSED_ARG(err);

    if(p != NULL) {
        altcp_recved(pcb, p->tot_len);
        modbus_process(arg, pcb, p);
    } else {
        LWIP_DEBUGF(MODBUS_DEBUG_WARN_STATE, ("modbus_tcp_recv: connection closed by remote host\n"));
        modbus_tcp_close((modbus_session_t *)arg, pcb, 1, 0, err);
    }

    return ERR_OK;
}

static err_t modbus_tcp_connected (void *arg, struct altcp_pcb *pcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if((((modbus_session_t *)arg)->connected = err == ERR_OK)) {
        LWIP_DEBUGF(MODBUS_DEBUG_STATE, ("modbus_client_connected"));

    } else {
        /* shouldn't happen, but we still check 'err', only to be sure */
        LWIP_DEBUGF(MODBUS_DEBUG_WARN, ("modbus_client_connected: %d\n", (int)err));
        modbus_tcp_close((modbus_session_t *)arg, pcb, 0, 0, err);
    }

    return ERR_OK;
}

static err_t modbus_client_connect (modbus_session_t *session)
{
    err_t err = ERR_MEM;

#if LWIP_ALTCP && LWIP_ALTCP_TLS
    if (modbus_client_tls_config)
        session->pcb = altcp_tls_new(modbus_client_server_tls_config, IP_GET_TYPE(remote_ip));
    else
#endif

    session->pcb = altcp_tcp_new_ip_type(IP_GET_TYPE(session->settings->ip));

    if(session->pcb != NULL) {

        altcp_arg(session->pcb, session);
        altcp_recv(session->pcb, modbus_tcp_recv);
        altcp_err(session->pcb, modbus_tcp_err);
//        altcp_poll(session->pcb, modbus_tcp_poll, MODBUS_CLIENT_POLL_INTERVAL);
        altcp_sent(session->pcb, modbus_tcp_sent);

        if((err = altcp_connect(session->pcb, (ip_addr_t *)&session->settings->ip, session->settings->port, modbus_tcp_connected)) != ERR_OK) {
            altcp_arg(session->pcb, NULL);
            altcp_close(session->pcb);
        }
    }

    return err;
}

// Settings handling

static inline void set_addr (char *ip, ip4_addr_t *addr)
{
    memcpy(ip, addr, sizeof(ip4_addr_t));
}

static modbus_tcp_setting_id_t normalize_id (setting_id_t setting, uint_fast16_t *idx)
{
    uint_fast16_t base_idx = (uint_fast16_t)setting - (uint_fast16_t)Setting_ModbusTCPBase;
    uint_fast8_t setting_idx = base_idx % MODBUS_TCP_SETTINGS_INCREMENT;
    *idx = (base_idx - setting_idx) / MODBUS_TCP_SETTINGS_INCREMENT;

    return (modbus_tcp_setting_id_t)setting_idx;
}

static status_code_t modbus_set_ip (setting_id_t setting, char *value)
{
    ip_addr_t addr;
    uint_fast16_t idx;

    normalize_id(setting, &idx);

    if(ip4addr_aton(value, &addr) != 1)
        return Status_InvalidStatement;

    status_code_t status = idx < MODBUS_N_CLIENTS ? Status_OK : Status_SettingDisabled;

    if(status == Status_OK)
        set_addr(modbus[idx].ip, &addr);

    return status;
}

static char *modbus_get_ip (setting_id_t setting)
{
    static char ip[IPADDR_STRLEN_MAX];

    uint_fast16_t idx;

    normalize_id(setting, &idx);

    if(idx < MODBUS_N_CLIENTS)
        ip4addr_ntoa_r((const ip_addr_t *)&modbus[idx].ip, ip, IPADDR_STRLEN_MAX);
    else
        *ip = '\0';

    return ip;
}

static status_code_t modbus_set_setting (setting_id_t setting , uint_fast16_t value)
{
    uint_fast16_t idx;
    status_code_t status = Status_OK;

    setting = normalize_id(setting, &idx);

    if(idx < MODBUS_N_CLIENTS) switch((modbus_tcp_setting_id_t)setting) {

        case Setting_ModbusPort:
            modbus[idx].port = value;
            break;

        case Setting_ModbusId:
            modbus[idx].id = value;
            break;

        default:
            status = Status_Unhandled;
            break;
    }

    return status;
}

static uint_fast16_t modbus_get_setting (setting_id_t setting)
{
    uint_fast16_t idx, value = 0;

    setting = normalize_id(setting, &idx);

    if(idx < MODBUS_N_CLIENTS) switch((modbus_tcp_setting_id_t)setting) {

        case Setting_ModbusPort:
            value = modbus[idx].port;
            break;

        case Setting_ModbusId:
            value = modbus[idx].id;
            break;

        default:
            break;
    }

    return value;
}

#define MSET_OPTS { .reboot_required = On, .subgroups = On, .increment = MODBUS_TCP_SETTINGS_INCREMENT }

static bool modbus_group_available (const setting_group_detail_t *group)
{
    return group->id < Group_ModBusUnit0 + MODBUS_N_CLIENTS;
}

static const setting_group_detail_t modbus_groups [] = {
    { Group_Root, Group_ModBus, "ModBus"},
    { Group_ModBus, Group_ModBusUnit0, "ModBus TCP, unit 1", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit1, "ModBus TCP, unit 2", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit2, "ModBus TCP, unit 3", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit3, "ModBus TCP, unit 4", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit4, "ModBus TCP, unit 5", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit5, "ModBus TCP, unit 6", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit6, "ModBus TCP, unit 7", modbus_group_available},
    { Group_ModBus, Group_ModBusUnit7, "ModBus TCP, unit 8", modbus_group_available}
};

static const setting_detail_t modbus_settings[] = {
    { Setting_ModbusIpAddressBase, Group_ModBusUnit0, "Unit ? IP address", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, modbus_set_ip, modbus_get_ip, NULL, MSET_OPTS },
    { Setting_ModbusPortBase, Group_ModBusUnit0, "Unit ? port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCoreFn, modbus_set_setting, modbus_get_setting, NULL, MSET_OPTS },
    { Setting_ModbusIdBase, Group_ModBusUnit0, "Unit ? ID", NULL, Format_Int16, "##0", "0", "255", Setting_NonCoreFn, modbus_set_setting, modbus_get_setting, NULL, MSET_OPTS }
};

#ifndef NO_SETTINGS_DESCRIPTIONS

static const setting_descr_t modbus_settings_descr[] = {
    { Setting_ModbusIpAddressBase, "IP address of unit." },
    { Setting_ModbusPortBase, "Port number of unit, 502 is the standard ModBus port." },
    { Setting_ModbusIdBase, "ModBus id of unit, set to to 0 to disable communication." },
};

#endif

static void modbus_settings_save (void)
{
    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&modbus, sizeof(modbus_tcp_settings_t) * MODBUS_N_CLIENTS, true);
}

static void modbus_settings_restore (void)
{
    uint_fast8_t idx = MODBUS_N_CLIENTS;

    do {
        modbus[--idx].id = 0;
        modbus[idx].port = NETWORK_MODBUS_PORT;
        modbus_set_ip(Setting_ModbusIpAddressBase + idx * MODBUS_TCP_SETTINGS_INCREMENT, "0.0.0.0");
    } while(idx);

    modbus_settings_save();
}

static void modbus_settings_load (void)
{
    if(hal.nvs.memcpy_from_nvs((uint8_t *)&modbus, nvs_address, sizeof(modbus_tcp_settings_t) * MODBUS_N_CLIENTS, true) != NVS_TransferResult_OK)
        modbus_settings_restore();
}

bool modbus_settings_iterator (const setting_detail_t *setting, setting_output_ptr callback, void *data)
{
    uint_fast16_t idx, instance;

    normalize_id(setting->id, &instance);

    for(idx = 0; idx < MODBUS_N_CLIENTS; idx++)
        callback(setting, idx * MODBUS_TCP_SETTINGS_INCREMENT + instance, data);

    return true;
}

static setting_details_t setting_details = {
    .groups = modbus_groups,
    .n_groups = sizeof(modbus_groups) / sizeof(setting_group_detail_t),
    .settings = modbus_settings,
    .n_settings = sizeof(modbus_settings) / sizeof(setting_detail_t),
#ifndef NO_SETTINGS_DESCRIPTIONS
    .descriptions = modbus_settings_descr,
    .n_descriptions = sizeof(modbus_settings_descr) / sizeof(setting_descr_t),
#endif
    .save = modbus_settings_save,
    .load = modbus_settings_load,
    .restore = modbus_settings_restore,
    .iterator = modbus_settings_iterator
};

static void pos_failed (uint_fast16_t state)
{
    report_message("Modbus TCP failed to initialize!", Message_Warning);
}

static bool modbus_tcp_isup (void)
{
    return modbus[0].id != 0; // TODO: how to handle this? Only check link is up? If any clients are defined? ...
}

// Public API

void modbus_tcp_client_poll (void)
{
    uint_fast8_t idx = MODBUS_N_CLIENTS;

    do {
        if(session[--idx].connected)
            modbus_process(&session[idx], session[idx].pcb, NULL);
    } while(idx);
}

void modbus_tcp_client_start (void)
{
    uint_fast8_t idx = MODBUS_N_CLIENTS;

    do {
        idx--;
        if(modbus[idx].id && modbus[idx].port) {
            session[idx].settings = &modbus[idx];
            modbus_client_connect(&session[idx]);
        }
    } while(idx);
}

void modbus_tcp_client_init (void)
{
    const modbus_api_t api = {
        .interface = Modbus_InterfaceTCP,
        .is_up = modbus_tcp_isup,
        .flush_queue = modbus_tcp_flush_queue,
        .send = modbus_rtu_send
    };

    if((nvs_address = nvs_alloc(sizeof(modbus_tcp_settings_t) * MODBUS_N_CLIENTS))) {

        driver_reset = hal.driver_reset;
        hal.driver_reset = modbus_reset;

        modbus_register_api(&api);
        settings_register(&setting_details);

    } else {
        protocol_enqueue_rt_command(pos_failed);
        system_raise_alarm(Alarm_SelftestFailed);
    }
}

#endif
