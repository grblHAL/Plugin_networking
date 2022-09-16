//
// websocketd.h - lwIP websocket daemon implementation
//
// v2.5 / 2022-09-15 / Io Engineering / Terje
//

/*

Copyright (c) 2019-2022, Terje Io
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
s
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

#ifndef __WSSTREAM_H__
#define __WSSTREAM_H__

typedef void websocket_t;
typedef char *(*websocket_on_protocol_select_ptr)(websocket_t *websocket, char *protocols, bool *is_binary);
typedef void (*websocket_on_client_connect_ptr)(websocket_t *websocket);
typedef void (*websocket_on_client_disconnect_ptr)(websocket_t *websocket);
typedef void (*websocket_on_frame_received_ptr)(websocket_t *websocket, void *data, size_t size);

typedef struct {
    websocket_on_protocol_select_ptr on_protocol_select;
    websocket_on_client_connect_ptr on_client_connect;
    websocket_on_client_disconnect_ptr on_client_disconnect;
} websocket_events_t;

extern websocket_events_t websocket;

bool websocketd_init (uint16_t port);
void websocketd_poll (void);
void websocketd_notify_link_status (bool link_up);
bool websocketd_RxPutC (char c);
void websocketd_stop (void);
void websocketd_close_connections (void);
bool websocket_register_frame_handler (websocket_t *websocket, websocket_on_frame_received_ptr handler, bool binary);
bool websocket_send_frame (websocket_t *websocket, const void *data, size_t size, bool is_binary);
bool websocket_broadcast_frame (const void *data, size_t size, bool is_binary);
bool websocket_set_stream_flags (websocket_t *session, io_stream_state_t stream_flags);
bool websocket_claim_stream (websocket_t *session);

#endif
