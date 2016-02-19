#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <stdint.h>

/* CONSTANTS */
#define Q 14
#define F (1 << Q)

typedef int32_t fixed_point;

/* CONVERTING */
#define convert_to_fixed_point(n) ( (n) * (F) )
/*converts x to an integer and rounds towards 0*/
#define convert_to_int_round_down(x) ( (x) / (F) )
/*converts x to integer and rounds to nearest whole integer*/
#define convert_to_int_round_nearest(x) ( ( (x) >= 0 )         \
            ? ( ((x) + ( (F) / 2) ) / F )          \
            : ( ((x) - ( (F) / 2) ) / F ) )

/* ADDITION */
#define add_fixed_to_fixed(x, y) ( (x) + (y) )
#define add_fixed_to_int(x, n) ( (x) + ((n) * (F)) )

/* SUBTRACTION */
#define fixed_subtract_fixed(x, y) ( (x) - (y) )
#define fixed_subtract_int(x, n) ( (x) - ((n) * (F)) )

/* MULTIPLICATION */
#define multiply_fixed_by_fixed(x, y) ( ( ( (int64_t) (x) ) * (y) ) / (F) )
#define multiply_fixed_by_int(x, n) ( (x) * (n) )

/* DIVISION */
#define div_fixed_by_fixed(x, y) ( ( ( (int64_t) (x) ) * (F) ) / (y) )
#define div_fixed_by_int(x, n) ( (x) / (n) )

#endif /* threads/fixed_point.h */
