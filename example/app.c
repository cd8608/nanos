#include <unistd.h>

int main()
{
    char hello_world[] = "Hello World!\n";
    write (1, hello_world, sizeof(hello_world)-1);
    return 0;
}