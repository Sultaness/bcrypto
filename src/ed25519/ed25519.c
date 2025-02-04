/*
  Public domain by Andrew M. <liquidsun@gmail.com>

  Ed25519 reference implementation using Ed25519-donna
*/

#include "ed25519-donna.h"
#include "ed25519.h"
#include "ed25519-randombytes.h"
#include "ed25519-hash.h"

static const unsigned char ED25519_PREFIX[] =
  "SigEd25519 no Ed25519 collisions";

/*
  Generates a (extsk[0..31]) and aExt (extsk[32..63])
*/

DONNA_INLINE static void
bcrypto_ed25519_extsk(hash_512bits extsk, const bcrypto_ed25519_privkey_t sk) {
  bcrypto_ed25519_hash(extsk, sk, 32);
  extsk[0] &= 248;
  extsk[31] &= 127;
  extsk[31] |= 64;
}

static void
bcrypto_ed25519_hprefix(
  bcrypto_ed25519_hash_t *hctx,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len
) {
  if (ph != -1) {
    bcrypto_ed25519_hash_update(hctx, ED25519_PREFIX,
                                sizeof(ED25519_PREFIX) - 1);

    unsigned char slab[2] = {
      (unsigned char)ph,
      (unsigned char)ctx_len
    };

    bcrypto_ed25519_hash_update(hctx, &slab[0], 2);
    bcrypto_ed25519_hash_update(hctx, ctx, ctx_len);
  }
}

static void
bcrypto_ed25519_hram(
  hash_512bits hram,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len,
  const bcrypto_ed25519_sig_t RS,
  const bcrypto_ed25519_pubkey_t pk,
  const unsigned char *m,
  size_t mlen
) {
  bcrypto_ed25519_hash_t hctx;
  bcrypto_ed25519_hash_init(&hctx);
  bcrypto_ed25519_hprefix(&hctx, ph, ctx, ctx_len);
  bcrypto_ed25519_hash_update(&hctx, RS, 32);
  bcrypto_ed25519_hash_update(&hctx, pk, 32);
  bcrypto_ed25519_hash_update(&hctx, m, mlen);
  bcrypto_ed25519_hash_final(&hctx, hram);
}

#include "ed25519-donna-batchverify.h"

int
bcrypto_ed25519_pubkey_from_scalar(
  bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t sk
) {
  bignum256modm a;
  ge25519 ALIGN(16) A;

  /* A = aB */
  expand256_modm(a, sk, 32);
  ge25519_scalarmult_base_niels(&A, ge25519_niels_base_multiples, a);

  if (!ge25519_pack_safe(pk, &A))
    return 0;

  return 1;
}

int
bcrypto_ed25519_pubkey_create(
  bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_privkey_t sk
) {
  hash_512bits extsk;
  bcrypto_ed25519_extsk(extsk, sk);
  return bcrypto_ed25519_pubkey_from_scalar(pk, extsk);
}

int
bcrypto_ed25519_verify(
  const unsigned char *m,
  size_t mlen,
  const bcrypto_ed25519_pubkey_t pk,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len,
  const bcrypto_ed25519_sig_t RS
) {
  ge25519 ALIGN(16) R, A;
  hash_512bits hash;
  bignum256modm hram, S;
  unsigned char checkR[32];

  if ((RS[63] & 224) || !ge25519_unpack_negative_vartime(&A, pk))
    return 0;

  /* hram = H(R,A,m) */
  bcrypto_ed25519_hram(hash, ph, ctx, ctx_len, RS, pk, m, mlen);
  expand256_modm(hram, hash, 64);

  /* S */
  expand256_modm(S, RS + 32, 32);

  /* SB - H(R,A,m)A */
  ge25519_double_scalarmult_vartime(&R, &A, hram, S);

  if (!ge25519_pack_safe(checkR, &R))
    return 0;

  /* check that R = SB - H(R,A,m)A */
  return bcrypto_ed25519_equal(RS, checkR, 32) ? 1 : 0;
}

int
bcrypto_ed25519_pubkey_verify(const bcrypto_ed25519_pubkey_t pk) {
  ge25519 ALIGN(16) A;

  if (!ge25519_unpack_vartime(&A, pk))
    return 0;

  return 1;
}

/*
  Fast Curve25519 basepoint scalar multiplication
*/

void
bcrypto_x25519_pubkey_create(
  bcrypto_x25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t e
) {
  bcrypto_ed25519_scalar_t ec;
  bignum256modm s;
  bignum25519 ALIGN(16) yplusz, zminusy;
  ge25519 ALIGN(16) p;
  size_t i;

  /* clamp */
  for (i = 0; i < 32; i++)
    ec[i] = e[i];

  ec[0] &= 248;
  ec[31] &= 127;
  ec[31] |= 64;

  expand_raw256_modm(s, ec);

  /* scalar * basepoint */
  ge25519_scalarmult_base_niels(&p, ge25519_niels_base_multiples, s);

  /* u = (y + z) / (z - y) */
  curve25519_add(yplusz, p.y, p.z);
  curve25519_sub(zminusy, p.z, p.y);
  curve25519_recip(zminusy, zminusy);
  curve25519_mul(yplusz, yplusz, zminusy);
  curve25519_contract(pk, yplusz);
}

void
bcrypto_ed25519_privkey_expand(
  unsigned char out[64],
  const bcrypto_ed25519_privkey_t sk
) {
  bcrypto_ed25519_extsk(out, sk);
}

void
bcrypto_ed25519_privkey_convert(
  bcrypto_ed25519_scalar_t out,
  const bcrypto_ed25519_privkey_t sk
) {
  hash_512bits extsk;
  bcrypto_ed25519_extsk(extsk, sk);
  memcpy(out, extsk, 32);
}

int
bcrypto_ed25519_pubkey_convert(
  bcrypto_x25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk
) {
  bignum25519 ALIGN(16) yplusz, zminusy;
  ge25519 ALIGN(16) p;

  /* ed25519 pubkey -> ed25519 point */
  if (!ge25519_unpack_vartime(&p, pk))
    return 0;

  /* ed25519 point -> x25519 point */
  curve25519_add(yplusz, p.y, p.z);
  curve25519_sub(zminusy, p.z, p.y);
  curve25519_recip(zminusy, zminusy);
  curve25519_mul(yplusz, yplusz, zminusy);

  /* output point (little-endian u coord) */
  curve25519_contract(out, yplusz);

  return 1;
}

int
bcrypto_ed25519_pubkey_deconvert(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_x25519_pubkey_t pk,
  int sign
) {
  bignum25519 ALIGN(16) x, z, xminusz, xplusz;

  curve25519_expand(x, pk);
  curve25519_set_word(z, 1);
  curve25519_sub(xminusz, x, z);
  curve25519_add(xplusz, x, z);
  curve25519_recip(xplusz, xplusz);
  curve25519_mul(x, xminusz, xplusz);

  curve25519_contract(out, x);

  if (sign)
    out[31] |= 0x80;

  return 1;
}

int
bcrypto_ed25519_derive_with_scalar(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t sk
) {
  bignum256modm k;
  ge25519 ALIGN(16) s, p;

  expand_raw256_modm(k, sk);

  if (!ge25519_unpack_vartime(&p, pk))
    return 0;

  ge25519_scalarmult_vartime(&s, &p, k);

  if (!ge25519_pack_safe(out, &s))
    return 0;

  return 1;
}

int
bcrypto_ed25519_derive(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_privkey_t sk
) {
  hash_512bits extsk;
  bcrypto_ed25519_extsk(extsk, sk);
  return bcrypto_ed25519_derive_with_scalar(out, pk, extsk);
}

int
bcrypto_ed25519_exchange_with_scalar(
  bcrypto_x25519_pubkey_t out,
  const bcrypto_x25519_pubkey_t xpk,
  const bcrypto_ed25519_scalar_t sk
) {
  bcrypto_ed25519_scalar_t k;
  bignum25519 ALIGN(16) nd, x1, x2, z2, x3, z3, t1, t2;

  int swap = 0;
  size_t i;
  int t, b;

  /* clamp */
  for (i = 0; i < 32; i++)
    k[i] = sk[i];

  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;

  curve25519_set_word(nd, 121666);

  curve25519_expand(x1, xpk);
  curve25519_set_word(x2, 1);
  curve25519_set_word(z2, 0);
  curve25519_copy(x3, x1);
  curve25519_set_word(z3, 1);

  for (t = 255 - 1; t >= 0; t--) {
    b = (k[t >> 3] >> (t & 7)) & 1;

    swap ^= b;

    curve25519_swap_conditional(x2, x3, swap);
    curve25519_swap_conditional(z2, z3, swap);

    swap = b;

    curve25519_sub(t1, x3, z3);
    curve25519_sub(t2, x2, z2);
    curve25519_add(x2, x2, z2);
    curve25519_add(z2, x3, z3);
    curve25519_mul(z3, t1, x2);
    curve25519_mul(z2, z2, t2);
    curve25519_square(t1, t2);
    curve25519_square(t2, x2);
    curve25519_add(x3, z3, z2);
    curve25519_sub(z2, z3, z2);
    curve25519_mul(x2, t2, t1);
    curve25519_sub(t2, t2, t1);
    curve25519_square(z2, z2);
    curve25519_mul(z3, t2, nd);
    curve25519_square(x3, x3);
    curve25519_add(t1, t1, z3);
    curve25519_mul(z3, x1, z2);
    curve25519_mul(z2, t2, t1);
  }

  /* Finish. */
  curve25519_swap_conditional(x2, x3, swap);
  curve25519_swap_conditional(z2, z3, swap);

  curve25519_recip(z2, z2);
  curve25519_mul(x1, x2, z2);

  if (curve25519_is_zero(x1))
    return 0;

  curve25519_contract(out, x1);

  return 1;
}

int
bcrypto_ed25519_exchange(
  bcrypto_x25519_pubkey_t out,
  const bcrypto_x25519_pubkey_t xpk,
  const bcrypto_ed25519_privkey_t sk
) {
  hash_512bits extsk;
  bcrypto_ed25519_extsk(extsk, sk);
  return bcrypto_ed25519_exchange_with_scalar(out, xpk, extsk);
}

int
bcrypto_ed25519_scalar_tweak_add(
  bcrypto_ed25519_scalar_t out,
  const bcrypto_ed25519_scalar_t sk,
  const bcrypto_ed25519_scalar_t tweak
) {
  bignum256modm k, t;

  expand256_modm(k, sk, 32);
  expand256_modm(t, tweak, 32);

  add256_modm(k, k, t);

  if (iszero256_modm_batch(k))
    return 0;

  contract256_modm(out, k);

  return 1;
}

int
bcrypto_ed25519_scalar_tweak_mul(
  bcrypto_ed25519_scalar_t out,
  const bcrypto_ed25519_scalar_t sk,
  const bcrypto_ed25519_scalar_t tweak
) {
  bignum256modm k, t;

  expand256_modm(k, sk, 32);
  expand256_modm(t, tweak, 32);

  mul256_modm(k, k, t);

  if (iszero256_modm_batch(k))
    return 0;

  contract256_modm(out, k);

  return 1;
}

void
bcrypto_ed25519_scalar_reduce(
  bcrypto_ed25519_scalar_t out,
  const bcrypto_ed25519_scalar_t sk
) {
  bignum256modm k;
  expand256_modm(k, sk, 32);
  contract256_modm(out, k);
}

int
bcrypto_ed25519_scalar_negate(
  bcrypto_ed25519_scalar_t out,
  const bcrypto_ed25519_scalar_t sk
) {
  bignum256modm k;

  expand256_modm(k, sk, 32);
  negate256_modm(k, k);

  contract256_modm(out, k);

  return 1;
}

int
bcrypto_ed25519_scalar_invert(
  bcrypto_ed25519_scalar_t out,
  const bcrypto_ed25519_scalar_t sk
) {
  bignum256modm k;

  expand256_modm(k, sk, 32);

  if (iszero256_modm_batch(k))
    return 0;

  recip256_modm(k, k);

  if (iszero256_modm_batch(k))
    return 0;

  contract256_modm(out, k);

  return 1;
}

int
bcrypto_ed25519_pubkey_tweak_add(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t tweak
) {
  ge25519 ALIGN(16) T, k;
  bignum256modm t;

  if (!ge25519_unpack_vartime(&k, pk))
    return 0;

  expand256_modm(t, tweak, 32);

  ge25519_scalarmult_base_niels(&T, ge25519_niels_base_multiples, t);

  ge25519_add(&k, &k, &T);

  if (!ge25519_pack_safe(out, &k))
    return 0;

  return 1;
}

int
bcrypto_ed25519_pubkey_tweak_mul(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t tweak
) {
  ge25519 ALIGN(16) T, k;
  bignum256modm t;

  if (!ge25519_unpack_vartime(&k, pk))
    return 0;

  expand256_modm(t, tweak, 32);

  ge25519_scalarmult_vartime(&T, &k, t);

  if (!ge25519_pack_safe(out, &T))
    return 0;

  return 1;
}

int
bcrypto_ed25519_pubkey_add(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk1,
  const bcrypto_ed25519_pubkey_t pk2
) {
  ge25519 ALIGN(16) k1, k2;

  if (!ge25519_unpack_vartime(&k1, pk1))
    return 0;

  if (!ge25519_unpack_vartime(&k2, pk2))
    return 0;

  ge25519_add(&k1, &k1, &k2);

  if (!ge25519_pack_safe(out, &k1))
    return 0;

  return 1;
}

int
bcrypto_ed25519_pubkey_combine(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t *pks,
  size_t length
) {
  ge25519 ALIGN(16) k1, k2;
  size_t i = 1;

  if (length == 0)
    return 0;

  if (!ge25519_unpack_vartime(&k1, pks[0]))
    return 0;

  for (; i < length; i++) {
    if (!ge25519_unpack_vartime(&k2, pks[i]))
      return 0;

    ge25519_add(&k1, &k1, &k2);
  }

  if (!ge25519_pack_safe(out, &k1))
    return 0;

  return 1;
}

int
bcrypto_ed25519_pubkey_negate(
  bcrypto_ed25519_pubkey_t out,
  const bcrypto_ed25519_pubkey_t pk
) {
  ge25519 ALIGN(16) k;

  if (!ge25519_unpack_vartime(&k, pk))
    return 0;

  ge25519_neg(&k, &k);

  if (!ge25519_pack_safe(out, &k))
    return 0;

  return 1;
}

int
bcrypto_ed25519_sign_with_scalar(
  bcrypto_ed25519_sig_t RS,
  const unsigned char *m,
  size_t mlen,
  const uint8_t extsk[64],
  const bcrypto_ed25519_pubkey_t pk,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len
) {
  bcrypto_ed25519_hash_t hctx;
  bignum256modm r, S, a;
  ge25519 ALIGN(16) R;
  hash_512bits hashr, hram;

  /* r = H(aExt[32..64], m) */
  bcrypto_ed25519_hash_init(&hctx);
  bcrypto_ed25519_hprefix(&hctx, ph, ctx, ctx_len);
  bcrypto_ed25519_hash_update(&hctx, extsk + 32, 32);
  bcrypto_ed25519_hash_update(&hctx, m, mlen);
  bcrypto_ed25519_hash_final(&hctx, hashr);
  expand256_modm(r, hashr, 64);

  /* R = rB */
  ge25519_scalarmult_base_niels(&R, ge25519_niels_base_multiples, r);

  if (!ge25519_pack_safe(RS, &R))
    return 0;

  /* S = H(R,A,m).. */
  bcrypto_ed25519_hram(hram, ph, ctx, ctx_len, RS, pk, m, mlen);
  expand256_modm(S, hram, 64);

  /* S = H(R,A,m)a */
  expand256_modm(a, extsk, 32);
  mul256_modm(S, S, a);

  /* S = (r + H(R,A,m)a) */
  add256_modm(S, S, r);

  /* S = (r + H(R,A,m)a) mod L */
  contract256_modm(RS + 32, S);

  return 1;
}

int
bcrypto_ed25519_sign(
  bcrypto_ed25519_sig_t RS,
  const unsigned char *m,
  size_t mlen,
  const bcrypto_ed25519_privkey_t sk,
  const bcrypto_ed25519_pubkey_t pk,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len
) {
  hash_512bits extsk;
  bcrypto_ed25519_extsk(extsk, sk);
  return bcrypto_ed25519_sign_with_scalar(RS, m, mlen, extsk, pk, ph, ctx, ctx_len);
}

int
bcrypto_ed25519_sign_tweak_add(
  bcrypto_ed25519_sig_t RS,
  const unsigned char *m,
  size_t mlen,
  const bcrypto_ed25519_privkey_t sk,
  const bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t tweak,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len
) {
  hash_512bits extsk, prefix;
  bcrypto_ed25519_pubkey_t tk;
  bcrypto_ed25519_hash_t hctx;

  bcrypto_ed25519_extsk(extsk, sk);

  if (!bcrypto_ed25519_scalar_tweak_add(extsk, extsk, tweak))
    return 0;

  bcrypto_ed25519_hash_init(&hctx);
  bcrypto_ed25519_hash_update(&hctx, extsk + 32, 32);
  bcrypto_ed25519_hash_update(&hctx, tweak, 32);
  bcrypto_ed25519_hash_final(&hctx, prefix);
  memcpy(extsk + 32, prefix, 32);

  if (!bcrypto_ed25519_pubkey_tweak_add(tk, pk, tweak))
    return 0;

  return bcrypto_ed25519_sign_with_scalar(RS, m, mlen, extsk, tk, ph, ctx, ctx_len);
}

int
bcrypto_ed25519_sign_tweak_mul(
  bcrypto_ed25519_sig_t RS,
  const unsigned char *m,
  size_t mlen,
  const bcrypto_ed25519_privkey_t sk,
  const bcrypto_ed25519_pubkey_t pk,
  const bcrypto_ed25519_scalar_t tweak,
  int ph,
  const unsigned char *ctx,
  size_t ctx_len
) {
  hash_512bits extsk, prefix;
  bcrypto_ed25519_pubkey_t tk;
  bcrypto_ed25519_hash_t hctx;

  bcrypto_ed25519_extsk(extsk, sk);

  if (!bcrypto_ed25519_scalar_tweak_mul(extsk, extsk, tweak))
    return 0;

  bcrypto_ed25519_hash_init(&hctx);
  bcrypto_ed25519_hash_update(&hctx, extsk + 32, 32);
  bcrypto_ed25519_hash_update(&hctx, tweak, 32);
  bcrypto_ed25519_hash_final(&hctx, prefix);
  memcpy(extsk + 32, prefix, 32);

  if (!bcrypto_ed25519_pubkey_tweak_mul(tk, pk, tweak))
    return 0;

  return bcrypto_ed25519_sign_with_scalar(RS, m, mlen, extsk, tk, ph, ctx, ctx_len);
}
