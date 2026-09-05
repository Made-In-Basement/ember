/* Doom for Ember: program entry */
#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

int main(int argc, char **argv)
{
    myargc = argc;
    myargv = argv;
    printf("NDOOM: native Doom for Ember (32-bit, HD Audio sound effects)\n");
    D_DoomMain();
    return 0;
}
