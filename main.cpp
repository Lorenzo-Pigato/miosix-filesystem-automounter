
#include <cstdio>
#include "miosix/arch/cortexM4_stm32f4/stm32f407vg_stm32f4discovery/interfaces-impl/bsp_impl.h"
#include "miosix.h"

using namespace std;
using namespace miosix;

int main()
{
    printf("---- Program started ----\n");
    for(;;)
    {
        ledOn();
        Thread::sleep(1000);
        ledOff();
        Thread::sleep(1000);
    }
}
