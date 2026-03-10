/* i_network_tanmatsu.c -- Network stub for Tanmatsu */

#include <string.h>
#include "config.h"
#include "doomtype.h"
#include "d_ticcmd.h"
#include "m_swap.h"
#include "protocol.h"
#include "i_network.h"

size_t sentbytes = 0;
size_t recvdbytes = 0;

void I_InitNetwork(void) {}

size_t I_GetPacket(packet_header_t *buffer, size_t buflen)
{
    return 0;
}

void I_SendPacket(packet_header_t *packet, size_t len) {}
void I_WaitForPacket(int ms) {}
