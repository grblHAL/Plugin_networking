/*

  client.h - a lightweight ModBus TCP implementation

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

#ifndef _MODBUS_TCP_CLIENT_H_
#define _MODBUS_TCP_CLIENT_H_

#ifdef ARDUINO
#include "../../grbl/modbus.h"
#else
#include "grbl/modbus.h"
#endif

#include "tcp.h"

void modbus_tcp_client_poll (void);
void modbus_tcp_client_start (void);
void modbus_tcp_client_init (void);

bool modbus_tcp_send (modbus_tcp_pdu_t *pdu, const modbus_callbacks_t *callbacks, void *context, bool block);

#endif
