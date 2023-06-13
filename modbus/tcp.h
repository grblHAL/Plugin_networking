/*

  tcp.h - a lightweight ModBus TCP implementation

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

#ifndef _MODBUS_TCP_H_
#define _MODBUS_TCP_H_

#pragma pack(push, 1)

typedef struct {
    uint16_t length;
    uint8_t uid;
    uint8_t code;
    uint8_t data[MODBUS_MAX_ADU_SIZE];
} modbus_tcp_pdu_t;

typedef struct {
    uint16_t tid;
    uint16_t pid;
    modbus_tcp_pdu_t pdu;
} modbus_tcp_adu_t;

#pragma pack(pop)

#endif
