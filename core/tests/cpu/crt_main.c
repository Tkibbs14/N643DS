#include "plat.h"

void crt_main(void);
void crt_main(void) {
    int r = test_main();
    if (ON_ROM) puts_(g_failures || r ? "FAIL (end)\n" : "PASS\n");
    else puts_("END\n");
    plat_exit(g_failures || r);
}
