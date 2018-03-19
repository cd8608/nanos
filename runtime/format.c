#include <runtime.h>

typedef u8 character;
static char *hex_digits="0123456789abcdef";

#define MAX(a, b) ((a)>(b)?(a):(b))

void format_number(buffer s, u64 x, int base, int pad)
{
    if ((x > 0) || (pad > 0)) {
        format_number(s, x/base, base, pad - 1);
        push_character(s, hex_digits[x%base]);
    }
}

void empty_buffer(buffer b)
{
    b->start = b->end = 0;
}

// should entertain a registration method with a type and a character and a function pointer
// or maybe just float this up to runtime

void vbprintf(buffer s, buffer fmt, vlist ap)
{
    character i;
    int state = 0;
    int base = 0;
    int pad;
    int count = 0;

    foreach_character(i, fmt) {
        switch (state){
        case 2:
            for (int j = 0; j < count; j++)
                push_character(s, i);
            state = 0;
            break;

        case 0:
            base = 10;
            pad = 0;
            if (i == '%') state = 3;
            else push_character(s, i);
            break;

        case 1:
            if ((i >= '0') && (i <= '9')) {
                pad = pad * 10 + (i - '0');
                break;
            } else {
                state = 3;
            }

        case 3:
            switch (i) {
            case '0':
                state = 1;
                break;

            case '%':
                push_character(s, '\%');
                break;

                //            case 't':
                //                print_time(s, varg(ap, ticks));
                //                break;

            case 'b':
                push_buffer(s, varg(ap, buffer));
                break;

            case 'n':
                count = varg(ap, unsigned int);
                state = 2;
                break;

            case 'c':
                push_character(s, varg(ap, int));
                break;

            case 's':
                {
                    char *c = varg(ap, char *);
                    if (!c) c = (char *)"(null)";
                    int len = runtime_strlen(c);
                    for (int i =0 ; i < pad; i++)
                        push_character(s, ' ');
                    pad = 0;
                    for (; *c; c++)
                        push_character(s, *c);
                }
                break;

            case 'S':
                {
                    unsigned int x = varg(ap, unsigned int);
                    for (int i =0 ; i < x; i++) push_character(s, ' ');
                    break;
                }

            // vector of strings
            case 'v':
                {
                    vector v = varg(ap, buffer);
                    buffer i;
                    boolean start=true;
                    push_character(s, '[');
                    vector_foreach(i, v) {
                        if (start) {
                            start = false;
                        } else {
                            push_character(s, ',');
                            push_character(s, ' ');
                        }
                        push_buffer(s, i);
                    }
                    push_character(s, ']');
                    break;
                }
                
            case 'p':
                pad = 16;
                unsigned long x = varg(ap, unsigned long);
                format_number(s, x, 16, pad?pad:1);
                break;

            case 'l':
                pad = 0;
                unsigned long z = varg(ap, unsigned long);
                format_number(s, z, 10, pad?pad:1);
                break;

            case 'x':
                base=16;

            case 'o':
                if (base == 10) base=8;
            case 'u':
                {
                    unsigned int x = varg(ap, unsigned int);
                    format_number(s, x, base, pad?pad:1);
                    break;
                }

            case 'X':
                print_hex_buffer(s, varg(ap, buffer));
                break;

            case 'd': case 'i':
                {
                    int x = varg(ap, int);
                    if (x <0){
                        push_character(s, '-');
                        x = -x;
                    }
                    format_number(s, (unsigned int)x, base, pad?pad:1);
                    break;
                }
            default:
                break;
            }
            // badness
            if (state == 3)
                state = 0;
            break;
        }
    }
}


buffer aprintf(heap h, char *fmt, ...)
{
    buffer b = allocate_buffer(h, 80);
    vlist ap;
    buffer f = alloca_wrap_buffer(fmt, runtime_strlen(fmt));
    vstart (ap, fmt);
    vbprintf(b, f, ap);
    vend(ap);
    return(b);
}

void bbprintf(buffer b, buffer fmt, ...)
{
    vlist ap;
    vstart(ap, fmt);
    vbprintf(b, fmt, ap);
    vend(ap);
}

void bprintf(buffer b, char *fmt, ...)
{
    vlist ap;
    buffer f = alloca_wrap_buffer(fmt, runtime_strlen(fmt));
    vstart (ap, fmt);
    vbprintf(b, f, ap);
    vend(ap);
}
