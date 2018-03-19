#include <sruntime.h>

typedef struct breakpoint {
    u32 address;
    boolean assigned;
    void (*set)();
} *breakpoint;

// there are 7 of these
static void _b0(u64 z){__asm__("mov %0, %%dr0\n":: "a"(z));}
static void _b1(u64 a){__asm__("mov %0, %%dr1\n":: "a"(a));}
static void _b2(u64 a){__asm__("mov %0, %%dr2":: "a"(a));}
static void _b3(u64 a){__asm__("mov %0, %%dr3":: "a"(a));}

struct breakpoint breakpoints[4] = {{0, 0, _b0}, {0, 0, _b1}, {0, 0, _b2}, {0, 0, _b3}};

#define mutate(__x, __offset, __len, __v)                           \
    (((__x) & ~ (((1<<__len) - 1) << (__offset))) | ((__v)<<(__offset)))

boolean breakpoint_insert(u32 a)
{
    for (int i = 0; i< 4; i++) {
        if (!breakpoints[i].assigned) {
            register u64 dr7;

            breakpoints[i].assigned = true;
            breakpoints[i].address = a;
            breakpoints[i].set(a);
            
            mov_from_cr("dr7", dr7);
            // r/w bits
            dr7 = mutate(dr7, 4 * i + 16, 2, 0); 
            // len
            dr7 = mutate(dr7, 4 * i + 18, 2, 0);
            // both global and local
            dr7 = mutate(dr7, 2 * i, 2, 3);
            mov_to_cr("dr7", dr7);
            return(true);
        }
    } 
    return(false);
}

boolean breakpoint_remove(u32 a)
{
    for (int i = 0; i< 4; i++) {
        if (breakpoints[i].address == a) {
            register u64 dr7;

            breakpoints[i].assigned = false;
            mov_from_cr("dr7", dr7);
            dr7 = mutate(dr7, 2 * i, 2, 0);
            mov_to_cr("dr7", dr7);
            return(true);
        }
    }
    return(false);
}
