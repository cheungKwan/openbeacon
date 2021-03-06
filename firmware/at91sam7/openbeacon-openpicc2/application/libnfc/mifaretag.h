/*
 
Public platform independent Near Field Communication (NFC) library
Copyright (C) 2009, Roel Verdult
 
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef _LIBNFC_MIFARE_TAG_H_
#define _LIBNFC_MIFARE_TAG_H_

#include "defines.h"

typedef struct {
  byte abtUID[4];
  byte btBCC;
  byte btUnknown;
  byte abtATQA[2];
  byte abtUnknown[8];
} mifare_block_manufacturer;

typedef struct {
  byte abtData[16];
} mifare_block_data;

typedef struct {
  byte abtKeyA[6];
  byte abtAccessBits[4];
  byte abtKeyB[6];
} mifare_block_trailer;

typedef union {
  mifare_block_manufacturer mbm;
  mifare_block_data mbd;
  mifare_block_trailer mbt;
} mifare_block;

typedef struct {
  mifare_block amb[256];
} mifare_tag;

#endif // _LIBNFC_MIFARE_TAG_H_
