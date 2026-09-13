#include <stdio.h>
#include "ff.h"

void test() {
    FF_DIR dir;
    FILINFO fno;
    f_opendir(&dir, "0:/");
    f_readdir(&dir, &fno);
}
