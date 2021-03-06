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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libnfc.h"

static byte abtRecv[MAX_FRAME_LEN];
static ui32 uiRecvBits;
static dev_info* pdi;

// ISO14443A Anti-Collision response
byte abtAtqa      [2] = { 0x04,0x00 };
byte abtUidBcc    [5] = { 0xDE,0xAD,0xBE,0xAF,0x62 };
byte abtSak       [9] = { 0x08,0xb6,0xdd };

int main(int argc, const char* argv[])
{			
  byte* pbtTx = null;
  ui32 uiTxBits;
  
  // Try to open the NFC reader
  pdi = nfc_connect();
  
  if (pdi == INVALID_DEVICE_INFO)
  {
    printf("Error connecting NFC second reader\n");
    return 1;
  }

  printf("\n");
  printf("[+] Connected to NFC reader: %s\n",pdi->acName);
  printf("[+] Try to break out the auto-simulation, this requires a second reader!\n");
  printf("[+] To do this, please send any command after the anti-collision\n");
  printf("[+] For example, send a RATS command or use the \"anticol\" tool\n");
  if (!nfc_target_init(pdi,abtRecv,&uiRecvBits))
  {
    printf("Error: Could not come out of auto-simulation, no command was received\n");
    return 1;
  }
  printf("[+] Received initiator command: ");
  print_hex_bits(abtRecv,uiRecvBits);
  printf("[+] Configuring communication\n");
  nfc_configure(pdi,DCO_HANDLE_CRC,false);
  nfc_configure(pdi,DCO_HANDLE_PARITY,true);
  printf("[+] Done, the simulated tag is initialized\n\n");

  while(true)
  {
    // Test if we received a frame
    if (nfc_target_receive_bits(pdi,abtRecv,&uiRecvBits,null))
    {
      // Prepare the command to send back for the anti-collision request
      switch(uiRecvBits)
      {
        case 7: // Request or Wakeup
          pbtTx = abtAtqa;
          uiTxBits = 16;
          // New anti-collsion session started
          printf("\n"); 
        break;

        case 16: // Select All
          pbtTx = abtUidBcc;
          uiTxBits = 40;
        break;

        case 72: // Select Tag
          pbtTx = abtSak;
          uiTxBits = 24;
        break;

        default: // unknown length?
          uiTxBits = 0;
        break;
      }

      printf("R: ");
      print_hex_bits(abtRecv,uiRecvBits);

      // Test if we know how to respond
      if(uiTxBits)
      {
        // Send and print the command to the screen
        nfc_target_send_bits(pdi,pbtTx,uiTxBits,null);
        printf("T: ");
        print_hex_bits(pbtTx,uiTxBits);
      }
    }
  }

  nfc_disconnect(pdi);
}
