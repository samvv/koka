/*---------------------------------------------------------------------------
  Copyright 2020 Daan Leijen, Microsoft Corporation.

  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the file "license.txt" at the root of this distribution.
---------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdint.h>
#include <string.h> // memcpy
#include <ctype.h>
#include "runtime.h"
#include "runtime/integer.h"

/*----------------------------------------------------------------------
Big integers. For our purposes, we need an implementation that does not have to be the fastest 
possible; we instead aim for portable, simple, well performing, and with fast conversion to/from decimal strings. 
Still, it performs quite respectable and does have various optimizations including Karatsuba multiplication.

  Big integers are arrays of `digits` with a `count` and `is_neg` flag. For a number `n` we have:
  
  n = (is_neg ? -1 : 1) * (digits[0]*(BASE^0) + digits[1]*(BASE^1) + ... + digits[count-1]*(BASE^(count-1)))

  For any `count>0`, we have `digits[count-1] != 0`.
  We use a decimal representation for efficient conversion of numbers to strings and back.
  We use 32-bit integers for the digits, this way:
  - we can use base 10^9 (which uses 29.9 bits of the 32 available).
  - it can hold `2*BASE + 1` which allows for efficient addition.
  - a double digit `ddigit_t` of 64-bit can hold a full multiply of `BASE*BASE + BASE + 1` 
    which allows efficient multiplication.
----------------------------------------------------------------------*/

typedef int32_t   digit_t;     // 2*BASE + 1 < digit_t_max
typedef int64_t   ddigit_t;    // (BASE*BASE + BASE) + 1 < ddigit_t_max
#define BASE        ((intptr_t)1000000000UL)  
#define LOG_BASE    (9)

typedef uint16_t extra_t;
#define MAX_EXTRA           (UINT16_MAX / 2)  // we use 1 bit for the negative bool

typedef struct bigint_s {
#if INTPTR_SIZE>=8
  uint8_t  is_neg: 1;      // negative
  extra_t  extra :15;      // extra digits available: `sizeof(digits) == (count+extra)*sizeof(digit_t)`
  uint64_t count :48;      // count of digits in the number
#else
  uint8_t  is_neg;
  extra_t  extra;
  uint32_t count;
#endif
  digit_t  digits[1];      // digits from least-significant to most significant. 
} bigint_t;


static bool bigint_is_neg_(const bigint_t* b) {
  return (b->is_neg != 0);
}

static intptr_t bigint_sign_(const bigint_t* b) {
  return (bigint_is_neg_(b) ? -1 : 1);
}

static size_t bigint_count_(const bigint_t* b) {
  return (b->count);
}

static size_t bigint_available_(const bigint_t* b) {
  return (b->count + b->extra);
}

static digit_t bigint_last_digit_(const bigint_t* b) {
  return b->digits[b->count-1];
}

static integer_t integer_bigint(bigint_t* x);

/*----------------------------------------------------------------------
  allocation, ref counts, trim
  functions ending in "_" do not change reference counts of input arguments
----------------------------------------------------------------------*/

static ptr_t bigint_ptr_(bigint_t* x) {
  return ptr_from_data(x);
}

static bool bigint_is_unique_(bigint_t* x) {
  return ptr_is_unique(bigint_ptr_(x));
}

static void bigint_incref(bigint_t* x) {
  ptr_incref(bigint_ptr_(x));
}

static void bigint_decref(bigint_t* x) {
  ptr_decref(bigint_ptr_(x));
}

static bigint_t* bigint_dup(bigint_t* x) {
  ptr_incref(bigint_ptr_(x));
  return x;
}


static size_t bigint_roundup_count(size_t count) {
  if (count < 4) return 4;                      // minimal size of 4 digits (128-bit)
  else if ((count & 1) == 1) return (count+1);  // always even 
  else return count;
}

static bigint_t* bigint_alloc(size_t count, bool is_neg) {
  size_t dcount = bigint_roundup_count(count);
  bigint_t* b = ptr_data_as(bigint_t, ptr_alloc(sizeof(bigint_t) - sizeof(digit_t) + dcount*sizeof(digit_t), 0, TAG_BIGINT));
  b->is_neg = (is_neg ? 1 : 0);
  b->extra = (extra_t)(dcount - count);
  b->count = count;
  return b;
}

static bigint_t* bigint_alloc_zero(size_t count, bool is_neg) {
  bigint_t* b = bigint_alloc(count, is_neg);
  memset(b->digits, 0, sizeof(digit_t)* bigint_available_(b));
  return b;
}

static bigint_t* bigint_trim_realloc_(bigint_t* x, size_t count) {
  assert_internal(bigint_is_unique_(x));
  size_t dcount = bigint_roundup_count(count);
  size_t xcount = bigint_available_(x);
  bigint_t* b;
  if ((dcount <= xcount) && (xcount-dcount) < 4) {
    b = x; // avoid realloc if shrinking by less than 4 digits.
    dcount = xcount;
  }
  else {
    b = ptr_data_as(bigint_t, ptr_realloc(bigint_ptr_(x), sizeof(bigint_t) - sizeof(digit_t) + dcount*sizeof(digit_t)));
  }
  b->count = count;
  b->extra = (extra_t)(dcount - count);
  return b;
}

static bigint_t* bigint_trim_to(bigint_t* x, size_t count, bool allow_realloc) {
  ptrdiff_t d = bigint_available_(x) - count;
  assert_internal(d >= 0 && bigint_is_unique_(x));
  if (d < 0) {
    return x;
  }
  else if (d > MAX_EXTRA) {
    if (allow_realloc) {
      return bigint_trim_realloc_(x, count);
    }
    else {
      x->count = count;
      x->extra = MAX_EXTRA;  // if we cannot realloc and extra > MAX_EXTRA, we may lose space :(
      return x;
    }
  }
  else {
    x->count = count;
    x->extra = (uint16_t)d;
    return x;
  }
}

static bigint_t* bigint_trim(bigint_t* x, bool allow_realloc) {
  assert_internal(bigint_is_unique_(x));
  size_t i = bigint_count_(x);
  while ((i > 0) && (x->digits[i-1] == 0)) { i--; }  // skip top zero's
  return bigint_trim_to(x, i, allow_realloc);
}

static bigint_t* bigint_alloc_reuse_(bigint_t* x, size_t count) {
  ptrdiff_t d = bigint_available_(x) - count;
  if (d >= 0 && d <= MAX_EXTRA && bigint_is_unique_(x)) {   // reuse?
    return bigint_trim_to(x, count, false /* no realloc */);    
  }
  else {
    return bigint_alloc(count, bigint_is_neg_(x)); 
  }
}

static bigint_t* bigint_copy(bigint_t* x, size_t extra ) {
  assert_internal(extra <= MAX_EXTRA);
  if (extra > MAX_EXTRA) extra = MAX_EXTRA;
  bigint_t* z = bigint_alloc(x->count + extra, bigint_is_neg_(x));
  z->is_neg = x->is_neg;
  z = bigint_trim_to(z, x->count, false);
  memcpy(z->digits, x->digits, x->count * sizeof(digit_t) );
  bigint_decref(x);
  return z;
}

static bigint_t* bigint_ensure_unique(bigint_t* x) {
  return (bigint_is_unique_(x) ? x : bigint_copy(x,0));
}

static bigint_t* bigint_push(bigint_t* x, digit_t d) {
  if (x->extra == 0) { x = bigint_copy(x, MAX_EXTRA); }
  x->digits[x->count] = d;
  x->count++;
  x->extra--;
  return x;
}

/*----------------------------------------------------------------------
  Conversion from numbers
----------------------------------------------------------------------*/

// Bigint to integer. Possibly converting to a small int.
static integer_t integer_bigint(bigint_t* x) {  
  if (x->count==1 && x->digits[0] <= SMALLINT_MAX) {
    // make it a small int
    intptr_t i = x->digits[0];
    if (x->is_neg) i = -i;
    bigint_decref(x);
    return integer_from_small(i);
  }
  else {
    return box_ptr(bigint_ptr_(x));
  }
}

// create a bigint from an intptr_t
static bigint_t* bigint_from_int(intptr_t i) {
  bool is_neg = (i < 0);
  if (is_neg) i = -i;
  bigint_t* b = bigint_alloc(0, is_neg); // will reserve at least 4 digits
  do {
    b = bigint_push(b, i%BASE);
    i /= BASE;
  } while (i > 0);
  return b;
}

// unpack to a bigint always
static bigint_t* integer_to_bigint(integer_t x) {
  assert_internal(is_integer(x));
  if (is_bigint(x)) {
    return ptr_data_as(bigint_t, unbox_ptr(x));
  }
  else {
    assert_internal(is_smallint(x));
    return bigint_from_int(unbox_int(x));
  }
}

integer_t integer_from_big(intptr_t i) {
  return box_ptr(bigint_ptr_(bigint_from_int(i)));
}


/*----------------------------------------------------------------------
  To string
----------------------------------------------------------------------*/

// Convert a digit to LOG_BASE characters.
// note: gets compiled without divisions on clang and GCC.
static intptr_t digit_to_str_full(digit_t d, char* buf) {
  for (intptr_t i = LOG_BASE - 1; i >= 0; i--, d /= 10) {
    buf[i] = '0' + (d % 10);
  }
  return LOG_BASE;
}
// convert digit to characters but skip leading zeros. No output if `d==0`.
static intptr_t digit_to_str_partial(digit_t d, char* buf) {
  char tmp[LOG_BASE];
  if (d==0) return 0;
  digit_to_str_full(d, tmp);
  intptr_t i = 0;
  while (i < LOG_BASE && tmp[i]=='0') { i++; }
  for (intptr_t j = i; j < LOG_BASE; j++) {
    buf[j - i] = tmp[j];
  }
  return (LOG_BASE - i);
}

// Efficient conversion to a string buffer. Use `buf == NULL` to get the required size.
static size_t bigint_to_buf_(const bigint_t* b, char* buf, size_t buf_size) {
  assert_internal(b != NULL);
  const intptr_t count = bigint_count_(b);
  const size_t needed = (count*LOG_BASE) + (bigint_is_neg_(b) ? 1 : 0) + 1; // + (sign and terminator);
  if (buf==NULL || buf_size==0 || needed > buf_size) return needed;
  intptr_t j = 0;  // current output position
  // sign
  if (bigint_is_neg_(b)) {
    buf[j++] = '-';
  }
  // skip leading zeros
  intptr_t i = count-1;
  while (i > 0 && b->digits[i]==0) {
    assert_internal(false); // we should never have leading zeros
    i--;
  }
  // output leading digit
  if (i >= 0) {
    j += digit_to_str_partial(b->digits[i], &buf[j]);
    i--;
  }
  // and output the rest of the digits
  while (i >= 0) {
    j += digit_to_str_full(b->digits[i], &buf[j]);
    i--;
  }
  buf[j++] = 0;
  return j;
}

static string_t bigint_to_string(bigint_t* b) {
  size_t needed = bigint_to_buf_(b, NULL, 0);
  string_t s = string_alloc_buf(needed);
  bigint_to_buf_(b, string_buf(s), needed);
  bigint_decref(b);
  return s;
}

// intptr_t to string
string_t int_to_string(intptr_t n) {
  assert_internal(INTPTR_SIZE <= 26);
  char buf[64];  // enough for 2^212
  bool neg = (n < 0);
  if (neg) n = -n;
  // output to buf in reverse order
  intptr_t i = 0;
  if (n == 0) {
    buf[i++] = '0';
  }
  else {
    for (; i < 63 && n != 0; i++, n /= 10) {
      buf[i] = '0' + n%10;
    }
    if (neg) {
      buf[i++] = '-';
    }
  }
  // write to the allocated string
  string_t s = string_alloc_buf(i + 1);
  char* p = string_buf(s);
  intptr_t j;
  for (j = 0; j < i; j++) {
    p[j] = buf[i - j - 1];
  }
  p[j] = 0;
  return s;
}

/*----------------------------------------------------------------------
  Parse an integer
----------------------------------------------------------------------*/

integer_t integer_parse(const char* s) {
  assert_internal(s!=NULL);
  // parse
  bool is_neg = false;
  size_t sig_digits = 0; // digits before the fraction
  size_t i = 0;
  // sign
  if (s[i] == '+') { i++; }
  else if (s[i] == '-') { is_neg = true; i++; }
  if (!isdigit(s[i])) return box_cptr(NULL);  // must start with a digit
  // significant 
  for (; s[i] != 0; i++) {
    char c = s[i];
    if (isdigit(c)) {
      sig_digits++;
    }
    else if (c=='_' && isdigit(s[i+1])) { // skip underscores
    }
    else if ((c == '.' || c=='e' || c=='E') && isdigit(s[i+1])) { // found fraction/exponent
      break;
    }
    else return box_cptr(NULL); // error    
  }
  // fraction
  size_t frac_digits = 0;
  if (s[i]=='.') {
    i++;
    for (; s[i] != 0; i++) {
      char c = s[i];
      if (isdigit(c)) {
        frac_digits++;
      }
      else if (c=='_' && isdigit(s[i+1])) { // skip underscores
      }
      else if ((c=='e' || c=='E') && isdigit(s[i+1]) && (s[i+1] != '0')) { // found fraction/exponent
        break;
      }
      else return box_cptr(NULL); // error    
    }
  }
  const char* end = s + i;
  // exponent 
  size_t exp = 0;
  if (s[i]=='e' || s[i]=='E') {
    i++;
    for (; s[i] != 0; i++) {
      char c = s[i];
      if (isdigit(c)) {
        exp = 10*exp + ((size_t)c - '0');
        if (exp > BASE) return box_cptr(NULL); // exponents must be < 10^9
      }
      else return box_cptr(NULL);
    }
  }
  if (exp < frac_digits) return box_cptr(NULL); // fractional number
  const size_t zero_digits = exp - frac_digits;
  const size_t dec_digits = sig_digits + frac_digits + zero_digits;  // total decimal digits needed in the bigint

  // parsed correctly, ready to construct the number
  // construct an `intptr_t` if it fits.
  if (dec_digits < LOG_BASE) {   // must be less than LOG_BASE to avoid overflow
    assert_internal(INTPTR_SIZE >= sizeof(digit_t));
    intptr_t d = 0;
    for (const char* p = s; p < end; p++) {
      char c = *p;
      if (isdigit(c)) {
        d = 10*d + ((intptr_t)c - '0');
      }
    }
    for (size_t z = 0; z < zero_digits; z++) {
      d *= 10;
    }
    if (is_neg) d = -d;
    return integer_from_int(d);
  }

  // otherwise construct a big int
  const size_t count = ((dec_digits + (LOG_BASE-1)) / LOG_BASE); // round up
  bigint_t* b = bigint_alloc(count, is_neg);
  size_t k     = count;
  size_t chunk = dec_digits%LOG_BASE; if (chunk==0) chunk = LOG_BASE; // initial number of digits to read
  const char* p = s;
  while (p < end) {   
    digit_t d = 0;
    // read a full digit
    for (size_t j = 0; j < chunk; ) {
      char c = (p < end ? *p++ : '0'); // fill out with zeros
      if (isdigit(c)) {
        j++;
        d = 10*d + ((digit_t)c - '0'); assert_internal(d<BASE);
      }
    }
    // and store it
    assert_internal(k > 0);
    if (k > 0) { b->digits[--k] = d; }    
    chunk = LOG_BASE;  // after the first digit, all chunks are full digits
  }
  // set the final zeros
  assert_internal(zero_digits / LOG_BASE == k);
  for (size_t j = 0; j < k; j++) { b->digits[j] = 0; }
  return integer_bigint(b);
}

integer_t integer_from_str(const char* num) {
  integer_t i = integer_parse(num);
  assert_internal(i != box_cptr(NULL));
  return i;
}

/*----------------------------------------------------------------------
  negate, compare
----------------------------------------------------------------------*/

static bigint_t* bigint_neg(bigint_t* x) {
  bigint_t* z = bigint_ensure_unique(x);
  z->is_neg = !z->is_neg;
  return z;
}


static int bigint_compare_abs_(bigint_t* x, bigint_t* y) {
  size_t cx = bigint_count_(x);
  size_t cy = bigint_count_(y);
  if (cx > cy) return 1;
  if (cx < cy) return -1;
  for (size_t i = cx; i > 0; ) {
    i--;
    if (x->digits[i] != y->digits[i]) return (x->digits[i] > y->digits[i] ? 1 : -1);
  }
  return 0;
}

static int bigint_compare_(bigint_t* x, bigint_t* y) {
  if (x->is_neg != y->is_neg) {
    return (y->is_neg ? 1 : -1);
  }
  else {
    return (int)(bigint_sign_(x)* bigint_compare_abs_(x, y));
  }
}

/*----------------------------------------------------------------------
  add absolute
----------------------------------------------------------------------*/

static bigint_t* bigint_add(bigint_t* x, bigint_t* y, bool y_isneg);
static bigint_t* bigint_sub(bigint_t* x, bigint_t* y, bool yneg);


static bigint_t* bigint_add_abs(bigint_t* x, bigint_t* y) {   // x.count >= y.count
  // assert_internal(bigint_sign_(x) == bigint_sign_(y));  
  // ensure x.count >= y.count
  const size_t cx = bigint_count_(x);
  const size_t cy = bigint_count_(y);
  assert_internal(cx >= cy);
  
  // allocate result bigint
  const size_t cz = ((intptr_t)bigint_last_digit_(x) + (intptr_t)bigint_last_digit_(y) + 1 >= BASE ? cx + 1 : cx);
  bigint_t* z = bigint_alloc_reuse_(x, cz); // if z==x, we reused x.
  //z->is_neg = x->is_neg;

  assert_internal(cx>=cy);
  assert_internal(bigint_count_(z) >= cx);
  digit_t carry = 0;
  digit_t sum = 0;
  // add y's digits
  size_t i;
  for (i = 0; i < cy; i++) {
    sum = x->digits[i] + y->digits[i] + carry;
    if (unlikely(sum >= BASE)) {
      carry = 1;
      sum -= BASE;
    }
    else {
      carry = 0;
    }
    z->digits[i] = sum;
  }
  // propagate the carry
  for (; carry != 0 && i < cx; i++) {
    sum = x->digits[i] + carry;
    if (unlikely(sum >= BASE)) {
      assert_internal(sum==BASE && carry==1);  // can only be at most BASE
      // carry stays 1;
      sum -= BASE;
    }
    else {
      carry = 0;
    }
    z->digits[i] = sum;
  }
  // copy the tail
  if (i < cx && z != x) {
    // memcpy(&z->digits[i], &x->digits[i], (cx - i)*sizeof(digit_t));
    for (; i < cx; i++) {
      z->digits[i] = x->digits[i];
    }
  }
  else {
    i = cx;
  }
  // carry flows into final extra digit?
  if (carry) {
    z->digits[i++] = carry;
  }
  assert_internal(i == bigint_count_(z) || i+1 == bigint_count_(z));
  if (z != x) bigint_decref(x);
  bigint_decref(y);
  return bigint_trim_to(z, i, true /* allow realloc */);
}

/*
static bigint_t* bigint_add_abs_small(bigint_t* x, intptr_t y) {
  assert_internal(y >= 0 && y < BASE);  
  // assert_internal(bigint_sign_(x) == bigint_sign_(y));  
 // ensure x.count >= y.count
  const size_t cx = bigint_count_(x);
 
  // allocate result bigint
  const size_t cz = ((intptr_t)bigint_last_digit_(x) + y + 1 >= BASE ? cx + 1 : cx);
  bigint_t* z = bigint_alloc_reuse_(x, cz); // if z==x, we reused x.
  assert_internal(bigint_count_(z) >= cx);
  digit_t carry = (digit_t)y;
  digit_t sum = 0;
  // add y do the digits of x
  size_t i;
  for (i = 0; carry!=0 && i < cx; i++) {
    sum = x->digits[i] + carry;
    if (unlikely(sum >= BASE)) {
      carry = 1;
      sum -= BASE;
    }
    else {
      carry = 0;
    }
    z->digits[i] = sum;
  }  
  // copy the tail
  if (i < cx && z != x) {
    assert_internal(carry == 0);
    // memcpy(&z->digits[i], &x->digits[i], (cx - i)*sizeof(digit_t));
    for (; i < cx; i++) {
      z->digits[i] = x->digits[i];
    }
  }
  else {
    i = cx;
    // carry flows into final extra digit?
    if (carry) {
      z->digits[i++] = carry;
    }
  }  
  assert_internal(i == bigint_count_(z) || i + 1 == bigint_count_(z));
  if (z != x) bigint_decref(x);
  return bigint_trim_to(z, i, true );
}
*/

/*----------------------------------------------------------------------
  subtract absolute
----------------------------------------------------------------------*/

static bigint_t* bigint_sub_abs(bigint_t* x, bigint_t* y) {  // |x| >= |y|
  assert_internal(bigint_compare_abs_(x, y) >= 0);
  size_t cx = bigint_count_(x);
  size_t cy = bigint_count_(y);
  assert_internal(cx>=cy);
  bigint_t* z = bigint_alloc_reuse_(x, cx);
  //z->is_neg = x->is_neg;
  assert_internal(bigint_count_(z) >= cx);
  digit_t borrow = 0;
  digit_t diff = 0;
  // subtract y digits
  size_t i;
  for (i = 0; i < cy; i++) {
    diff = x->digits[i] - borrow - y->digits[i];
    if (unlikely(diff < 0)) {
      borrow = 1;
      diff += BASE; assert_internal(diff >= 0);
    }
    else {
      borrow = 0;
    }
    z->digits[i] = diff;
  }
  // propagate borrow
  for (; borrow != 0 && i < cx; i++) {
    diff = x->digits[i] - borrow;
    if (unlikely(diff < 0)) {
      // borrow stays 1;
      assert_internal(diff==-1);
      diff += BASE;
    }
    else {
      borrow = 0;
    }
    z->digits[i] = diff;
  }
  assert_internal(borrow==0);  // since x >= y.
  // copy the tail
  if (z != x) {
    // memcpy(&z->digits[i], &x->digits[i], (cx - i)*sizeof(digit_t));
    for (; i <= cx; i++) {
      z->digits[i] = x->digits[i];
    }
    bigint_decref(x);
  }
  bigint_decref(y);
  return bigint_trim(z,true);
}

/*----------------------------------------------------------------------
  Multiply & Sqr. including Karatsuba multiplication
----------------------------------------------------------------------*/

static bigint_t* bigint_mul(bigint_t* x, bigint_t* y) {
  size_t cx = bigint_count_(x);
  size_t cy = bigint_count_(y);
  uint8_t is_neg = (bigint_is_neg_(x) != bigint_is_neg_(y) ? 1 : 0);
  size_t cz = cx+cy;
  bigint_t* z = bigint_alloc_zero(cz,is_neg);  
  int64_t carry = 0;
  int64_t prod = 0;  
  for (size_t i = 0; i < cx; i++) {
    int64_t dx = x->digits[i];
    for (size_t j = 0; j < cy; j++) {
      int64_t dy = y->digits[j];
      prod = (dx * dy) + z->digits[i+j];
      carry = prod/BASE;
      z->digits[i+j]    = (digit_t)(prod - (carry*(int64_t)BASE));
      z->digits[i+j+1] += (digit_t)carry;
    }
  }   
  bigint_decref(x);
  bigint_decref(y);  
  return bigint_trim(z, true);
}

static bigint_t* bigint_mul_small(bigint_t* x, intptr_t y) {
  assert_internal(y > -BASE && y < BASE);
  size_t cx = bigint_count_(x);
  uint8_t is_neg = (bigint_is_neg_(x) && y<0 ? 1 : 0);
  size_t cz = cx+1;
  bigint_t* z = bigint_alloc_reuse_(x, cz);
  if (y < 0) y = -y;
  int64_t carry = 0;
  int64_t prod = 0;
  size_t i;
  for (i = 0; i < cx; i++) {
    prod  = (x->digits[i] * (int64_t)y) + carry;
    carry = prod/BASE;
    z->digits[i] = (digit_t)(prod - (carry*BASE));
  }
  while (carry > 0) {
    assert_internal(i < bigint_count_(z));
    z->digits[i++] = carry % BASE;
    carry /= BASE;
  }
  if (z != x) { bigint_decref(x); }
  if (is_neg && !bigint_is_neg_(z)) { z = bigint_neg(z); }
  return bigint_trim_to(z, i, true);
}

static bigint_t* bigint_sqr(bigint_t* x) {
  bigint_incref(x);
  return bigint_mul(x, x);
}

static bigint_t* bigint_shift_left(bigint_t* x, intptr_t digits) {
  if (digits <= 0) return x;
  size_t cx = x->count;
  bigint_t* z = bigint_alloc_reuse_(x, x->count + digits);
  memmove(&z->digits[digits], &x->digits[0], sizeof(digit_t)*cx);
  memset(&z->digits[0], 0, sizeof(digit_t)*digits);
  if (z != x) bigint_decref(x);
  return z;
}

static bigint_t* bigint_slice(bigint_t* x, size_t lo, size_t hi) {
  if (lo == 0 && bigint_is_unique_(x)) {
    return bigint_trim_to(x, hi, false);
  }
  if (lo >= x->count) lo = x->count;
  if (hi > x->count)  hi = x->count;
  const size_t cz = hi - lo;
  bigint_t* z = bigint_alloc(cz, x->is_neg);
  if (cz==0) {
    z->digits[0] = 0;
    z->count = 1;
    z->extra--;
  }
  else if (lo < x->count) {
    memcpy(&z->digits[0], &x->digits[lo], sizeof(digit_t)*cz);
  }
  return z;
}

static bigint_t* bigint_mul_karatsuba(bigint_t* x, bigint_t* y) {
  intptr_t n = (x->count >= y->count ? x->count : y->count);
  if (n <= 25) return bigint_mul(x, y);
  n = ((n + 1) / 2);

  bigint_t* b = bigint_slice(bigint_dup(x), n, x->count);
  bigint_t* a = bigint_slice(x,0, n);
  bigint_t* d = bigint_slice(bigint_dup(y), n, y->count);
  bigint_t* c = bigint_slice(y, 0, n);

  bigint_t* ac = bigint_mul_karatsuba(bigint_dup(a), bigint_dup(c));
  bigint_t* bd = bigint_mul_karatsuba(bigint_dup(b), bigint_dup(d));
  bigint_t* abcd = bigint_mul_karatsuba( bigint_add(a, b, b->is_neg), 
                                         bigint_add(c, d, d->is_neg));
  bigint_t* p1 = bigint_shift_left(bigint_sub(bigint_sub(abcd, bigint_dup(ac), ac->is_neg), 
                                              bigint_dup(bd), bd->is_neg), n);
  bigint_t* p2 = bigint_shift_left(bd, 2 * n);
  bigint_t* prod = bigint_add(bigint_add(ac, p1, p1->is_neg), p2, p2->is_neg);
  return bigint_trim(prod,true);
}


/*----------------------------------'------------------------------------
  Pow
----------------------------------------------------------------------*/

integer_t integer_pow(integer_t x, integer_t p) {
  if (is_smallint(p)) {
    if (p == integer_from_small(0)) return integer_from_small(1);
  }
  if (is_smallint(x)) {
    if (x == integer_from_small(0)) {
      integer_decref(p);  return integer_from_small(0);
    }
    if (x == integer_from_small(1)) {
      integer_decref(p);  return integer_from_small(1);
    }
    if (x == integer_from_small(-1)) {
      return (integer_is_even(p) ? integer_from_small(1) : integer_from_small(-1));
    }
  }
  integer_incref(p);
  if (integer_signum(p)==-1) {
    integer_decref(p); return integer_from_small(0);
  }
  integer_t y = integer_from_small(1);
  if (is_bigint(p)) {
    while (1) {
      integer_incref(p);
      if (integer_is_odd(p)) {
        integer_incref(x);
        y = integer_mul(y, x);
        p = integer_dec(p);
      }
      if (is_smallint(p)) break;
      p = integer_div(p, integer_from_small(2));
      x = integer_sqr(x);
    }
  }
  assert_internal(is_smallint(p));
  intptr_t i = unbox_int(p);
  while (1) {
    if ((i&1)!=0) {
      integer_incref(x);
      y = integer_mul(y, x);
      i--;
    }
    if (i==0) break;
    i /= 2;
    x = integer_sqr(x);
  }
  integer_decref(x);
  return y;  
}


/*----------------------------------------------------------------------
  Division
----------------------------------------------------------------------*/

static bigint_t* bigint_div_mod_small(bigint_t* x, intptr_t y, intptr_t* pmod) {
  size_t cx = bigint_count_(x);
  // uint8_t is_neg = (bigint_is_neg_(x) != (y<0) ? 1 : 0);
  bigint_t* z = bigint_alloc_reuse_(x, cx);
  int64_t mod = 0;
  for (size_t i = cx; i > 0; i--) {
    int64_t div = mod*BASE + x->digits[i-1];
    int64_t q = div / y;
    mod = div - (q*y);
    z->digits[i-1] = (digit_t)q;
  }
  if (pmod != NULL) {
    *pmod = (intptr_t)mod;
  }
  if (z != x) bigint_decref(x);
  return bigint_trim(z, true);
}
  

static bigint_t* bigint_div_mod(bigint_t* x, bigint_t* y, bigint_t** pmod) {
  size_t cx = bigint_count_(x);
  size_t cy = bigint_count_(y);
  assert_internal(cx >= cy);
  uint8_t is_neg = (bigint_is_neg_(x) != bigint_is_neg_(y) ? 1 : 0);
  bigint_t* z = bigint_alloc_zero(cx - cy + 1, is_neg);
  // normalize
  intptr_t divisorHi = bigint_last_digit_(y);
  intptr_t lambda = ((int64_t)BASE + 2*divisorHi - 1)/(2*divisorHi);
  bigint_t* rem = bigint_mul_small(x, lambda);
  if (rem->count <= cx) { rem = bigint_push(rem, 0); }
  bigint_t* div = bigint_mul_small(y, lambda);
  divisorHi = bigint_last_digit_(div); // todo: check more
  div = bigint_push(div, 0);
  for (intptr_t shift = cx - cy; shift >= 0; shift--) {
    int64_t qd = BASE - 1;
    assert_internal(rem->count > shift + cy);
    if (rem->digits[shift + cy] != divisorHi) {
      assert_internal(rem->count > 1);
      int64_t rem_hi = (rem->digits[shift + cy]*(int64_t)BASE) + rem->digits[shift + cy - 1];
      qd = (rem_hi / divisorHi);
    }
    assert_internal(qd <= (BASE - 1));
    int64_t carry = 0;
    int64_t borrow = 0;
    size_t cd = div->count;
    for (size_t i = 0; i < cd; i++) {
      carry += qd * div->digits[i];
      int64_t q = carry / BASE;
      borrow += rem->digits[shift + i] - (carry - (q*BASE));
      carry = q;
      if (borrow < 0) {
        rem->digits[shift + i] = (digit_t)(borrow + BASE);
        borrow = -1;
      }
      else {
        rem->digits[shift + i] = (digit_t)borrow;
        borrow = 0;
      }
    }
    while (borrow != 0) {
      qd--;
      carry = 0;
      for (size_t i = 0; i < cd; i++) {
        carry += rem->digits[shift + i] - (int64_t)BASE + div->digits[i];
        if (carry < 0) {
          rem->digits[shift + i] = (digit_t)(carry + BASE);
          carry = 0;
        }
        else {
          rem->digits[shift + i] = (digit_t)carry;
          carry = 1;
        }
      }
      borrow += carry;
    }
    z->digits[shift] = (digit_t)qd;
  }
  bigint_decref(div);
  if (pmod != NULL) {
    *pmod = bigint_div_mod_small(rem, lambda, NULL); // denormalize remainder
  }
  else {
    bigint_decref(rem);
  }
  return bigint_trim(z,true);
}


/*----------------------------------------------------------------------
  Addition and substraction
----------------------------------------------------------------------*/

static bigint_t* bigint_add(bigint_t* x, bigint_t* y, bool yneg) {
  if (bigint_is_neg_(x) != yneg) {
    return bigint_sub(x, y, !yneg);
  }
  bigint_t* z;
  if (bigint_count_(x) < bigint_count_(y)) {
    z = bigint_add_abs(y, x);
  }
  else {
    z = bigint_add_abs(x, y);
  }
  assert_internal(bigint_is_unique_(z));
  z->is_neg = yneg;
  return z;
}

static bigint_t* bigint_sub(bigint_t* x, bigint_t* y, bool yneg) {
  if (bigint_is_neg_(x) != yneg) {
    return bigint_add(x, y, !yneg);
  }
  if (bigint_compare_abs_(x,y) >= 0) {
    return bigint_sub_abs(x, y);
  }
  else {
    bigint_t* z = bigint_sub_abs(y, x);
    assert_internal(bigint_is_unique_(z));
    z->is_neg = !yneg;
    return z;
  }
}



/*----------------------------------------------------------------------
  Integer interface
----------------------------------------------------------------------*/

 integer_t integer_neg_generic(integer_t x) {
  assert_internal(is_integer(x));
  bigint_t* bx = integer_to_bigint(x);
  return integer_bigint(bigint_neg(bx));
}

 integer_t integer_sqr_generic(integer_t x) {
  assert_internal(is_integer(x));
  bigint_t* bx = integer_to_bigint(x);
  return integer_bigint(bigint_sqr(bx));
}

 int integer_signum_generic(integer_t x) {
  assert_internal(is_integer(x));
  bigint_t* bx = integer_to_bigint(x);
  int signum = (bx->is_neg ? -1 : ((bx->count==0 && bx->digits[0]==0) ? 0 : 1));
  integer_decref(x);
  return signum;
}

 bool integer_is_even_generic(integer_t x) {
  assert_internal(is_integer(x));
  if (is_smallint(x)) return ((x&0x08)==0);
  bigint_t* bx = integer_to_bigint(x);
  bool even = ((bx->digits[0]&0x1)==0);
  integer_decref(x);
  return even;
}

int integer_cmp_generic(integer_t x, integer_t y) {
  assert_internal(is_integer(x)&&is_integer(y));
  bigint_t* bx = integer_to_bigint(x);
  bigint_t* by = integer_to_bigint(y);
  int sign = bigint_compare_(bx, by);
  bigint_decref(bx);
  bigint_decref(by);
  return sign;
}

integer_t integer_add_generic(integer_t x, integer_t y) { 
  assert_internal(is_integer(x)&&is_integer(y));
  bigint_t* bx = integer_to_bigint(x);
  bigint_t* by = integer_to_bigint(y);
  return integer_bigint(bigint_add(bx, by, by->is_neg));
}

integer_t integer_sub_generic(integer_t x, integer_t y) {
  assert_internal(is_integer(x)&&is_integer(y));
  bigint_t* bx = integer_to_bigint(x);
  bigint_t* by = integer_to_bigint(y);
  return integer_bigint(bigint_sub(bx, by, by->is_neg));
}

static bool use_karatsuba(size_t i, size_t j) {
  return ((0.000012*(i*j) - 0.0025*(i+j)) >= 0);
}

integer_t integer_mul_generic(integer_t x, integer_t y) {
  assert_internal(is_integer(x)&&is_integer(y));
  bigint_t* bx = integer_to_bigint(x);
  bigint_t* by = integer_to_bigint(y);
  bool usek = use_karatsuba(bx->count, by->count);
  return integer_bigint((usek ? bigint_mul_karatsuba(bx,by) : bigint_mul(bx, by)));
}


/*----------------------------------------------------------------------
  Division and modulus
----------------------------------------------------------------------*/

integer_t integer_div_mod_generic(integer_t x, integer_t y, integer_t* mod) {
  assert_internal(is_integer(x)&&is_integer(y));
  if (is_smallint(y)) {
    if (y == integer_from_small(0)) return 0; // raise div-by-zero
    if (y == integer_from_small(1)) {
      if (mod!=NULL) *mod = integer_from_small(0);
      return x;
    }
    if (y == integer_from_small(-1)) {
      if (mod!=NULL) *mod = integer_from_small(0);
      return integer_neg(x);
    }
    intptr_t ay = unbox_int(y);
    bool ay_neg = ay < 0;
    if (ay_neg) ay = -ay;
    if (ay < BASE) {
      // small division
      intptr_t imod;
      bigint_t* bx = integer_to_bigint(x);
      bool     xneg = bigint_is_neg_(bx);
      bigint_t* bz = bigint_div_mod_small(bx, ay, &imod);
      if (xneg) imod = -imod;
      bz->is_neg = (xneg != ay_neg);
      if (mod != NULL) *mod = integer_from_int(imod);
      return integer_bigint(bz);
    }
    // fall through to full division
  }
  bigint_t* bx = integer_to_bigint(x);
  bigint_t* by = integer_to_bigint(y);
  int cmp = bigint_compare_abs_(bx, by);
  if (cmp < 0) {
    if (mod) {
      *mod = x;
    }
    else {
      integer_decref(x);
    }
    integer_decref(y);
    return integer_from_small(0);
  }
  if (cmp==0) {
    if (mod) *mod = integer_from_small(0);
    intptr_t i = (bigint_is_neg_(bx) == bigint_is_neg_(by) ? 1 : -1);
    integer_decref(x);
    integer_decref(y);
    return integer_from_small(i);
  }
  bool qneg = (bigint_is_neg_(bx) != bigint_is_neg_(by));
  bool mneg = bigint_is_neg_(bx);
  bigint_t* bmod = NULL;
  bigint_t* bz = bigint_div_mod(bx, by, (mod!=NULL ? &bmod : NULL));
  bz->is_neg = qneg;
  if (mod!=NULL && bmod != NULL) {
    bmod->is_neg = mneg;
    *mod = integer_bigint(bmod);
  }
  return integer_bigint(bz);
}

integer_t integer_div_generic(integer_t x, integer_t y) {
  return integer_div_mod_generic(x, y, NULL);
}

integer_t integer_mod_generic(integer_t x, integer_t y) {
  integer_t mod = 0;
  integer_t div = integer_div_mod_generic(x, y, &mod);
  integer_decref(div);
  return mod;
}

/*----------------------------------------------------------------------
  Conversion, printing
----------------------------------------------------------------------*/


string_t integer_to_string(integer_t x) {
  if (is_smallint(x)) {
    return int_to_string(unbox_int(x));
  }
  else {
    return bigint_to_string(integer_to_bigint(x));
  }
}

void integer_fprint(FILE* f, integer_t x) {
  string_t s = integer_to_string(x);
  fprintf(f, "%s", string_buf(s));
  string_decref(s);
}

void integer_print(integer_t x) {
  integer_fprint(stdout, x);
}

/*----------------------------------------------------------------------
  Operations for efficient fixed point arithmetic.
  Count trailing zeros, count digits, mul_pow10, div_pow10
----------------------------------------------------------------------*/

// count trailing decimal zeros
static intptr_t int_ctz(intptr_t x) {
  intptr_t count = 0;
  for (; x != 0 && (x%10) == 0; x /= 10) {
    count++;
  }
  return count;
}

static intptr_t bigint_ctz(bigint_t* x) {
  size_t i;
  for (i = 0; i < (x->count-1); i++) {
    if (x->digits[i] != 0) break;
  }
  assert_internal(x->digits[i]!=0);
  intptr_t ctz = (int_ctz(x->digits[i]) + LOG_BASE*i);
  bigint_decref(x);
  return ctz;
}

integer_t integer_ctz(integer_t x) {
  if (is_smallint(x)) {
    return integer_from_small(int_ctz(unbox_int(x)));
  }
  else {
    return integer_from_int(bigint_ctz(integer_to_bigint(x)));
  }
}

static intptr_t count_digits32(uint32_t x) {
  if (x < 10000) { // 1 - 4
    if (x < 100) return (x < 10 ? 1 : 2);
    else return (x < 1000 ? 3 : 4);
  }
  else { // 5 - 9
    if (x < 1000000UL) /*6*/ return (x < 100000 ? 5 : 6);
    else if (x < 100000000UL) /*8*/ return (x < 10000000UL ? 7 : 8);
    else return (x < 1000000000UL /*9*/ ? 9 : 10);
  }  
}

static intptr_t int_count_digits(intptr_t x) {
  // make positive
  uintptr_t u;
  if (x < 0) {
    u = (uintptr_t)(x == INTPTR_MIN ? INTPTR_MAX : -x);  // careful for overflow
  }
  else {
    u = (uintptr_t)x;
  }
  intptr_t count = 0;
  do {
    count += count_digits32(u % BASE);  // count up to 9 digits at a time 
    u /= BASE;
  } while (u > 0);
  return count;
}

static intptr_t bigint_count_digits(bigint_t* x) {
  assert_internal(x->count > 0);
  return count_digits32(x->digits[x->count-1]) + LOG_BASE*(x->count - 1);
}

integer_t integer_count_digits(integer_t x) {
  if (is_smallint(x)) {
    return integer_from_small(int_count_digits(unbox_int(x)));
  }
  else {
    return integer_from_int(bigint_count_digits(integer_to_bigint(x)));
  }
}

static intptr_t powers_of_10[LOG_BASE+1] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };

integer_t integer_mul_pow10(integer_t x, integer_t p) {
  if (p==integer_from_small(0)) {
    integer_decref(p);
    return x;
  }
  if (x==integer_from_small(0)) {
    integer_decref(p); // x is small
    return integer_from_small(0);
  }  
  if (!is_smallint(p)) {
    // TODO: raise error
    return integer_from_small(0);
  }
  intptr_t i = unbox_int(p);

  // negative?
  if (i < 0) {
    return integer_div_pow10(x, integer_from_small(-i));
  }

  // small multiply?
  if (is_smallint(x) && i < LOG_BASE) {
    return integer_mul(x, integer_from_int(powers_of_10[i]));
  }

  // multiply a bigint
  intptr_t large = i / LOG_BASE;  // number of zero digits to shift in
  intptr_t small = i % LOG_BASE;  // small multiply the left over
  bigint_t* b = integer_to_bigint(x);
  if (small > 0) {
    b = bigint_mul_small(b, powers_of_10[small]);
  }
  if (large > 0) {
    size_t bcount = b->count;
    size_t ccount = bcount + large;
    bigint_t* c = bigint_alloc_reuse_(b, ccount);
    memmove(&c->digits[large], &b->digits[0], bcount * sizeof(digit_t)); 
    memset(&c->digits[0], 0, large * sizeof(digit_t));
    assert_internal(c->count == ccount);
    if (b != c) bigint_decref(b);
    b = c;
  }
  return integer_bigint(b);
}


integer_t integer_div_pow10(integer_t x, integer_t p) {
  if (p==integer_from_small(0)) {
    integer_decref(p);
    return x;
  }
  if (x==integer_from_small(0)) {
    integer_decref(p); // x is small
    return integer_from_small(0);
  }
  if (!is_smallint(p)) {
    // TODO: raise error
    return integer_from_small(0);
  }
  intptr_t i = unbox_int(p);

  // negative?
  if (i < 0) {
    return integer_mul_pow10(x, integer_from_small(-i));
  }

  // small divide?
  if (is_smallint(x) && i < LOG_BASE) {
    return integer_div(x, integer_from_int(powers_of_10[i]));
  }

  // divide a bigint
  intptr_t large = i / LOG_BASE;  // number of zero digits to shift out
  intptr_t small = i % LOG_BASE;  // small divide the left over
  bigint_t* b = integer_to_bigint(x);
  size_t bcount = b->count;
  if (large > 0) {
    if (large >= (intptr_t)bcount) { 
      bigint_decref(b);
      return integer_from_small(0);
    }
    size_t ccount = bcount - large;
    if (bigint_is_unique_(b)) {
      memmove(&b->digits[0], &b->digits[large], ccount * sizeof(digit_t));
      b = bigint_trim_to(b, ccount, true);
    }
    else {
      bigint_t* c = bigint_alloc(ccount, b->is_neg);
      memcpy(&c->digits[0], &b->digits[large], bcount * sizeof(digit_t));
      bigint_decref(b);
      b = c;
    }    
  }
  if (small > 0) {
    b = bigint_div_mod_small(b, powers_of_10[small], NULL);
  }
  return integer_bigint(b);
}
