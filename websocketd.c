//
// websocketd.c - lwIP websocket daemon implementation
//
// v2.7 / 2023-07-09 / Io Engineering / Terje
//

/*

Copyright (c) 2019-2023, Terje Io
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

#if WEBSOCKET_ENABLE

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "networking.h"
#include "base64.h"
#include "sha1.h"
#include "utils.h"
#include "strutils.h"
#include "websocketd.h"

#include "grbl/grbl.h"
#include "grbl/protocol.h"

//#define WSDEBUG

#define CRLF "\r\n"
#define SOCKET_TIMEOUT 0
#define MAX_HTTP_HEADER_SIZE 512
#define FRAME_NONE 0xFF

#ifndef WEBSOCKETD_TCP_PRIO
#define WEBSOCKETD_TCP_PRIO TCP_PRIO_MIN
#endif

#ifndef WEBSOCKETD_POLL_INTERVAL
#define WEBSOCKETD_POLL_INTERVAL 2
#endif

#ifndef WEBUI_MAX_CLIENTS
#define WEBUI_MAX_CLIENTS 4
#endif

#define WEBSOCKETD_MAGIC 1819047252

PROGMEM static const char WS_GUID[]  = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
PROGMEM static const char WS_KEY[]   = "Sec-WebSocket-Key: ";
PROGMEM static const char WS_PROT[]  = "Sec-WebSocket-Protocol: ";
PROGMEM static const char WS_RSP[]   = "HTTP/1.1 101 Switching Protocols" CRLF \
                                       "Upgrade: websocket" CRLF \
                                       "Connection: Upgrade" CRLF \
                                       "Sec-WebSocket-Accept: ";
PROGMEM static const char HTTP_400[] = "HTTP/1.1 400" CRLF \
                                       "Status: 400 Bad Request" CRLF CRLF;
PROGMEM static const char HTTP_500[] = "HTTP/1.1 500" CRLF \
                                       "Status: 500 Internal Server Error" CRLF CRLF;

typedef enum {
    WsOpcode_Continuation = 0x00,
    WsOpcode_Text = 0x1,
    WsOpcode_Binary = 0x2,
    WsOpcode_Close = 0x8,
    WsOpcode_Ping = 0x9,
    WsOpcode_Pong = 0xA
} websocket_opcode_t;

typedef enum
{
    WsState_Free,
    WsState_Idle,
    WsState_Connecting,
    WsState_Connected,
    WsState_Closing
} websocket_state_t;

typedef union {
    uint8_t token;
    struct {
        uint8_t opcode :4,
                rsv3   :1,
                rsv2   :1,
                rsv1   :1,
                fin    :1;
    };
} ws_frame_start_t;

typedef struct {
    uint32_t idx;
    uint32_t payload_len;
    uint32_t payload_rem;
    uint32_t rx_index;
    uint8_t *frame;
    uint32_t mask;
    bool masked;
    bool complete;
    uint8_t data[13];
} frame_header_t;

typedef struct {
    struct pbuf *p;
    struct pbuf *q;
    uint_fast16_t len;
    void *payload;
} packet_chain_t;

typedef struct ws_sessiondata
{
    uint32_t magic;
    const io_stream_t *stream;
    io_stream_state_t stream_state;
    websocket_state_t state;
    ws_frame_start_t ftype;
    websocket_opcode_t fragment_opcode;
    ws_frame_start_t start;
    frame_header_t header;
    uint32_t timeout;
    uint32_t timeoutMax;
    struct tcp_pcb *pcb;
    packet_chain_t packet;
    TickType_t lastSendTime;
    err_t lastErr;
    uint8_t errorCount;
    uint8_t pingCount;
    char *http_request;
    uint8_t *payload;
    bool collect_payload;
    uint32_t hdrsize;
    websocket_on_frame_received_ptr on_txt_frame_received;
    websocket_on_frame_received_ptr on_bin_frame_received;
} ws_sessiondata_t;

typedef struct {
    ws_sessiondata_t *session;
    stream_rx_buffer_t rxbuf;
    stream_tx_buffer_t txbuf;
} ws_streambuffers_t;

static void websocket_stream_handler (ws_sessiondata_t *session);

static const ws_frame_start_t wshdr_txt = {
  .fin    = true,
  .opcode = WsOpcode_Text
};

static const ws_frame_start_t wshdr_bin = {
  .fin    = true,
  .opcode = WsOpcode_Binary
};

static const ws_frame_start_t wshdr_ping = {
  .fin    = true,
  .opcode = WsOpcode_Ping
};

static const ws_sessiondata_t defaultSettings =
{
    .magic = WEBSOCKETD_MAGIC,
    .stream = NULL,
    .stream_state.connected = true,
    .state = WsState_Free,
    .fragment_opcode = WsOpcode_Continuation,
    .start.token = FRAME_NONE,
//    .ftype = wshdr_txt,
    .timeout = 0,
    .timeoutMax = SOCKET_TIMEOUT,
    .pcb = NULL,
    .packet = {0},
    .header = {0},
    .lastSendTime = 0,
    .errorCount = 0,
    .pingCount = 0,
    .lastErr = ERR_OK,
    .http_request = NULL,
    .hdrsize = MAX_HTTP_HEADER_SIZE,
    .payload = NULL,
    .collect_payload = false,
    .on_txt_frame_received = NULL,
    .on_bin_frame_received = NULL
};

static const io_stream_t *claim_stream (uint32_t baud_rate);

static tcp_server_t ws_server;
static ws_sessiondata_t clients[WEBUI_MAX_CLIENTS] = {0};
static ws_streambuffers_t streambuffers = {0};
static io_stream_properties_t ws_streams[] = {
    {
      .type = StreamType_WebSocket,
      .instance = 10,
      .flags.claimable = On,
      .flags.claimed = Off,
      .flags.connected = Off,
      .flags.can_set_baud = Off,
      .flags.modbus_ready = Off,
      .claim = claim_stream
    }
};
static enqueue_realtime_command_ptr enqueue_realtime_command = protocol_enqueue_realtime_command;
#if ESP_PLATFORM
static portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

websocket_events_t websocket;


//
// streamGetC - returns -1 if no data available
//
static int16_t streamGetC (void)
{
    int16_t data;

    if(streambuffers.rxbuf.tail == streambuffers.rxbuf.head)
        return SERIAL_NO_DATA; // no data available else EOF

    data = streambuffers.rxbuf.data[streambuffers.rxbuf.tail];                          // Get next character
    streambuffers.rxbuf.tail = BUFNEXT(streambuffers.rxbuf.tail, streambuffers.rxbuf);  // and update pointer

    return data;
}

static inline uint16_t streamRxCount (void)
{
    uint_fast16_t head = streambuffers.rxbuf.head, tail = streambuffers.rxbuf.tail;

    return BUFCOUNT(head, tail, RX_BUFFER_SIZE);
}

static uint16_t streamRxFree (void)
{
    return (RX_BUFFER_SIZE - 1) - streamRxCount();
}

static void streamRxFlush (void)
{
    streambuffers.rxbuf.tail = streambuffers.rxbuf.head;
}

static void websocketd_RxCancel (void)
{
    streambuffers.rxbuf.data[streambuffers.rxbuf.head] = ASCII_CAN;
    streambuffers.rxbuf.tail = streambuffers.rxbuf.head;
    streambuffers.rxbuf.head = BUFNEXT(streambuffers.rxbuf.head, streambuffers.rxbuf);
}

static bool streamSuspendInput (bool suspend)
{
    return stream_rx_suspend(&streambuffers.rxbuf, suspend);
}

bool websocketd_RxPutC (char c)
{
    bool ok, overflow = false;

    // discard input if MPG has taken over...
    if((ok = streambuffers.session && streambuffers.session->state == WsState_Connected && hal.stream.type != StreamType_MPG)) {
#if ESP_PLATFORM
        taskENTER_CRITICAL(&rx_mux);
#else
        taskENTER_CRITICAL();
#endif
        if(!enqueue_realtime_command(c)) {                          // If not a real time command attempt to buffer it
            uint_fast16_t next_head = BUFNEXT(streambuffers.rxbuf.head, streambuffers.rxbuf);
            if((overflow = next_head == streambuffers.rxbuf.tail))  // If buffer full
                streambuffers.rxbuf.overflow = true;                // flag overflow
            streambuffers.rxbuf.data[streambuffers.rxbuf.head] = c; // add data to buffer
            streambuffers.rxbuf.head = next_head;                   // and update pointer
        }
#if ESP_PLATFORM
        taskEXIT_CRITICAL(&rx_mux);
#else
        taskEXIT_CRITICAL();
#endif
    }

    return ok && !overflow;
}

static bool streamPutC (const char c)
{
    uint_fast16_t next_head = BUFNEXT(streambuffers.txbuf.head, streambuffers.txbuf);

    while(streambuffers.txbuf.tail == next_head) {                               // Buffer full, block until space is available...
        if(!hal.stream_blocking_callback())
            return false;
    }

    streambuffers.txbuf.data[streambuffers.txbuf.head] = c;                     // Add data to buffer
    streambuffers.txbuf.head = next_head;                                       // and update head pointer

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

    uint_fast16_t head = streambuffers.txbuf.head, tail = streambuffers.txbuf.tail;

    return BUFCOUNT(head, tail, TX_BUFFER_SIZE);
}

static int16_t streamTxGetC (void)
{
    int16_t data;

    if(streambuffers.txbuf.tail == streambuffers.txbuf.head)
        return SERIAL_NO_DATA; // no data available else EOF

    data = streambuffers.txbuf.data[streambuffers.txbuf.tail];                          // Get next character
    streambuffers.txbuf.tail = BUFNEXT(streambuffers.txbuf.tail, streambuffers.txbuf);  // and update pointer

    return data;
}

static void streamTxFlush (void)
{
    streambuffers.txbuf.tail = streambuffers.txbuf.head;
}

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

static void streamClose (ws_sessiondata_t *session)
{
    // Switch I/O stream back to default
    if(session->stream) {
        stream_disconnect(session->stream);
        session->stream = NULL;
        streambuffers.session = NULL;
        ws_streams[0].flags.connected = false;
        streamRxFlush();
        streamTxFlush();
    }
}

bool websocket_register_frame_handler (websocket_t *session, websocket_on_frame_received_ptr handler, bool binary)
{
    bool ok;

    if((ok = session && ((ws_sessiondata_t *)session)->magic == WEBSOCKETD_MAGIC)) {
        if(binary)
            ((ws_sessiondata_t *)session)->on_bin_frame_received = handler;
        else
            ((ws_sessiondata_t *)session)->on_txt_frame_received = handler;
    }

    return ok;
}

static bool is_connected (void)
{
    return ws_streams[0].flags.connected;
}

static const io_stream_t *claim_stream (uint32_t baud_rate)
{
    static const io_stream_t stream = {
        .type = StreamType_WebSocket,
        .is_connected = is_connected,
        .read = streamGetC,
        .write = streamWriteS,
        .write_n = streamWrite,
        .write_char = streamPutC,
        .enqueue_rt_command = streamEnqueueRtCommand,
        .get_rx_buffer_free = streamRxFree,
        .reset_write_buffer = streamTxFlush,
        .reset_read_buffer = streamRxFlush,
        .cancel_read_buffer = websocketd_RxCancel,
        .suspend_read = streamSuspendInput,
        .set_enqueue_rt_handler = streamSetRtHandler
    };

    if(ws_streams[0].flags.claimed)
        return NULL;

    if(baud_rate != 0)
        ws_streams[0].flags.claimed = On;

    return &stream;
}

bool websocket_send_frame (websocket_t *session, const void *data, size_t size, bool is_binary)
{
    uint8_t *msg;
    size_t hdr_len = size >= 126 ? 4 : 2;

    if(session == NULL || ((ws_sessiondata_t *)session)->magic != WEBSOCKETD_MAGIC)
        return false;

    if((msg = malloc(size + hdr_len))) {

        memcpy(msg + hdr_len, data, size);

        msg[0] = is_binary ? wshdr_bin.token : wshdr_txt.token;
        msg[1] = size < 126 ? size : 126;
        if(size >= 126) {
            msg[2] = (size >> 8) & 0xFF;
            msg[3] = size & 0xFF;
        }

        if(tcp_write(((ws_sessiondata_t *)session)->pcb, msg, (u16_t)(size + hdr_len), TCP_WRITE_FLAG_COPY) == ERR_OK)
            tcp_output(((ws_sessiondata_t *)session)->pcb);

        ((ws_sessiondata_t *)session)->lastSendTime = xTaskGetTickCount();

        free(msg);
    }

    return msg != 0;
}

bool websocket_broadcast_frame (const void *data, size_t size, bool is_binary)
{
    uint_fast16_t idx = WEBUI_MAX_CLIENTS;

    do {
        if(clients[--idx].state == WsState_Connected)
            websocket_send_frame(&clients[idx], data, size, is_binary);
    } while(idx);

    return true;
}

bool websocket_set_stream_flags (websocket_t *session, io_stream_state_t stream_state)
{
    if(session == NULL || ((ws_sessiondata_t *)session)->magic != WEBSOCKETD_MAGIC)
        return false;

    ((ws_sessiondata_t *)session)->stream_state = stream_state;

    return true;
}

//
// TCP handlers
//

static void websocket_state_free (ws_sessiondata_t *session)
{
    session->magic = 0; // Invalidate session

    // Free any buffer chain currently beeing processed
    if(session->packet.p) {
        pbuf_free(session->packet.p);
        session->packet.p = NULL;
    }

    // Free any http request currently beeing processed
    if(session->http_request) {
        free(session->http_request);
        session->http_request = NULL;
        session->hdrsize = MAX_HTTP_HEADER_SIZE;
    }

    if(session->payload) {
        free(session->payload);
        session->payload = NULL;
        session->collect_payload = false;
    }

    if(session->header.frame) {
        free(session->header.frame);
        session->header.frame = NULL;
    }
}

static void websocket_unlink_session (ws_sessiondata_t *session)
{
    session->magic = 0;             // Invalidate session
    session->state = WsState_Free;

    websocket_state_free(session);

    streamClose(session);

    if(websocket.on_client_disconnect)
        websocket.on_client_disconnect(session);
}

static void websocket_err (void *arg, err_t err)
{
    ws_sessiondata_t *session = arg;

    session->pcb = NULL;
    websocket_unlink_session(session);
}

static err_t websocket_poll (void *arg, struct tcp_pcb *pcb)
{
    ws_sessiondata_t *session = arg;

    if(!session)
        tcp_close(pcb);
    else {
        session->timeout++;
        if(session->timeoutMax && session->timeout > session->timeoutMax)
            tcp_abort(pcb);
    }

    return ERR_OK;
}

static void websocket_close_conn (ws_sessiondata_t *session, struct tcp_pcb *pcb)
{
    session->pcb = NULL;
    websocket_unlink_session(session);

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);

    if (tcp_close(pcb) != ERR_OK)
        tcp_poll(pcb, websocket_poll, WEBSOCKETD_POLL_INTERVAL);
}

//
// Process data for streaming
//

static bool collect_msg_frame (frame_header_t *header, uint8_t *payload, uint32_t len)
{
    if(header->payload_rem > len && header->payload_rem == header->payload_len) {
        if((header->frame = malloc(header->payload_len + header->idx)))
            memcpy(header->frame, &header->data, header->idx);
    }

    header->payload_rem -= len;

    if(header->frame)
        memcpy(header->frame + header->idx + header->payload_len - header->payload_rem - 1, payload, len);

    return header->frame != NULL;
}

static uint32_t websocket_msg_parse (ws_sessiondata_t *session, uint8_t *payload, uint32_t len)
{
    bool frame_done = false;
    uint32_t plen = len;

    // Collect frame header
    while(!session->header.complete && plen) {

        session->header.data[session->header.idx++] = *payload++;

        if(session->header.idx == 2) {
            session->header.masked      = session->header.data[1] & 0x80; // always true from client
            session->header.payload_len = session->header.data[1] & 0x7F;
        }

        if(session->header.idx >= 6) {
            if((session->header.complete = (session->header.idx == (session->header.payload_len == 126 ? 8 : 6)))) {
                if(session->header.payload_len == 126) {
                    session->header.payload_len = (session->header.data[2] << 8) | session->header.data[3];
                    memcpy(&session->header.mask, &session->header.data[4], sizeof(uint32_t));
                } else
                    memcpy(&session->header.mask, &session->header.data[2], sizeof(uint32_t));

                session->header.payload_rem = session->header.payload_len;
            }
        }

        plen--;
    }

//    if(session->start.token != FRAME_NONE)
//        DEBUG_PRINT("\r\n!span\r\n");

    // Process frame
    if (session->header.complete && (plen || session->header.payload_rem == 0)) {

        bool is_binary = false;

        ws_frame_start_t fs = (ws_frame_start_t)session->header.data[0];

        if(!fs.fin && (websocket_opcode_t)fs.opcode != WsOpcode_Continuation)
            session->fragment_opcode = (websocket_opcode_t)fs.opcode;

        if((websocket_opcode_t)fs.opcode == WsOpcode_Continuation)
            fs.opcode = session->fragment_opcode;

        switch ((websocket_opcode_t)fs.opcode) {

            case WsOpcode_Continuation:
                // Something went wrong, exit fragment handling mode
                session->fragment_opcode = WsOpcode_Continuation;
                break;

            case WsOpcode_Binary:
                is_binary = true;
//              session->ftype = wshdr_bin; // Switch to binary responses if client talks binary to us
                //  No break
            case WsOpcode_Text:

                if(fs.fin)
                    session->fragment_opcode = WsOpcode_Continuation;

                if(session->header.payload_rem == session->header.payload_len && ((session->on_txt_frame_received && !is_binary) || (session->on_bin_frame_received && is_binary))) {
                    if((session->collect_payload = frame_done = plen >= session->header.payload_len))
                        session->payload = payload;
                    else
                        session->collect_payload = session->header.payload_len > 0 && (session->payload = malloc(session->header.payload_len + is_binary ? 0 : 1)) != NULL;
                        //TODO: handle malloc failure?
                }

                if(session->header.payload_rem) {

                    uint8_t *mask = (uint8_t *)&session->header.mask;
                    uint_fast16_t payload_len = session->header.payload_rem > plen ? plen : session->header.payload_rem;

                    session->start.token = session->header.payload_rem > plen ? fs.token : FRAME_NONE;
/*
                    if(session->start.token != FRAME_NONE)
                        DEBUG_PRINT("\r\n!span!\r\n");
                    if(session->rxbuf.overflow)
                        DEBUG_PRINT("\r\n!overflow\r\n");
                    DEBUG_PRINT("\r\nPLEN:");
                    DEBUG_PRINT(uitoa(session->header.payload_rem));
                    DEBUG_PRINT(" ");
                    DEBUG_PRINT(uitoa(payload_len));
                    DEBUG_PRINT("\r\n");
*/

                    if(session->collect_payload) { // collect the complete frame before processing

                        if(session->header.payload_rem && !frame_done) {

                            plen = 0;
                            memcpy(session->payload + (session->header.payload_len - session->header.payload_rem), payload, payload_len);

                            if((frame_done = (session->header.payload_rem = session->header.payload_rem - payload_len) == 0)) {
                                if(!is_binary)
                                    *(session->payload + session->header.payload_len) = '\0';
                            }
                        }

                        if(frame_done) {

                            uint_fast16_t i = 0, j;
                            uint8_t *pm = session->payload;
                            websocket_on_frame_received_ptr on_received = is_binary ? session->on_bin_frame_received : session->on_txt_frame_received;

                            // Unmask data
                            for(j = 0; j < session->header.payload_len; j++)
                                *pm++ ^= mask[i++ % 4];

                            on_received(session, session->payload, session->header.payload_len);

                            if(session->payload == payload) { // not malloc'ed?
                                plen = 0;
                                session->payload = NULL;
                            }
                        }
                    } else if(session == streambuffers.session && session->stream_state.connected) { // Unmask and push into RX buffer on the go

                        uint_fast16_t i = session->header.rx_index;

                        streambuffers.rxbuf.overflow = false;

                        while (payload_len--) {
                            if(!websocketd_RxPutC(*payload++ ^ mask[i % 4]))
                                break; // If overflow pend buffering rest of data until next polling
                            plen--;
                            i++;
                        }

                        session->header.rx_index = i;
                        frame_done = (session->header.payload_rem = session->header.payload_len - session->header.rx_index) == 0;

                    } else { // No client, sink payload
                        plen = 0;
                        frame_done = (session->header.payload_rem = session->header.payload_rem - payload_len) == 0;
                    }
                }
                break;

            case WsOpcode_Close:
                if((frame_done = plen >= session->header.payload_rem)) {
                    plen -= session->header.payload_rem;
                    if(collect_msg_frame(&session->header, payload, session->header.payload_rem))
                        payload = session->header.frame;
                    tcp_write(session->pcb, payload, session->header.payload_len, 1);
                    tcp_output(session->pcb);
                    session->state = WsState_Closing;
                } else {
                    collect_msg_frame(&session->header, payload, plen);
                    plen = 0;
                }
                break;

            case WsOpcode_Ping:
                if((frame_done = plen >= session->header.payload_rem)) {
                    if(session->state != WsState_Closing) {
                        plen -= session->header.payload_rem;
                        if(collect_msg_frame(&session->header, payload, session->header.payload_rem))
                            payload = session->header.frame;
                        fs.opcode = WsOpcode_Pong;
                        payload[0] = fs.token;
                        tcp_write(session->pcb, payload, session->header.payload_len, 1);
                        tcp_output(session->pcb);
                    }
                } else {
                    collect_msg_frame(&session->header, payload, plen);
                    plen = 0;
                }
                break;

            case WsOpcode_Pong:
                if((frame_done = plen >= session->header.payload_rem)) {
                    session->pingCount = 0;
                    plen -= session->header.payload_rem;
                } else {
                    session->header.payload_rem -= plen;
                    plen = 0;
                }
                break;

            default:
                // Unsupported/undefined opcode - ditch any payload(?)
                if((frame_done = plen >= session->header.payload_rem))
                    plen -= session->header.payload_rem;
                else {
                    session->header.payload_rem -= plen;
                    plen = 0;
                }
                break;
        }

        if(frame_done) {
            if(session->payload) {
                free(session->payload);
                session->payload = NULL;
                session->collect_payload = false;
            }
            if(session->header.frame)
                free(session->header.frame);
            memset(&session->header, 0, sizeof(frame_header_t));
        }
    }

    return len - plen;
}

//
// Queue incoming packet for processing
//
static err_t websocket_recv (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    ws_sessiondata_t *session = arg;

    if(err != ERR_OK || p == NULL || session == NULL) {

        if (p != NULL) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }

        websocket_close_conn(session, pcb);

    } else if(session->packet.p == NULL) {

        struct pbuf *q = p;
        uint8_t *payload = q->payload;
        uint_fast16_t len = q->len, processed, taken = 0;

        while(q) {
            processed = websocket_msg_parse(session, payload, len);
            payload += processed;
            taken += processed;
            len -= processed;

            if(streambuffers.rxbuf.overflow)
                break;

            if(len == 0 && (q = q->next)) {
                len = q->len;
                payload = q->payload;
            }
        }

        tcp_recved(session->pcb, taken);

        if(q == NULL) {
            pbuf_free(p);
            session->packet.p = NULL;
            websocket_stream_handler(session);
        } else {
            session->packet.p = p;
            session->packet.q = q;
            session->packet.len = q->len - (payload - (uint8_t *)q->payload);
            session->packet.payload = payload;
        }
    }

    return ERR_OK;
}

static err_t websocket_sent (void *arg, struct tcp_pcb *pcb, u16_t ui16len)
{
    ((ws_sessiondata_t *)arg)->timeout = 0;

    return ERR_OK;
}

/** Call tcp_write() in a loop trying smaller and smaller length
 *
 * @param pcb tcp_pcb to send
 * @param ptr Data to send
 * @param length Length of data to send (in/out: on return, contains the
 *        amount of data sent)
 * @param apiflags directly passed to tcp_write
 * @return the return value of tcp_write
 */
static err_t http_write (struct tcp_pcb *pcb, const void *ptr, u16_t *length, u8_t apiflags)
{
    u16_t len;
    err_t err;

    LWIP_ASSERT("length != NULL", length != NULL);

    len = *length;

    if (len == 0)
        return ERR_OK;

    do {
        if ((err = tcp_write(pcb, ptr, len, apiflags)) == ERR_MEM) {
            if (tcp_sndbuf(pcb) == 0 || tcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN)
        /* no need to try smaller sizes */
                len = 1;
            else
                len /= 2;
        }
    } while (err == ERR_MEM && len > 1);

    *length = len;

    return err;
}

static void http_write_error (ws_sessiondata_t *session, const char *status)
{
    uint16_t len = strlen(status);
    http_write(session->pcb, status, &len, TCP_WRITE_FLAG_COPY);
    session->state = WsState_Closing;
}

bool websocket_claim_stream (websocket_t *websocket)
{
    const io_stream_t *stream;
    ws_sessiondata_t *session = (ws_sessiondata_t *)websocket;

    if(session && session->magic == WEBSOCKETD_MAGIC && (stream = claim_stream(0))) {

        if(hal.stream.type == StreamType_WebSocket || !session->stream_state.connected)
            return session->stream != NULL;

        stream_connect(stream);

        if(hal.stream.type == StreamType_WebSocket || hal.stream.state.webui_connected) {
            session->stream = stream;
            streambuffers.session = session;
            hal.stream.state = session->stream_state;
        }

        ws_streams[0].flags.connected = true;
    }

    return hal.stream.type == StreamType_WebSocket;
}

//
// Process connection handshake
//
static err_t http_recv (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    static uint32_t ptr = 0;

    ws_sessiondata_t *session = arg;

    bool hdr_ok;

    if(err != ERR_OK || p == NULL || session == NULL) {

        if (p != NULL) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }

        websocket_close_conn(session, pcb);

        return ERR_OK;
    }

    if(session->http_request == NULL) {
        ptr = 0;
        if((session->http_request = malloc(session->hdrsize)) == NULL) {
            http_write_error(session, HTTP_500);
            return ERR_MEM;
        }
    }

    struct pbuf *q = p;
    uint8_t *payload = q->payload;
    uint_fast16_t len = q->len;

    uint32_t hdrsize = session->hdrsize - 1;

    // 1. Process input

    while(q) {

        if(ptr == hdrsize) {
            session->hdrsize += p->tot_len - len + 1;
            if((session->http_request = realloc(session->http_request, session->hdrsize)) == NULL) {
                http_write_error(session, HTTP_500);
                return ERR_MEM;
            }
            hdrsize = session->hdrsize - 1;
        }

        if(len) {
            session->http_request[ptr++] = *payload++;
            len--;
        } else if((q = q->next)) {
            len = q->len;
            payload = q->payload;
        }
    }

    tcp_recved(session->pcb, p->tot_len);
    pbuf_free(p);

    session->http_request[ptr] = '\0';

    if((hdr_ok = strstr(session->http_request, CRLF CRLF))) {

#ifdef WSDEBUG
    DEBUG_PRINT(session->http_request);
#endif

        char c = '\r', *argp, *argend, *protocols = NULL, *protocol = NULL;

        if((argend = stristr(session->http_request, WS_PROT))) {

            argp = argend + sizeof(WS_PROT) - 1;

            if((argend = strstr(argp, CRLF))) {

                *argend = '\0';

                // Trim leading spaces from protocol
                while(*argp == ' ')
                    argp++;

                // Trim trailing spaces from protocol
                while(*(argend - 1) == ' ') {
                    *argend = c;
                    c = *(--argend);
                    *argend = '\0';
                }

                if((protocols = malloc(strlen(argp) + 1))) {

                    bool is_binary = false;

                    memcpy(protocols, argp, strlen(argp) + 1);

                    if(websocket.on_protocol_select)
                        protocol = websocket.on_protocol_select(session, protocols, &is_binary);

                    if(protocol == NULL) {

                        protocol = protocols;

                        // Switch to binary frames if protocol is arduino or webui
                        if(strlookup(protocols, "arduino", ',') >= 0) {
                            strcpy(protocol, "arduino");
                            session->ftype = wshdr_bin;
                        } else if((argp = strchr(protocols, ','))) // Select the first protocol if more than one and not arduino
                            *argp = '\0';
                    } else if(is_binary)
                        session->ftype = wshdr_bin;
                }

                *argend = c;
            }
        }

        if((argend = stristr(session->http_request, WS_KEY))) {

            argp = argend + sizeof(WS_KEY) - 1;

            if((argend = strstr(argp, CRLF))) {

                char key[64];
                char rsp[200];

                *argend = '\0';

                // Trim leading spaces from key
                while(*argp == ' ')
                    argp++;

                // Trim trailing spaces from key
                while(*(argend - 1) == ' ') {
                    *argend = c;
                    c = *(--argend);
                    *argend = '\0';
                }

                // Copy base response header to response buffer
                char *response = memcpy(rsp /*session->http_request*/, WS_RSP, sizeof(WS_RSP) - 1);

                // Concatenate keys
                strcpy(key, argp);
                strcat(key, WS_GUID);

                *argend = c;

                // Get SHA1 of keys
                BYTE sha1sum[SHA1_BLOCK_SIZE];
                SHA1_CTX ctx;
                sha1_init(&ctx);
                sha1_update(&ctx, (BYTE *)key, strlen(key));
                sha1_final(&ctx, sha1sum);

                // Base64 encode SHA1
                size_t olen = base64_encode((BYTE *)sha1sum, (BYTE *)&response[sizeof(WS_RSP) - 1], SHA1_BLOCK_SIZE, 0);

                // Upgrade...
                if (olen) {
                    response[olen + sizeof(WS_RSP) - 1] = '\0';
                    if(protocol) {
                        strcat(response, CRLF);
                        strcat(response, WS_PROT);
                        strcat(response, protocol);
                    }
                    strcat(response, CRLF CRLF);
#ifdef WSDEBUG
    DEBUG_PRINT(response);
#endif
                    u16_t len = strlen(response);
                    http_write(session->pcb, response, (u16_t *)&len, TCP_WRITE_FLAG_COPY);
                    session->state = WsState_Connected;
                    session->lastSendTime = xTaskGetTickCount();
                }
            }
        }

        free(session->http_request);
        if(protocols)
            free(protocols);

        session->http_request = NULL;
        session->hdrsize = MAX_HTTP_HEADER_SIZE;

        if(session->state == WsState_Connected) {

            tcp_recv(pcb, websocket_recv);

            if(websocket.on_client_connect)
                websocket.on_client_connect(session);

            if(session->stream_state.connected)
                websocket_claim_stream(session);

        } else
            websocket_unlink_session(session);
    }

    // Bad request?
    if(hdr_ok ? session->state != WsState_Connected : ptr > (MAX_HTTP_HEADER_SIZE * 2)) {
        http_write_error(session, HTTP_400);
        if(session->http_request) {
            free(session->http_request);
            session->http_request = NULL;
            session->hdrsize = MAX_HTTP_HEADER_SIZE;
            websocket_unlink_session(session);
        }
    }

    return ERR_OK;
}

static err_t websocketd_accept (void *arg, struct tcp_pcb *pcb, err_t err)
{
    ws_sessiondata_t *session = NULL;

    uint_fast16_t idx = WEBUI_MAX_CLIENTS;

    do {
        if(clients[--idx].state == WsState_Free) {
            session = &clients[idx];
            memcpy(session, &defaultSettings, sizeof(ws_sessiondata_t));
            session->state = WsState_Connecting;
            break;
        }
    } while(idx);

    if(session == NULL) {

        if(!ws_server.link_lost)
            return ERR_CONN; // Busy, refuse connection

        // Link was previously lost, abort current connection
/*
        websocket_state_free(session);

        tcp_abort(session->pcb);

        ws_server.link_lost = false;
*/
        return ERR_ABRT;
    }

    streamClose(session);

    session->pcb = pcb;
    session->ftype = wshdr_txt;

    tcp_accepted(pcb);
    tcp_setprio(pcb, WEBSOCKETD_TCP_PRIO);
    tcp_arg(pcb, session);
    tcp_recv(pcb, http_recv);
    tcp_err(pcb, websocket_err);
    tcp_poll(pcb, websocket_poll, WEBSOCKETD_POLL_INTERVAL);
    tcp_sent(pcb, websocket_sent);

    return ERR_OK;
}

static void websocket_ping (ws_sessiondata_t *session)
{
    uint8_t txbuf[5];

    // Send ping every 3 seconds if no outgoing traffic.
    // Disconnect session after 3 failed pings (9 seconds).
    if(session->pingCount > 3)
        session->state = WsState_Closing;
    else if(session->state != WsState_Closing && (xTaskGetTickCount() - session->lastSendTime) > (3 * configTICK_RATE_HZ)) {
        if(tcp_sndbuf(session->pcb) > 4) {
            txbuf[0] = wshdr_ping.token;
            txbuf[1] = 2;
            strcpy((char *)&txbuf[2], "Hi");
            tcp_write(session->pcb, txbuf, 4, TCP_WRITE_FLAG_COPY);
            tcp_output(session->pcb);
            session->lastSendTime = xTaskGetTickCount();
            session->pingCount++;
        }
    }
}

static void websocket_stream_handler (ws_sessiondata_t *session)
{
    static uint8_t txbuf[TX_BUFFER_SIZE + 4];

    uint_fast16_t len;

    // 1. Process pending input packet
    if(session->packet.p) {

        struct pbuf *q = session->packet.q;
        uint8_t *payload = session->packet.payload;
        uint_fast16_t processed, taken = 0;

        len = session->packet.len;

        while(q) {
            processed = websocket_msg_parse(session, payload, len);
            payload += processed;
            taken += processed;
            len -= processed;

            if(streambuffers.rxbuf.overflow)
                break;

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
    if((len = streamTxCount()) && tcp_sndbuf(session->pcb) > 4) {

        int16_t c;
        uint_fast16_t idx = 0;

        if(len > tcp_sndbuf(session->pcb) - 4)
            len = tcp_sndbuf(session->pcb) - 4;

        txbuf[idx++] = session->ftype.token;
        txbuf[idx++] = len < 126 ? len : 126;
        if(len >= 126) {
            txbuf[idx++] = (len >> 8) & 0xFF;
            txbuf[idx++] = len & 0xFF;
        }

        while(len) {
            if((c = (uint8_t)streamTxGetC()) == SERIAL_NO_DATA)
                break;
            txbuf[idx++] = (uint8_t)c;
            len--;
        }

#ifdef WSDEBUG
    DEBUG_PRINT(uitoa(txbuf[1]));
    DEBUG_PRINT(" - ");
    DEBUG_PRINT(uitoa(idx));
    DEBUG_PRINT(" - ");
    DEBUG_PRINT(uitoa(plen));
    DEBUG_PRINT("\r\n");
#endif

        tcp_write(session->pcb, txbuf, (u16_t)idx, TCP_WRITE_FLAG_COPY);
        tcp_output(session->pcb);

        session->lastSendTime = xTaskGetTickCount();
    }
}

//
// Process data for streaming
//
void websocketd_poll (void)
{
    ws_sessiondata_t *client;
    uint_fast16_t idx = WEBUI_MAX_CLIENTS;

    do {
        client = &clients[--idx];
        if(client->state == WsState_Connected) {
            if(client->stream)
                websocket_stream_handler(client);
            websocket_ping(client);
        } else if(client->state == WsState_Closing)
            websocket_close_conn(client, client->pcb);
    } while(idx);
}

void websocketd_notify_link_status (bool up)
{
    if(!up)
        ws_server.link_lost = true;
}

void websocketd_close_connections (void)
{
    uint_fast16_t idx = WEBUI_MAX_CLIENTS;

    do {
        if(clients[--idx].state == WsState_Connected)
            websocket_close_conn(&clients[idx], clients[idx].pcb);
    } while(idx);
}

void websocketd_stop (void)
{
    ws_sessiondata_t *client;
    uint_fast16_t idx = WEBUI_MAX_CLIENTS;

    do {
        client = &clients[--idx];

        if(client->pcb != NULL) {
            tcp_arg(client->pcb, NULL);
            tcp_recv(client->pcb, NULL);
            tcp_sent(client->pcb, NULL);
            tcp_err(client->pcb, NULL);
            tcp_poll(client->pcb, NULL, 0);

            tcp_abort(client->pcb);

            client->pcb = NULL;
        }

        websocket_unlink_session(client);

    } while(idx);

    if(ws_server.pcb != NULL)
        tcp_close(ws_server.pcb);
}

bool websocketd_init (uint16_t port)
{
    static io_stream_details_t streams = {
        .n_streams = sizeof(ws_streams) / sizeof(io_stream_properties_t),
        .streams = ws_streams,
    };

    err_t err;

    ws_server.port = port;
    ws_server.link_lost = false;

    struct tcp_pcb *pcb = tcp_new();

    if((err = tcp_bind(pcb, IP_ADDR_ANY, port)) == ERR_OK) {
        ws_server.pcb = tcp_listen(pcb);
        tcp_accept(ws_server.pcb, websocketd_accept);
        stream_register_streams(&streams);
    }

    return err == ERR_OK;
}

#endif
