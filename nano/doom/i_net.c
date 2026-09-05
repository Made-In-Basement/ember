/* Doom for Ember: no network, single player */
#include <nanolibc.h>
#include "doomdef.h"
#include "doomstat.h"
#include "d_net.h"
#include "i_net.h"

void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));
    doomcom->id = DOOMCOM_ID;
    doomcom->ticdup = 1;
    doomcom->extratics = 0;
    doomcom->numnodes = 1;
    doomcom->numplayers = 1;
    doomcom->consoleplayer = 0;
    doomcom->deathmatch = 0;
    netgame = false;
}

void I_NetCmd(void)
{
}
