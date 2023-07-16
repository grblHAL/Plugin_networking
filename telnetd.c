//
// telnetd.c - lwIP "raw" telnet daemon
//
// v2.3 / 2023-07-09 / Io Engineering / Terje
//

/*

Copyright (c) 2018-2023, Terje Io
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

#if TELNET_ENABLE

#include <assert.h>
#include <string.h>

#include "telnetd.h"
#include "networking.h"
#include "grbl/protocol.h"

#ifndef TELNETD_TCP_PRIO
#define TELNETD_TCP_PRIO TCP_PRIO_MIN
#endif

#ifndef TELNETD_POLL_INTERVAL
#define TELNETD_POLL_INTERVAL 2
#endif

typedef struct {
    struct pbuf *p;
    struct pbuf *q;
    uint_fast16_t len;
    void *payload;
} packet_chain_t;

typedef struct
{
    const io_stream_t *stream;
    uint32_t timeout;
    uint32_t timeoutMax;
    struct tcp_pcb *pcb;
    packet_chain_t packet;
    stream_rx_buffer_t rxbuf;
    stream_tx_buffer_t txbuf;
    TickType_t lastSendTime;
    err_t lastErr;
    uint8_t errorCount;
} sessiondata_t;

static const sessiondata_t defaultSettings =
{
    .timeout = 0,
    .timeoutMax = SOCKET_TIMEOUT,
    .pcb = NULL,
    .packet = {0},
    .rxbuf = {0},
    .txbuf = {0},
    .lastSendTime = 0,
    .errorCount = 0,
    .lastErr = ERR_OK
};

static tcp_server_t telnet_server;
static sessiondata_t streamSession;
static enqueue_realtime_command_ptr enqueue_realtime_command = protocol_enqueue_realtime_command;
#if ESP_PLATFORM
static portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

static void telnet_stream_handler (sessiondata_t *session);

//
// streamGetC - returns -1 if no data available
//
static int16_t streamGetC (void)
{
    int16_t data;

    if(streamSession.rxbuf.tail == streamSession.rxbuf.head)
        return SERIAL_NO_DATA; // no data available else EOF

    data = streamSession.rxbuf.data[streamSession.rxbuf.tail];                          // Get next character
    streamSession.rxbuf.tail = BUFNEXT(streamSession.rxbuf.tail, streamSession.rxbuf);  // and update pointer

    return data;
}

static inline uint16_t streamRxCount (void)
{
    uint_fast16_t head = streamSession.rxbuf.head, tail = streamSession.rxbuf.tail;

    return BUFCOUNT(head, tail, RX_BUFFER_SIZE);
}

static uint16_t streamRxFree (void)
{
    return (RX_BUFFER_SIZE - 1) - streamRxCount();
}

static void streamRxFlush (void)
{
    streamSession.rxbuf.tail = streamSession.rxbuf.head;
}

static void streamRxCancel (void)
{
    streamSession.rxbuf.data[streamSession.rxbuf.head] = ASCII_CAN;
    streamSession.rxbuf.tail = streamSession.rxbuf.head;
    streamSession.rxbuf.head = BUFNEXT(streamSession.rxbuf.head, streamSession.rxbuf);
}

static bool streamSuspendInput (bool suspend)
{
    return stream_rx_suspend(&streamSession.rxbuf, suspend);
}

static bool streamRxPutC (char c)
{
    bool mpg, overflow = false;

    // discard input if MPG has taken over...
    if(!(mpg = hal.stream.type == StreamType_MPG)) {
#if ESP_PLATFORM
        taskENTER_CRITICAL(&rx_mux);
#else
        taskENTER_CRITICAL();
#endif
        if(!enqueue_realtime_command(c)) {                              // If not a real time command attempt to buffer it
            uint_fast16_t next_head = BUFNEXT(streamSession.rxbuf.head, streamSession.rxbuf);
            if((overflow = next_head == streamSession.rxbuf.tail))      // If buffer full
                streamSession.rxbuf.overflow = true;                    // flag overflow
            else {
                streamSession.rxbuf.data[streamSession.rxbuf.head] = c; // Add data to buffer and
                streamSession.rxbuf.head = next_head;                   // update pointer
            }
        }
#if ESP_PLATFORM
        taskEXIT_CRITICAL(&rx_mux);
#else
        taskEXIT_CRITICAL();
#endif
    }

    return mpg || !overflow;
}

static bool streamPutC (const char c)
{
    uint_fast16_t next_head = BUFNEXT(streamSession.txbuf.head, streamSession.txbuf);

    while(streamSession.txbuf.tail == next_head) {  // Buffer full, block until space is available...
        if(!hal.stream_blocking_callback())
            return false;
    }

    streamSession.txbuf.data[streamSession.txbuf.head] = c; // Add data to buffer
    streamSession.txbuf.head = next_head;                   // and update head pointer

    return true;
}

static void streamWriteS (const char *data)
{
    char c, *ptr = (char *)data;

    while((c = *ptr++) != '\0')
        streamPutC(c);
}

static void streamWrite (const char *data, uint16_t length)
{
    char *ptr = (char *)data;

    while(length--)
        streamPutC(*ptr++);
}

static uint16_t streamTxCount (void) {

    uint_fast16_t head = streamSession.txbuf.head, tail = streamSession.txbuf.tail;

    return BUFCOUNT(head, tail, TX_BUFFER_SIZE);
}

static int16_t streamTxGetC (void)
{
    int16_t data;

    if(streamSession.txbuf.tail == streamSession.txbuf.head)
        return SERIAL_NO_DATA; // no data available else EOF

    data = streamSession.txbuf.data[streamSession.txbuf.tail];                          // Get next character
    streamSession.txbuf.tail = BUFNEXT(streamSession.txbuf.tail, streamSession.txbuf);  // and update pointer

    return data;
}

/*
static void streamTxFlush (void)
{
    streamSession.txbuf.tail = streamSession.txbuf.head;
}
*/

static bool streamEnqueueRtCommand (char c)
{
    return enqueue_realtime_command(c);
}

static enqueue_realtime_command_ptr streamSetRtHandler (enqueue_realtime_command_ptr handler)
{
    enqueue_realtime_command_ptr prev = enqueue_realtime_command;

    if(handler)
        enqueue_realtime_command = handler;

    return prev;
}

static void streamClose (sessiondata_t *session)
{
    // Switch I/O stream back to default
    if(session->stream) {
        stream_disconnect(session->stream);
        session->stream = NULL;
    }
}

//
// TCP handlers
//

static void telnet_state_free (sessiondata_t *session)
{
    SYS_ARCH_DECL_PROTECT(lev);
    SYS_ARCH_PROTECT(lev);

    if(session->packet.p) {
        pbuf_free(session->packet.p);
        session->packet.p = NULL;
    }

    SYS_ARCH_UNPROTECT(lev);
}

static void telnet_err (void *arg, err_t err)
{
    sessiondata_t *session = arg;

    telnet_state_free(session);

    telnet_server.link_lost = false;

    session->errorCount++;
    session->lastErr = err;
    session->pcb = NULL;
    session->timeout = 0;
    session->lastSendTime = 0;

    streamClose(session);
}

static err_t telnet_poll (void *arg, struct tcp_pcb *pcb)
{
    sessiondata_t *session = arg;

    if(!session)
        tcp_close(pcb);
    else {
        session->timeout++;
        if(session->timeoutMax && session->timeout > session->timeoutMax)
            tcp_abort(pcb);
    }

    return ERR_OK;
}

static void telnet_close_conn (sessiondata_t *session, struct tcp_pcb *pcb)
{
    telnet_state_free(session);

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 1);

    if (tcp_close(pcb) != ERR_OK)
        tcp_poll(pcb, telnet_poll, TELNETD_POLL_INTERVAL);

    session->pcb = NULL;

    // Switch I/O stream back to default
    streamClose(session);
}

//
// Queue incoming packet for processing
//
static err_t telnet_recv (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    sessiondata_t *session = arg;

    if(err != ERR_OK || p == NULL || session == NULL) {

        if (p != NULL) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }

        telnet_close_conn(session, pcb);

    } else if(session->packet.p == NULL) {

        struct pbuf *q = p;
        uint8_t *payload = q->payload;
        uint_fast16_t len = q->len, taken = 0;

        while(q) {

            if(!streamRxPutC(*payload++)) {
                payload--;
                break;
            }

            len--;
            taken++;
            if(len == 0 && (q = q->next)) {
                len = q->len;
                payload = q->payload;
            }
        }

        if(taken)
            tcp_recved(session->pcb, taken);

        if(q == NULL) {
            pbuf_free(p);
            session->packet.p = NULL;
            telnet_stream_handler(session);
        } else {
            session->packet.p = p;
            session->packet.q = q;
            session->packet.len = q->len - (payload - (uint8_t *)q->payload);
            session->packet.payload = payload;
        }
    }

    return ERR_OK;
}

static err_t telnet_sent (void *arg, struct tcp_pcb *pcb, u16_t ui16len)
{
    sessiondata_t *session = arg;

    session->timeout = 0;

    telnet_stream_handler(session);

    return ERR_OK;
}

static bool is_connected (void)
{
    return true;
}

static err_t telnet_accept (void *arg, struct tcp_pcb *pcb, err_t err)
{
    static const io_stream_t telnet_stream = {
        .type = StreamType_Telnet,
        .is_connected = is_connected,
        .read = streamGetC,
        .write = streamWriteS,
        .write_n = streamWrite,
        .write_char = streamPutC,
        .enqueue_rt_command = streamEnqueueRtCommand,
        .get_rx_buffer_free = streamRxFree,
        .reset_read_buffer = streamRxFlush,
        .cancel_read_buffer = streamRxCancel,
        .suspend_read = streamSuspendInput,
        .set_enqueue_rt_handler = streamSetRtHandler
    };

    if ((err != ERR_OK) || (pcb == NULL))
        return ERR_VAL;

    sessiondata_t *session = arg;

    if(session->pcb)
        return ERR_CONN; // Busy, refuse connection

    if(telnet_server.link_lost) {
        // Link was previously lost, abort current connection

        tcp_abort(session->pcb);

        telnet_state_free(session);

        telnet_server.link_lost = false;

        return ERR_ABRT;
    }

    streamClose(session);

    memcpy(session, &defaultSettings, sizeof(sessiondata_t));

    session->pcb = pcb;

    tcp_accepted(pcb);
    tcp_setprio(pcb, TELNETD_TCP_PRIO);
    tcp_recv(pcb, telnet_recv);
    tcp_err(pcb, telnet_err);
    tcp_poll(pcb, telnet_poll, TELNETD_POLL_INTERVAL);
    tcp_sent(pcb, telnet_sent);
    tcp_arg(pcb, &streamSession);

    // Switch I/O stream to Telnet connection
    if(stream_connect(&telnet_stream))
        session->stream = &telnet_stream;
    // else abort connection?

    return ERR_OK;
}

void telnet_stream_handler (sessiondata_t *session)
{
    static uint_fast16_t tx_len = 0;
    static uint8_t txbuf[TX_BUFFER_SIZE];

    uint_fast16_t len;

    if(session->pcb == NULL)
        return;

    // 1. Process pending input packet

    if(session->packet.p) {

        struct pbuf *q = session->packet.q;
        uint8_t *payload = session->packet.payload;
        uint_fast16_t taken = 0;

        len = session->packet.len;

        while(q) {

            if(!streamRxPutC(*payload++)) {
                payload--;
                break;
            }

            len--;
            taken++;
            if(len == 0 && (q = q->next)) {
                len = q->len;
                payload = q->payload;
            }
        }

        if(taken)
            tcp_recved(session->pcb, taken);

        if(q == NULL) {
            pbuf_free(session->packet.p);
            session->packet.p = NULL;
        } else {
            session->packet.q = q;
            session->packet.len = q->len - (payload - (uint8_t *)q->payload);
            session->packet.payload = payload;
        }
    }

    // 2. Process output stream

    if(tx_len == 0 && (len = streamTxCount())) {

        int16_t c;

        while(len) {
            if((c = (uint8_t)streamTxGetC()) == SERIAL_NO_DATA)
                break;
            txbuf[tx_len++] = (uint8_t)c;
            len--;
        }
    }

    if(tx_len) {

        err_t err;

        len = tx_len;

        do {
            if((err = tcp_write(session->pcb, txbuf, (u16_t)len, TCP_WRITE_FLAG_COPY)) == ERR_MEM)
                len = tcp_sndqueuelen(session->pcb) >= TCP_SND_QUEUELEN ? 1 : len / 2;
        } while(err == ERR_MEM && len > 1);

        if(err == ERR_OK) {
            if(tx_len != len)
                memmove(txbuf, &txbuf[len + 1], tx_len - len);
            tx_len -= len;
            tcp_output(session->pcb);
            session->lastSendTime = xTaskGetTickCount();
        }
    }
}

void telnetd_poll (void)
{
    telnet_stream_handler(&streamSession);
}

void telnetd_notify_link_status (bool up)
{
    if(!up)
        telnet_server.link_lost = true;
}


void telnetd_close_connections (void)
{
    streamClose(&streamSession);
}

void telnetd_stop (void)
{
    if(telnet_server.pcb != NULL) {

        if(streamSession.pcb != NULL) {

            tcp_arg(streamSession.pcb, NULL);
            tcp_recv(streamSession.pcb, NULL);
            tcp_sent(streamSession.pcb, NULL);
            tcp_err(streamSession.pcb, NULL);
            tcp_poll(streamSession.pcb, NULL, 1);

            tcp_abort(streamSession.pcb);

            // Switch grbl I/O stream back to default
            streamClose(&streamSession);
        }

        telnet_state_free(&streamSession);

        tcp_close(telnet_server.pcb);

        telnet_server.pcb = NULL;
    }
}

bool telnetd_init (uint16_t port)
{
    err_t err;

    telnet_server.port = port;

    struct tcp_pcb *pcb = tcp_new();

    if((err = tcp_bind(pcb, IP_ADDR_ANY, port)) == ERR_OK) {

        telnet_server.pcb = tcp_listen(pcb);

        tcp_arg(telnet_server.pcb, &streamSession);
        tcp_accept(telnet_server.pcb, telnet_accept);
    }

    return err == ERR_OK;
}

#endif
