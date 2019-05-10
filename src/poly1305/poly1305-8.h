/**
 * Parts of this software are based on poly1305-donna:
 * https://github.com/floodyberry/poly1305-donna
 *
 * MIT License
 * http://www.opensource.org/licenses/mit-license.php
 */

#if defined(_MSC_VER)
#define BCRYPTO_POLY1305_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define BCRYPTO_POLY1305_NOINLINE __attribute__((noinline))
#else
#define BCRYPTO_POLY1305_NOINLINE
#endif

#define bcrypto_poly1305_block_size 16

typedef struct bcrypto_poly1305_state_internal_t {
  unsigned char buffer[bcrypto_poly1305_block_size];
  size_t leftover;
  unsigned char h[17];
  unsigned char r[17];
  unsigned char pad[17];
  unsigned char final;
} bcrypto_poly1305_state_internal_t;

void
bcrypto_poly1305_init(bcrypto_poly1305_ctx *ctx, const unsigned char key[32]) {
  bcrypto_poly1305_state_internal_t *st =
    (bcrypto_poly1305_state_internal_t *)ctx;
  size_t i;

  st->leftover = 0;

  // h = 0
  for (i = 0; i < 17; i++)
    st->h[i] = 0;

  // r &= 0xffffffc0ffffffc0ffffffc0fffffff
  st->r[0] = key[0] & 0xff;
  st->r[1] = key[1] & 0xff;
  st->r[2] = key[2] & 0xff;
  st->r[3] = key[3] & 0x0f;
  st->r[4] = key[4] & 0xfc;
  st->r[5] = key[5] & 0xff;
  st->r[6] = key[6] & 0xff;
  st->r[7] = key[7] & 0x0f;
  st->r[8] = key[8] & 0xfc;
  st->r[9] = key[9] & 0xff;
  st->r[10] = key[10] & 0xff;
  st->r[11] = key[11] & 0x0f;
  st->r[12] = key[12] & 0xfc;
  st->r[13] = key[13] & 0xff;
  st->r[14] = key[14] & 0xff;
  st->r[15] = key[15] & 0x0f;
  st->r[16] = 0;

  // save pad for later
  for (i = 0; i < 16; i++)
    st->pad[i] = key[i + 16];

  st->pad[16] = 0;

  st->final = 0;
}

static void
bcrypto_poly1305_add(unsigned char h[17], const unsigned char c[17]) {
  unsigned short u;
  unsigned int i;
  for (u = 0, i = 0; i < 17; i++) {
    u += (unsigned short)h[i] + (unsigned short)c[i];
    h[i] = (unsigned char)u & 0xff;
    u >>= 8;
  }
}

static void
bcrypto_poly1305_squeeze(unsigned char h[17], unsigned long hr[17]) {
  unsigned long u;
  unsigned int i;

  u = 0;

  for (i = 0; i < 16; i++) {
    u += hr[i];
    h[i] = (unsigned char)u & 0xff;
    u >>= 8;
  }

  u += hr[16];
  h[16] = (unsigned char)u & 0x03;
  u >>= 2;
  u += (u << 2); /* u *= 5; */

  for (i = 0; i < 16; i++) {
    u += h[i];
    h[i] = (unsigned char)u & 0xff;
    u >>= 8;
  }

  h[16] += (unsigned char)u;
}

static void
bcrypto_poly1305_freeze(unsigned char h[17]) {
  static const unsigned char minusp[17] = {
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xfc
  };

  unsigned char horig[17], negative;
  unsigned int i;

  // compute h + -p
  for (i = 0; i < 17; i++)
    horig[i] = h[i];

  bcrypto_poly1305_add(h, minusp);

  // select h if h < p, or h + -p if h >= p
  negative = -(h[16] >> 7);
  for (i = 0; i < 17; i++)
    h[i] ^= negative & (horig[i] ^ h[i]);
}

static void
bcrypto_poly1305_blocks(bcrypto_poly1305_state_internal_t *st,
                        const unsigned char *m,
                        size_t bytes) {
  const unsigned char hibit = st->final ^ 1; // 1 << 128

  while (bytes >= bcrypto_poly1305_block_size) {
    unsigned long hr[17], u;
    unsigned char c[17];
    unsigned int i, j;

    // h += m
    for (i = 0; i < 16; i++)
      c[i] = m[i];

    c[16] = hibit;
    bcrypto_poly1305_add(st->h, c);

    // h *= r
    for (i = 0; i < 17; i++) {
      u = 0;

      for (j = 0; j <= i ; j++)
        u += (unsigned short)st->h[j] * st->r[i - j];

      for (j = i + 1; j < 17; j++) {
        unsigned long v = (unsigned short)st->h[j] * st->r[i + 17 - j];
        v = ((v << 8) + (v << 6)); // v *= (5 << 6);
        u += v;
      }

      hr[i] = u;
    }

    // (partial) h %= p
    bcrypto_poly1305_squeeze(st->h, hr);

    m += bcrypto_poly1305_block_size;
    bytes -= bcrypto_poly1305_block_size;
  }
}

BCRYPTO_POLY1305_NOINLINE void
bcrypto_poly1305_finish(bcrypto_poly1305_ctx *ctx, unsigned char mac[16]) {
  bcrypto_poly1305_state_internal_t *st =
    (bcrypto_poly1305_state_internal_t *)ctx;
  size_t i;

  // process the remaining block
  if (st->leftover) {
    size_t i = st->leftover;

    st->buffer[i++] = 1;

    for (; i < bcrypto_poly1305_block_size; i++)
      st->buffer[i] = 0;

    st->final = 1;
    bcrypto_poly1305_blocks(st, st->buffer, bcrypto_poly1305_block_size);
  }

  // fully reduce h
  bcrypto_poly1305_freeze(st->h);

  // h = (h + pad) % (1 << 128)
  bcrypto_poly1305_add(st->h, st->pad);

  for (i = 0; i < 16; i++)
    mac[i] = st->h[i];

  // zero out the state
  for (i = 0; i < 17; i++)
    st->h[i] = 0;

  for (i = 0; i < 17; i++)
    st->r[i] = 0;

  for (i = 0; i < 17; i++)
    st->pad[i] = 0;
}
