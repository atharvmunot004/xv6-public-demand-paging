#include "types.h"      
#include "user.h"

int main(int argc, char *argv[]) 
{ 
    printf(1, "Hey, I am trying xv6\n"); 
    int *p = (int *)0x40000000;
    *p = 43;
    exit();
}