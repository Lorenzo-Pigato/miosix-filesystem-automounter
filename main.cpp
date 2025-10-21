
#include <cstdio>
#include "miosix.h"

using namespace std;
using namespace miosix;

int main()
{
    for (int i = 0; i< 10; i++)
    {
        ledOn();
        Thread::sleep(1000);
        ledOff();
        Thread::sleep(1000);
    }
}
