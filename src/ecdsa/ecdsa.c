#include "../compat.h"

#ifdef BCRYPTO_HAS_ECDSA

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "ecdsa.h"

#include "openssl/ecdsa.h"
#include "openssl/objects.h"
#include "openssl/x509.h"
#include "../random/random.h"

/*
 * Helpers
 */

static int
bcrypto_ecdsa_curve(const char *name) {
  int type = -1;

  if (name == NULL)
    return type;

  if (strcmp(name, "P192") == 0)
    type = NID_X9_62_prime192v1;
  else if (strcmp(name, "P224") == 0)
    type = NID_secp224r1;
  else if (strcmp(name, "P256") == 0)
    type = NID_X9_62_prime256v1;
  else if (strcmp(name, "P384") == 0)
    type = NID_secp384r1;
  else if (strcmp(name, "P521") == 0)
    type = NID_secp521r1;
  else if (strcmp(name, "SECP256K1") == 0)
    type = NID_secp256k1;

  return type;
}

static int
bcrypto_ecdsa_hash_type(int type) {
  switch (type) {
    case NID_X9_62_prime192v1:
    case NID_secp224r1:
    case NID_X9_62_prime256v1:
    case NID_secp256k1:
      return NID_sha256;
    case NID_secp384r1:
      return NID_sha384;
    case NID_secp521r1:
      return NID_sha512;
  }
  return -1;
}

static int
bcrypto_ecdsa_has_schnorr(int type) {
  switch (type) {
    case NID_X9_62_prime192v1:
    case NID_X9_62_prime256v1:
    case NID_secp384r1:
    case NID_secp521r1:
    case NID_secp256k1:
      return 1;
  }
  return 0;
}

static int
bcrypto_ecdsa_valid_scalar(bcrypto_ecdsa_t *ec, const uint8_t *scalar) {
  if (scalar == NULL)
    return 0;

  return memcmp(scalar, ec->zero, ec->scalar_size) != 0
      && memcmp(scalar, ec->order, ec->scalar_size) < 0;
}

static int
bcrypto_ecdsa_valid_point(bcrypto_ecdsa_t *ec,
                          const uint8_t *raw,
                          size_t raw_len) {
  if (raw == NULL)
    return 0;

  if (raw_len < 1 + ec->size)
    return 0;

  switch (raw[0]) {
    case 0x02:
    case 0x03:
      return raw_len == 1 + ec->size;
    case 0x04:
      return raw_len == 1 + ec->size * 2;
    case 0x06:
    case 0x07:
      return raw_len == 1 + ec->size * 2
          && (raw[0] & 1) == (raw[raw_len - 1] & 1);
    default:
      return 0;
  }
}

/*
 * Public Key
 */

static int
bcrypto_ecdsa_pubkey_from_ec_point(bcrypto_ecdsa_t *ec,
                                   bcrypto_ecdsa_pubkey_t *pub,
                                   const EC_POINT *point);

void
bcrypto_ecdsa_pubkey_encode(bcrypto_ecdsa_t *ec,
                            uint8_t *out,
                            size_t *out_len,
                            const bcrypto_ecdsa_pubkey_t *pub,
                            int compress) {
  if (compress) {
    out[0] = 0x02 | (pub->y[ec->size - 1] & 1);
    memcpy(&out[1], &pub->x[0], ec->size);
    *out_len = 1 + ec->size;
  } else {
    out[0] = 0x04;
    memcpy(&out[1], &pub->x[0], ec->size);
    memcpy(&out[1 + ec->size], &pub->y[0], ec->size);
    *out_len = 1 + ec->size * 2;
  }
}

int
bcrypto_ecdsa_pubkey_decode(bcrypto_ecdsa_t *ec,
                            bcrypto_ecdsa_pubkey_t *pub,
                            const uint8_t *raw,
                            size_t raw_len) {
  if (!bcrypto_ecdsa_valid_point(ec, raw, raw_len))
    return 0;

  if (!EC_POINT_oct2point(ec->group, ec->point, raw, raw_len, ec->ctx))
    return 0;

  if (raw[0] >= 0x04) {
    if (EC_POINT_is_on_curve(ec->group, ec->point, ec->ctx) <= 0)
      return 0;
  }

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, pub, ec->point))
    return 0;

  return 1;
}

static EC_POINT *
bcrypto_ecdsa_pubkey_to_ec_point(bcrypto_ecdsa_t *ec,
                                 const bcrypto_ecdsa_pubkey_t *pub) {
  EC_POINT *point = NULL;
  uint8_t raw[BCRYPTO_ECDSA_MAX_PUB_SIZE];
  size_t raw_len = 0;

  point = EC_POINT_new(ec->group);

  if (point == NULL)
    goto fail;

  bcrypto_ecdsa_pubkey_encode(ec, raw, &raw_len, pub, 0);

  if (!EC_POINT_oct2point(ec->group, point, raw, raw_len, ec->ctx))
    goto fail;

  return point;

fail:
  if (point != NULL)
    EC_POINT_free(point);

  return NULL;
}

static EC_KEY *
bcrypto_ecdsa_pubkey_to_ec_key(bcrypto_ecdsa_t *ec,
                               const bcrypto_ecdsa_pubkey_t *pub) {
  EC_KEY *key = NULL;
  uint8_t raw[BCRYPTO_ECDSA_MAX_PUB_SIZE];
  size_t raw_len = 0;

  key = EC_KEY_new_by_curve_name(ec->type);

  if (key == NULL)
    goto fail;

  bcrypto_ecdsa_pubkey_encode(ec, raw, &raw_len, pub, 0);

  if (!EC_KEY_oct2key(key, raw, raw_len, ec->ctx))
    goto fail;

  return key;

fail:
  if (key != NULL)
    EC_KEY_free(key);

  return NULL;
}

static int
bcrypto_ecdsa_pubkey_from_ec_point(bcrypto_ecdsa_t *ec,
                                   bcrypto_ecdsa_pubkey_t *pub,
                                   const EC_POINT *point) {
  point_conversion_form_t form = POINT_CONVERSION_UNCOMPRESSED;
  uint8_t raw[BCRYPTO_ECDSA_MAX_PUB_SIZE];
  size_t raw_len = 0;

  if (EC_POINT_is_at_infinity(ec->group, point))
    return 0;

  raw_len = EC_POINT_point2oct(ec->group, point, form, raw,
                               BCRYPTO_ECDSA_MAX_PUB_SIZE, ec->ctx);

  if (raw_len != 1 + ec->size * 2)
    return 0;

  assert(raw[0] == 0x04);

  memcpy(&pub->x[0], &raw[1], ec->size);
  memcpy(&pub->y[0], &raw[1 + ec->size], ec->size);

  return 1;
}

static int
bcrypto_ecdsa_pubkey_from_ec_key(bcrypto_ecdsa_t *ec,
                                 bcrypto_ecdsa_pubkey_t *pub,
                                 const EC_KEY *key) {
  const EC_POINT *point = EC_KEY_get0_public_key(key);
  assert(point != NULL);

  return bcrypto_ecdsa_pubkey_from_ec_point(ec, pub, point);
}

static int
bcrypto_ecdsa_pubkey_equals(bcrypto_ecdsa_t *ec,
                            const bcrypto_ecdsa_pubkey_t *a,
                            const bcrypto_ecdsa_pubkey_t *b) {
  return memcmp(a->x, b->x, ec->size) == 0
      && memcmp(a->y, b->y, ec->size) == 0;
}

/*
 * Signature
 */

void
bcrypto_ecdsa_sig_encode(bcrypto_ecdsa_t *ec,
                         uint8_t *out,
                         const bcrypto_ecdsa_sig_t *sig) {
  memcpy(&out[0], &sig->r[0], ec->scalar_size);
  memcpy(&out[ec->scalar_size], &sig->s[0], ec->scalar_size);
}

int
bcrypto_ecdsa_sig_decode(bcrypto_ecdsa_t *ec,
                         bcrypto_ecdsa_sig_t *sig,
                         const uint8_t *raw) {
  memcpy(&sig->r[0], &raw[0], ec->scalar_size);
  memcpy(&sig->s[0], &raw[ec->scalar_size], ec->scalar_size);

  return memcmp(sig->r, ec->zero, ec->scalar_size) != 0
      && memcmp(sig->s, ec->zero, ec->scalar_size) != 0
      && memcmp(sig->r, ec->order, ec->scalar_size) < 0
      && memcmp(sig->s, ec->order, ec->scalar_size) < 0;
}

int
bcrypto_ecdsa_sig_encode_der(bcrypto_ecdsa_t *ec,
                             uint8_t *out,
                             size_t *out_len,
                             const bcrypto_ecdsa_sig_t *sig) {
  assert(ec->scalar_size < 0x7d);

  uint8_t r[1 + BCRYPTO_ECDSA_MAX_SCALAR_SIZE] = {0};
  uint8_t *rp = (uint8_t *)&sig->r[0];
  size_t rlen = ec->scalar_size;
  uint8_t s[1 + BCRYPTO_ECDSA_MAX_SCALAR_SIZE] = {0};
  uint8_t *sp = (uint8_t *)&sig->s[0];
  size_t slen = ec->scalar_size;

  while (rlen > 1 && rp[0] == 0)
    rlen--, rp++;

  while (slen > 1 && sp[0] == 0)
    slen--, sp++;

  size_t rn = (rp[0] & 0x80) ? 1 : 0;
  size_t sn = (sp[0] & 0x80) ? 1 : 0;

  memcpy(r + rn, rp, rlen);
  memcpy(s + sn, sp, slen);

  rlen += rn;
  slen += sn;

  size_t seq = 2 + rlen + 2 + slen;
  size_t wide = seq >= 0x80 ? 1 : 0;
  size_t len = 2 + wide + seq;

  if (len > *out_len)
    return 0;

  *(out++) = 0x30;

  if (wide)
    *(out++) = 0x81;

  *(out++) = seq;
  *(out++) = 0x02;
  *(out++) = rlen;

  memcpy(out, r, rlen);
  out += rlen;

  *(out++) = 0x02;
  *(out++) = slen;

  memcpy(out, s, slen);
  out += slen;

  *out_len = len;

  return 1;
}

int
bcrypto_ecdsa_sig_decode_der(bcrypto_ecdsa_t *ec,
                             bcrypto_ecdsa_sig_t *sig,
                             const uint8_t *raw,
                             size_t raw_len) {
  size_t rpos, rlen, spos, slen;
  size_t pos = 0;
  size_t lenbyte;
  int overflow = 0;

  memset(sig->r, 0x00, ec->scalar_size);
  memset(sig->s, 0x00, ec->scalar_size);

  /* Sequence tag byte */
  if (pos == raw_len || raw[pos] != 0x30)
    return 0;

  pos++;

  /* Sequence length bytes */
  if (pos == raw_len)
    return 0;

  lenbyte = raw[pos++];

  if (lenbyte & 0x80) {
    lenbyte -= 0x80;

    if (pos + lenbyte > raw_len)
      return 0;

    pos += lenbyte;
  }

  /* Integer tag byte for R */
  if (pos == raw_len || raw[pos] != 0x02)
    return 0;

  pos++;

  /* Integer length for R */
  if (pos == raw_len)
    return 0;

  lenbyte = raw[pos++];

  if (lenbyte & 0x80) {
    lenbyte -= 0x80;

    if (pos + lenbyte > raw_len)
      return 0;

    while (lenbyte > 0 && raw[pos] == 0) {
      pos++;
      lenbyte--;
    }

    if (lenbyte >= sizeof(size_t))
      return 0;

    rlen = 0;

    while (lenbyte > 0) {
      rlen = (rlen << 8) + raw[pos];
      pos++;
      lenbyte--;
    }
  } else {
    rlen = lenbyte;
  }

  if (rlen > raw_len - pos)
    return 0;

  rpos = pos;
  pos += rlen;

  /* Integer tag byte for S */
  if (pos == raw_len || raw[pos] != 0x02)
    return 0;

  pos++;

  /* Integer length for S */
  if (pos == raw_len)
    return 0;

  lenbyte = raw[pos++];

  if (lenbyte & 0x80) {
    lenbyte -= 0x80;

    if (pos + lenbyte > raw_len)
      return 0;

    while (lenbyte > 0 && raw[pos] == 0) {
      pos++;
      lenbyte--;
    }

    if (lenbyte >= sizeof(size_t))
      return 0;

    slen = 0;

    while (lenbyte > 0) {
      slen = (slen << 8) + raw[pos];
      pos++;
      lenbyte--;
    }
  } else {
    slen = lenbyte;
  }

  if (slen > raw_len - pos)
    return 0;

  spos = pos;
  pos += slen;

  /* Ignore leading zeroes in R */
  while (rlen > 0 && raw[rpos] == 0) {
    rlen--;
    rpos++;
  }

  /* Copy R value */
  if (rlen > ec->scalar_size)
    overflow = 1;
  else
    memcpy(sig->r + ec->scalar_size - rlen, raw + rpos, rlen);

  /* Ignore leading zeroes in S */
  while (slen > 0 && raw[spos] == 0) {
    slen--;
    spos++;
  }

  /* Copy S value */
  if (slen > ec->scalar_size)
    overflow = 1;
  else
    memcpy(sig->s + ec->scalar_size - slen, raw + spos, slen);

  if (!overflow) {
    overflow = memcmp(sig->r, ec->order, ec->scalar_size) >= 0
            || memcmp(sig->s, ec->order, ec->scalar_size) >= 0;
  }

  if (overflow) {
    memset(sig->r, 0x00, ec->scalar_size);
    memset(sig->s, 0x00, ec->scalar_size);
  }

  return 1;
}

static ECDSA_SIG *
bcrypto_ecdsa_sig_to_ecdsa_sig(bcrypto_ecdsa_t *ec,
                               const bcrypto_ecdsa_sig_t *sig) {
  ECDSA_SIG *ecsig = NULL;
  BIGNUM *r = NULL;
  BIGNUM *s = NULL;

  ecsig = ECDSA_SIG_new();

  if (ecsig == NULL)
    goto fail;

  r = BN_bin2bn(sig->r, ec->scalar_size, NULL);

  if (r == NULL)
    goto fail;

  s = BN_bin2bn(sig->s, ec->scalar_size, NULL);

  if (s == NULL)
    goto fail;

  if (BN_is_zero(r) || BN_cmp(r, ec->n) >= 0)
    goto fail;

  if (BN_is_zero(s) || BN_cmp(s, ec->n) >= 0)
    goto fail;

  if (!ECDSA_SIG_set0(ecsig, r, s))
    goto fail;

  return ecsig;

fail:
  if (ecsig != NULL)
    ECDSA_SIG_free(ecsig);

  if (r != NULL)
    BN_free(r);

  if (s != NULL)
    BN_free(s);

  return NULL;
}

static void
bcrypto_ecdsa_sig_from_ecdsa_sig(
  bcrypto_ecdsa_t *ec,
  bcrypto_ecdsa_sig_t *sig,
  const ECDSA_SIG *ecsig
) {
  const BIGNUM *r = NULL;
  const BIGNUM *s = NULL;

  ECDSA_SIG_get0(ecsig, &r, &s);

  assert(r != NULL && s != NULL);

  assert((size_t)BN_num_bytes(r) <= ec->scalar_size);
  assert((size_t)BN_num_bytes(s) <= ec->scalar_size);

  assert(BN_bn2binpad(r, sig->r, ec->scalar_size) > 0);
  assert(BN_bn2binpad(s, sig->s, ec->scalar_size) > 0);
}

void
bcrypto_ecdsa_sig_normalize(bcrypto_ecdsa_t *ec,
                            bcrypto_ecdsa_sig_t *out,
                            const bcrypto_ecdsa_sig_t *sig) {
  if (out != sig)
    memcpy(out, sig, sizeof(bcrypto_ecdsa_sig_t));

  if (memcmp(out->s, ec->half, ec->scalar_size) > 0) {
    int carry = 0;
    int i, r;

    for (i = ec->scalar_size - 1; i >= 0; i--) {
      r = (int)ec->order[i] - (int)out->s[i] + carry;
      carry = r >> 8;
      out->s[i] = r & 0xff;
    }
  }
}

int
bcrypto_ecdsa_sig_is_low_s(bcrypto_ecdsa_t *ec,
                           const bcrypto_ecdsa_sig_t *sig) {
  return memcmp(sig->s, ec->zero, ec->scalar_size) != 0
      && memcmp(sig->s, ec->half, ec->scalar_size) <= 0;
}

/*
 * ECDSA
 */

int
bcrypto_ecdsa_init(bcrypto_ecdsa_t *ec, const char *name) {
  assert(ec != NULL && name != NULL);
  memset(ec, 0x00, sizeof(bcrypto_ecdsa_t));

  int type = bcrypto_ecdsa_curve(name);

  if (type == -1)
    return 0;

  EC_KEY *key = EC_KEY_new_by_curve_name(type);

  if (key == NULL)
    return 0;

  ec->type = type;

  ec->hash_type = bcrypto_ecdsa_hash_type(type);
  assert(ec->hash_type != -1);

  ec->hash = EVP_get_digestbynid(ec->hash_type);
  assert(ec->hash != NULL);

  ec->hash_size = EVP_MD_size(ec->hash);

  ec->has_schnorr = bcrypto_ecdsa_has_schnorr(ec->type);

  ec->ctx = BN_CTX_new();
  assert(ec->ctx != NULL);

  ec->key = key;

  ec->group = EC_KEY_get0_group(ec->key);
  assert(ec->group != NULL);

  ec->point = EC_POINT_new(ec->group);
  assert(ec->point != NULL);

  ec->bits = (size_t)EC_GROUP_get_degree(ec->group);
  ec->size = (ec->bits + 7) / 8;
  assert(ec->size <= BCRYPTO_ECDSA_MAX_FIELD_SIZE);

  ec->n = BN_new();
  ec->nh = BN_new();
  ec->ns1 = BN_new();
  ec->p = BN_new();
  ec->a = BN_new();
  ec->b = BN_new();

  assert(ec->n != NULL);
  assert(ec->nh != NULL);
  assert(ec->ns1 != NULL);
  assert(ec->p != NULL);
  assert(ec->a != NULL);
  assert(ec->b != NULL);

  assert(EC_GROUP_get_order(ec->group, ec->n, ec->ctx) != 0);
  assert(BN_rshift1(ec->nh, ec->n) != 0);
  assert(BN_sub(ec->ns1, ec->n, BN_value_one()) != 0);

#if OPENSSL_VERSION_NUMBER >= 0x10200000L
  assert(EC_GROUP_get_curve(ec->group, ec->p, ec->a, ec->b, ec->ctx) != 0);
#else
  assert(EC_GROUP_get_curve_GFp(ec->group, ec->p, ec->a, ec->b, ec->ctx) != 0);
#endif

  ec->g = EC_GROUP_get0_generator(ec->group);
  assert(ec->g != NULL);

  ec->scalar_bits = (size_t)BN_num_bits(ec->n);
  ec->scalar_size = (ec->scalar_bits + 7) >> 3;
  ec->sig_size = ec->scalar_size * 2;
  ec->schnorr_size = ec->size + ec->scalar_size;

  assert(ec->scalar_size <= BCRYPTO_ECDSA_MAX_SCALAR_SIZE);

  assert(BN_bn2binpad(ec->p, &ec->prime[0], ec->size) > 0);
  memset(&ec->zero[0], 0x00, ec->scalar_size);
  assert(BN_bn2binpad(ec->n, &ec->order[0], ec->scalar_size) > 0);
  assert(BN_bn2binpad(ec->nh, &ec->half[0], ec->scalar_size) > 0);

  ec->initialized = 1;

  return 1;
}

void
bcrypto_ecdsa_uninit(bcrypto_ecdsa_t *ec) {
  assert(ec != NULL);

  if (!ec->initialized)
    return;

  BN_CTX_free(ec->ctx);
  EC_KEY_free(ec->key);
  EC_POINT_free(ec->point);
  BN_free(ec->n);
  BN_free(ec->nh);
  BN_free(ec->ns1);
  BN_free(ec->p);
  BN_free(ec->a);
  BN_free(ec->b);

  ec->ctx = NULL;
  ec->key = NULL;
  ec->group = NULL;
  ec->point = NULL;
  ec->n = NULL;
  ec->nh = NULL;
  ec->ns1 = NULL;
  ec->p = NULL;
  ec->a = NULL;
  ec->b = NULL;
  ec->g = NULL;
  ec->initialized = 0;
}

int
bcrypto_ecdsa_privkey_generate(bcrypto_ecdsa_t *ec, uint8_t *priv) {
  do {
    if (!bcrypto_random(priv, ec->scalar_size))
      return 0;
  } while (!bcrypto_ecdsa_valid_scalar(ec, priv));

  return 1;
}

int
bcrypto_ecdsa_privkey_verify(bcrypto_ecdsa_t *ec, const uint8_t *priv) {
  return bcrypto_ecdsa_valid_scalar(ec, priv);
}

static int
bcrypto_ecdsa_privkey_export_inner(bcrypto_ecdsa_t *ec,
                                   uint8_t **out,
                                   size_t *out_len,
                                   const uint8_t *priv,
                                   int compress,
                                   int no_params) {
  EC_KEY *eckey = NULL;
  EC_POINT *point = NULL;
  const BIGNUM *scalar = NULL;
  uint8_t *buf = NULL;
  int len = 0;

  if (!bcrypto_ecdsa_valid_scalar(ec, priv))
    goto fail;

  eckey = EC_KEY_new_by_curve_name(ec->type);

  if (eckey == NULL)
    goto fail;

  if (!EC_KEY_oct2priv(eckey, priv, ec->scalar_size))
    goto fail;

  point = EC_POINT_new(ec->group);

  if (point == NULL)
    goto fail;

  scalar = EC_KEY_get0_private_key(eckey);
  assert(scalar != NULL);

  if (!EC_POINT_mul(ec->group, point, scalar, NULL, NULL, ec->ctx))
    goto fail;

  if (!EC_KEY_set_public_key(eckey, point))
    goto fail;

  point_conversion_form_t form = compress
    ? POINT_CONVERSION_COMPRESSED
    : POINT_CONVERSION_UNCOMPRESSED;

  EC_KEY_set_conv_form(eckey, form);

  if (no_params) {
    EC_KEY_set_enc_flags(eckey,
      EC_KEY_get_enc_flags(eckey) | EC_PKEY_NO_PARAMETERS);
  }

  EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);

  buf = NULL;
  len = i2d_ECPrivateKey(eckey, &buf);

  if (len <= 0)
    goto fail;

  FIX_BORINGSSL(buf, len);

  *out = buf;
  *out_len = (size_t)len;

  EC_KEY_free(eckey);
  EC_POINT_free(point);

  return 1;

fail:
  if (eckey != NULL)
    EC_KEY_free(eckey);

  if (point != NULL)
    EC_POINT_free(point);

  return 0;
}

int
bcrypto_ecdsa_privkey_export(bcrypto_ecdsa_t *ec,
                             uint8_t **out,
                             size_t *out_len,
                             const uint8_t *priv,
                             int compress) {
  return bcrypto_ecdsa_privkey_export_inner(ec, out, out_len,
                                            priv, compress, 0);
}

int
bcrypto_ecdsa_privkey_import(bcrypto_ecdsa_t *ec,
                             uint8_t *out,
                             const uint8_t *raw,
                             size_t raw_len) {
  EC_KEY *eckey = NULL;
  const BIGNUM *scalar = NULL;
  const uint8_t *p = raw;

  eckey = EC_KEY_new_by_curve_name(ec->type);

  if (eckey == NULL)
    goto fail;

  EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);

  if (d2i_ECPrivateKey(&eckey, &p, raw_len) == NULL)
    goto fail;

  scalar = EC_KEY_get0_private_key(eckey);
  assert(scalar != NULL);

  if (BN_is_zero(scalar) || BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  assert((size_t)BN_num_bytes(scalar) <= ec->scalar_size);

  assert(BN_bn2binpad(scalar, out, ec->scalar_size) > 0);

  EC_KEY_free(eckey);

  return 1;

fail:
  if (eckey != NULL)
    EC_KEY_free(eckey);

  return 0;
}

int
bcrypto_ecdsa_privkey_export_pkcs8(bcrypto_ecdsa_t *ec,
                                   uint8_t **out,
                                   size_t *out_len,
                                   const uint8_t *priv,
                                   int compress) {
  /* https://github.com/openssl/openssl/blob/32f803d/crypto/ec/ec_ameth.c#L217 */
  uint8_t *ep = NULL;
  size_t eplen = 0;
  PKCS8_PRIV_KEY_INFO *p8 = NULL;
  uint8_t *buf = NULL;
  int len = 0;

  if (!bcrypto_ecdsa_privkey_export_inner(ec, &ep, &eplen,
                                          priv, compress, 1)) {
    goto fail;
  }

  p8 = PKCS8_PRIV_KEY_INFO_new();

  if (p8 == NULL)
    goto fail;

  if (!PKCS8_pkey_set0(p8, OBJ_nid2obj(NID_X9_62_id_ecPublicKey), 0,
                       V_ASN1_OBJECT, OBJ_nid2obj(ec->type), ep, (int)eplen)) {
    goto fail;
  }

  ep = NULL;

  buf = NULL;
  len = i2d_PKCS8_PRIV_KEY_INFO(p8, &buf);

  if (len <= 0)
    goto fail;

  FIX_BORINGSSL(buf, len);

  *out = buf;
  *out_len = (size_t)len;

  PKCS8_PRIV_KEY_INFO_free(p8);

  return 1;

fail:
  if (ep != NULL)
    OPENSSL_free(ep);

  if (p8 != NULL)
    PKCS8_PRIV_KEY_INFO_free(p8);

  return 0;
}

int
bcrypto_ecdsa_privkey_import_pkcs8(bcrypto_ecdsa_t *ec,
                                   uint8_t *out,
                                   const uint8_t *raw,
                                   size_t raw_len) {
  /* https://github.com/openssl/openssl/blob/32f803d/crypto/ec/ec_ameth.c#L184 */
  PKCS8_PRIV_KEY_INFO *p8 = NULL;
  const unsigned char *p = NULL;
  const void *pval = NULL;
  int ptype = 0;
  int pklen = 0;
  const X509_ALGOR *palg = NULL;
  const ASN1_OBJECT *palgoid = NULL;
  const uint8_t *pp = raw;

  if (d2i_PKCS8_PRIV_KEY_INFO(&p8, &pp, raw_len) == NULL)
    goto fail;

  if (!PKCS8_pkey_get0(NULL, &p, &pklen, &palg, p8))
    goto fail;

  X509_ALGOR_get0(&palgoid, &ptype, &pval, palg);

  if (OBJ_obj2nid(palgoid) != NID_X9_62_id_ecPublicKey)
    goto fail;

  if (ptype == V_ASN1_OBJECT) {
    if (OBJ_obj2nid(pval) != ec->type)
      goto fail;
  } else if (ptype != V_ASN1_UNDEF && ptype != V_ASN1_NULL) {
    goto fail;
  }

  if (!bcrypto_ecdsa_privkey_import(ec, out, p, pklen))
    goto fail;

  PKCS8_PRIV_KEY_INFO_free(p8);

  return 1;

fail:
  if (p8 != NULL)
    PKCS8_PRIV_KEY_INFO_free(p8);

  return 0;
}

int
bcrypto_ecdsa_privkey_tweak_add(bcrypto_ecdsa_t *ec,
                                uint8_t *out,
                                const uint8_t *priv,
                                const uint8_t *tweak) {
  BIGNUM *scalar = NULL;
  BIGNUM *tweak_bn = NULL;

  scalar = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (BN_is_zero(scalar) || BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  tweak_bn = BN_bin2bn(tweak, ec->scalar_size, BN_secure_new());

  if (tweak_bn == NULL)
    goto fail;

  if (BN_cmp(tweak_bn, ec->n) >= 0)
    goto fail;

  if (!BN_mod_add(scalar, scalar, tweak_bn, ec->n, ec->ctx))
    goto fail;

  if (BN_is_zero(scalar))
    goto fail;

  assert((size_t)BN_num_bytes(scalar) <= ec->scalar_size);

  assert(BN_bn2binpad(scalar, out, ec->scalar_size) > 0);

  BN_clear_free(scalar);
  BN_clear_free(tweak_bn);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  if (tweak_bn != NULL)
    BN_clear_free(tweak_bn);

  return 0;
}

int
bcrypto_ecdsa_privkey_tweak_mul(bcrypto_ecdsa_t *ec,
                                uint8_t *out,
                                const uint8_t *priv,
                                const uint8_t *tweak) {
  BIGNUM *scalar = NULL;
  BIGNUM *tweak_bn = NULL;

  scalar = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (BN_is_zero(scalar) || BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  tweak_bn = BN_bin2bn(tweak, ec->scalar_size, BN_secure_new());

  if (tweak_bn == NULL)
    goto fail;

  if (BN_is_zero(tweak_bn) || BN_cmp(tweak_bn, ec->n) >= 0)
    goto fail;

  if (!BN_mod_mul(scalar, scalar, tweak_bn, ec->n, ec->ctx))
    goto fail;

  if (BN_is_zero(scalar))
    goto fail;

  assert((size_t)BN_num_bytes(scalar) <= ec->scalar_size);

  assert(BN_bn2binpad(scalar, out, ec->scalar_size) > 0);

  BN_clear_free(scalar);
  BN_clear_free(tweak_bn);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  if (tweak_bn != NULL)
    BN_clear_free(tweak_bn);

  return 0;
}

int
bcrypto_ecdsa_privkey_reduce(bcrypto_ecdsa_t *ec,
                             uint8_t *out,
                             const uint8_t *priv,
                             size_t priv_len) {
  BIGNUM *scalar = NULL;

  if (priv_len > ec->scalar_size)
    priv_len = ec->scalar_size;

  scalar = BN_bin2bn(priv, priv_len, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (!BN_mod(scalar, scalar, ec->n, ec->ctx))
    goto fail;

  assert((size_t)BN_num_bytes(scalar) <= ec->scalar_size);

  assert(BN_bn2binpad(scalar, out, ec->scalar_size) > 0);

  BN_clear_free(scalar);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  return 0;
}

int
bcrypto_ecdsa_privkey_negate(bcrypto_ecdsa_t *ec,
                             uint8_t *out,
                             const uint8_t *priv) {
  BIGNUM *scalar = NULL;

  scalar = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  if (!BN_mod_sub(scalar, ec->n, scalar, ec->n, ec->ctx))
    goto fail;

  assert((size_t)BN_num_bytes(scalar) <= ec->scalar_size);

  assert(BN_bn2binpad(scalar, out, ec->scalar_size) > 0);

  BN_clear_free(scalar);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  return 0;
}

int
bcrypto_ecdsa_privkey_invert(bcrypto_ecdsa_t *ec,
                             uint8_t *out,
                             const uint8_t *priv) {
  BIGNUM *scalar = NULL;

  scalar = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (BN_is_zero(scalar) || BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  if (!BN_mod_inverse(scalar, scalar, ec->n, ec->ctx))
    goto fail;

  if (BN_is_zero(scalar))
    goto fail;

  assert((size_t)BN_num_bytes(scalar) <= ec->scalar_size);

  assert(BN_bn2binpad(scalar, out, ec->scalar_size) > 0);

  BN_clear_free(scalar);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  return 0;
}

int
bcrypto_ecdsa_pubkey_create(bcrypto_ecdsa_t *ec,
                            bcrypto_ecdsa_pubkey_t *pub,
                            const uint8_t *priv) {
  BIGNUM *scalar = NULL;
  EC_POINT *point = NULL;

  scalar = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (BN_is_zero(scalar) || BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  point = EC_POINT_new(ec->group);

  if (point == NULL)
    goto fail;

  if (!EC_POINT_mul(ec->group, point, scalar, NULL, NULL, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, pub, point))
    goto fail;

  BN_clear_free(scalar);
  EC_POINT_free(point);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  if (point != NULL)
    EC_POINT_free(point);

  return 0;
}

int
bcrypto_ecdsa_pubkey_export_spki(bcrypto_ecdsa_t *ec,
                                 uint8_t **out,
                                 size_t *out_len,
                                 const bcrypto_ecdsa_pubkey_t *pub,
                                 int compress) {
  EC_KEY *eckey = NULL;
  uint8_t *buf = NULL;
  int len = 0;

  eckey = bcrypto_ecdsa_pubkey_to_ec_key(ec, pub);

  if (eckey == NULL)
    goto fail;

  point_conversion_form_t form = compress
    ? POINT_CONVERSION_COMPRESSED
    : POINT_CONVERSION_UNCOMPRESSED;

  EC_KEY_set_conv_form(eckey, form);
  EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);

  buf = NULL;
  len = i2d_EC_PUBKEY(eckey, &buf);

  if (len <= 0)
    goto fail;

  FIX_BORINGSSL(buf, len);

  *out = buf;
  *out_len = (size_t)len;

  EC_KEY_free(eckey);

  return 1;

fail:
  if (eckey != NULL)
    EC_KEY_free(eckey);

  return 0;
}

int
bcrypto_ecdsa_pubkey_import_spki(bcrypto_ecdsa_t *ec,
                                 bcrypto_ecdsa_pubkey_t *out,
                                 const uint8_t *raw,
                                 size_t raw_len) {
  EC_KEY *eckey = NULL;
  const uint8_t *p = raw;

  eckey = EC_KEY_new_by_curve_name(ec->type);

  if (eckey == NULL)
    goto fail;

  EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);

  if (d2i_EC_PUBKEY(&eckey, &p, raw_len) == NULL)
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_key(ec, out, eckey))
    goto fail;

  EC_KEY_free(eckey);

  return 1;

fail:
  if (eckey != NULL)
    EC_KEY_free(eckey);

  return 0;
}

int
bcrypto_ecdsa_pubkey_tweak_add(bcrypto_ecdsa_t *ec,
                               bcrypto_ecdsa_pubkey_t *out,
                               const bcrypto_ecdsa_pubkey_t *pub,
                               const uint8_t *tweak) {
  EC_POINT *point = NULL;
  BIGNUM *tweak_bn = NULL;
  EC_POINT *tweak_point = NULL;

  point = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub);

  if (point == NULL)
    goto fail;

  tweak_bn = BN_bin2bn(tweak, ec->scalar_size, BN_secure_new());

  if (tweak_bn == NULL)
    goto fail;

  if (BN_cmp(tweak_bn, ec->n) >= 0)
    goto fail;

  tweak_point = EC_POINT_new(ec->group);

  if (tweak_point == NULL)
    goto fail;

  if (!EC_POINT_mul(ec->group, tweak_point, tweak_bn, NULL, NULL, ec->ctx))
    goto fail;

  if (!EC_POINT_add(ec->group, point, point, tweak_point, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, out, point))
    goto fail;

  EC_POINT_free(point);
  BN_clear_free(tweak_bn);
  EC_POINT_free(tweak_point);

  return 1;

fail:
  if (point != NULL)
    EC_POINT_free(point);

  if (tweak_bn != NULL)
    BN_clear_free(tweak_bn);

  if (tweak_point != NULL)
    EC_POINT_free(tweak_point);

  return 0;
}

int
bcrypto_ecdsa_pubkey_tweak_mul(bcrypto_ecdsa_t *ec,
                               bcrypto_ecdsa_pubkey_t *out,
                               const bcrypto_ecdsa_pubkey_t *pub,
                               const uint8_t *tweak) {
  return bcrypto_ecdsa_derive(ec, out, pub, tweak);
}

int
bcrypto_ecdsa_pubkey_add(bcrypto_ecdsa_t *ec,
                         bcrypto_ecdsa_pubkey_t *out,
                         const bcrypto_ecdsa_pubkey_t *pub1,
                         const bcrypto_ecdsa_pubkey_t *pub2) {
  EC_POINT *point1 = NULL;
  EC_POINT *point2 = NULL;

  point1 = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub1);

  if (point1 == NULL)
    goto fail;

  point2 = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub2);

  if (point2 == NULL)
    goto fail;

  if (!EC_POINT_add(ec->group, point1, point1, point2, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, out, point1))
    goto fail;

  EC_POINT_free(point1);
  EC_POINT_free(point2);

  return 1;

fail:
  if (point1 != NULL)
    EC_POINT_free(point1);

  if (point2 != NULL)
    EC_POINT_free(point2);

  return 0;
}

int
bcrypto_ecdsa_pubkey_combine(bcrypto_ecdsa_t *ec,
                             bcrypto_ecdsa_pubkey_t *out,
                             const bcrypto_ecdsa_pubkey_t *pubs,
                             size_t length) {
  EC_POINT *result = NULL;
  EC_POINT *point = NULL;
  size_t i = 0;
  int r = 0;

  result = EC_POINT_new(ec->group);

  if (result == NULL)
    goto fail;

  for (; i < length; i++) {
    const bcrypto_ecdsa_pubkey_t *pub = &pubs[i];

    point = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub);

    if (point == NULL)
      goto fail;

    if (!EC_POINT_add(ec->group, result, result, point, ec->ctx))
      goto fail;

    EC_POINT_free(point);
    point = NULL;
  }

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, out, result))
    goto fail;

  r = 1;
fail:
  if (result != NULL)
    EC_POINT_free(result);

  if (point != NULL)
    EC_POINT_free(point);

  return r;
}

int
bcrypto_ecdsa_pubkey_negate(bcrypto_ecdsa_t *ec,
                            bcrypto_ecdsa_pubkey_t *out,
                            const bcrypto_ecdsa_pubkey_t *pub) {
  EC_POINT *point = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub);

  if (point == NULL)
    goto fail;

  if (!EC_POINT_invert(ec->group, point, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, out, point))
    goto fail;

  EC_POINT_free(point);

  return 1;

fail:
  if (point != NULL)
    EC_POINT_free(point);

  return 0;
}

int
bcrypto_ecdsa_sign(bcrypto_ecdsa_t *ec,
                   bcrypto_ecdsa_sig_t *sig,
                   const uint8_t *msg,
                   size_t msg_len,
                   const uint8_t *priv) {
  EC_KEY *eckey = NULL;
  ECDSA_SIG *ecsig = NULL;

  if (!bcrypto_ecdsa_valid_scalar(ec, priv))
    goto fail;

  eckey = EC_KEY_new_by_curve_name(ec->type);

  if (eckey == NULL)
    goto fail;

  if (!EC_KEY_oct2priv(eckey, priv, ec->scalar_size))
    goto fail;

  bcrypto_poll();

  ecsig = ECDSA_do_sign(msg, msg_len, eckey);

  if (ecsig == NULL)
    goto fail;

  bcrypto_ecdsa_sig_from_ecdsa_sig(ec, sig, ecsig);

  bcrypto_ecdsa_sig_normalize(ec, sig, sig);

  EC_KEY_free(eckey);
  ECDSA_SIG_free(ecsig);

  return 1;

fail:
  if (eckey != NULL)
    EC_KEY_free(eckey);

  if (ecsig != NULL)
    ECDSA_SIG_free(ecsig);

  return 0;
}

int
bcrypto_ecdsa_sign_recoverable(bcrypto_ecdsa_t *ec,
                               bcrypto_ecdsa_sig_t *sig,
                               const uint8_t *msg,
                               size_t msg_len,
                               const uint8_t *priv) {
  bcrypto_ecdsa_pubkey_t Q, Qprime;
  int i = 0;

  if (!bcrypto_ecdsa_sign(ec, sig, msg, msg_len, priv))
    return 0;

  if (!bcrypto_ecdsa_pubkey_create(ec, &Q, priv))
    return 0;

  for (; i < 4; i++) {
    if (!bcrypto_ecdsa_recover(ec, &Qprime, msg, msg_len, sig, i))
      continue;

    if (!bcrypto_ecdsa_pubkey_equals(ec, &Q, &Qprime))
      continue;

    sig->param = i;

    return 1;
  }

  return 0;
}

int
bcrypto_ecdsa_verify(bcrypto_ecdsa_t *ec,
                     const uint8_t *msg,
                     size_t msg_len,
                     const bcrypto_ecdsa_sig_t *sig,
                     const bcrypto_ecdsa_pubkey_t *pub) {
  ECDSA_SIG *ecsig = NULL;
  EC_KEY *eckey = NULL;

  ecsig = bcrypto_ecdsa_sig_to_ecdsa_sig(ec, sig);

  if (ecsig == NULL)
    goto fail;

  eckey = bcrypto_ecdsa_pubkey_to_ec_key(ec, pub);

  if (eckey == NULL)
    goto fail;

  if (ECDSA_do_verify(msg, msg_len, ecsig, eckey) <= 0)
    goto fail;

  ECDSA_SIG_free(ecsig);
  EC_KEY_free(eckey);

  return 1;

fail:
  if (ecsig != NULL)
    ECDSA_SIG_free(ecsig);

  if (eckey != NULL)
    EC_KEY_free(eckey);

  return 0;
}

int
bcrypto_ecdsa_recover(bcrypto_ecdsa_t *ec,
                      bcrypto_ecdsa_pubkey_t *pub,
                      const uint8_t *msg,
                      size_t msg_len,
                      const bcrypto_ecdsa_sig_t *sig,
                      int param) {
  int y_odd = 0;
  int second_key = 0;
  ECDSA_SIG *ecsig = NULL;
  const BIGNUM *r = NULL;
  const BIGNUM *s = NULL;
  BIGNUM *x = NULL;
  EC_POINT *rp = NULL;
  BIGNUM *rinv = NULL;
  BIGNUM *s1 = NULL;
  BIGNUM *s2 = NULL;
  BIGNUM *e = NULL;
  int d = 0;
  EC_POINT *Q = NULL;

  if (param < 0 || (param & 3) != param)
    goto fail;

  y_odd = param & 1;
  second_key = param >> 1;

  ecsig = bcrypto_ecdsa_sig_to_ecdsa_sig(ec, sig);

  if (ecsig == NULL)
    goto fail;

  ECDSA_SIG_get0(ecsig, &r, &s);

  assert(r != NULL && s != NULL);

  if (BN_is_zero(r) || BN_cmp(r, ec->n) >= 0)
    goto fail;

  if (BN_is_zero(s) || BN_cmp(s, ec->n) >= 0)
    goto fail;

  x = BN_new();

  if (x == NULL)
    goto fail;

  if (!BN_copy(x, r))
    goto fail;

  if (second_key) {
    BIGNUM *m = BN_new();

    if (m == NULL)
      goto fail;

    if (!BN_mod(m, ec->p, ec->n, ec->ctx)) {
      BN_free(m);
      goto fail;
    }

    if (BN_cmp(r, m) >= 0) {
      BN_free(m);
      goto fail;
    }

    BN_free(m);

    if (!BN_mod_add(x, x, ec->n, ec->p, ec->ctx))
      goto fail;
  }

  rp = EC_POINT_new(ec->group);

  if (rp == NULL)
    goto fail;

#if OPENSSL_VERSION_NUMBER >= 0x10200000L
  if (!EC_POINT_set_compressed_coordinates(ec->group, rp, x, y_odd, ec->ctx))
#else
  if (!EC_POINT_set_compressed_coordinates_GFp(ec->group, rp, x, y_odd, ec->ctx))
#endif
    goto fail;

  rinv = BN_new();

  if (rinv == NULL)
    goto fail;

  if (!BN_mod_inverse(rinv, r, ec->n, ec->ctx))
    goto fail;

  if (msg_len > ec->scalar_size)
    msg_len = ec->scalar_size;

  e = BN_bin2bn(msg, msg_len, NULL);

  if (e == NULL)
    goto fail;

  d = (int)msg_len * 8 - (int)ec->scalar_bits;

  if (d > 0) {
    if (!BN_rshift(e, e, d))
      goto fail;
  }

  if (!BN_mod(e, e, ec->n, ec->ctx))
    goto fail;

  s1 = BN_new();

  if (s1 == NULL)
    goto fail;

  if (!BN_mod_sub(s1, ec->n, e, ec->n, ec->ctx))
    goto fail;

  if (!BN_mod_mul(s1, s1, rinv, ec->n, ec->ctx))
    goto fail;

  s2 = BN_new();

  if (s2 == NULL)
    goto fail;

  if (!BN_mod_mul(s2, s, rinv, ec->n, ec->ctx))
    goto fail;

  Q = EC_POINT_new(ec->group);

  if (Q == NULL)
    goto fail;

  if (!EC_POINT_mul(ec->group, Q, s1, rp, s2, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, pub, Q))
    goto fail;

  ECDSA_SIG_free(ecsig);
  BN_free(x);
  EC_POINT_free(rp);
  BN_free(rinv);
  BN_free(s1);
  BN_free(s2);
  BN_free(e);
  EC_POINT_free(Q);

  return 1;

fail:
  if (ecsig != NULL)
    ECDSA_SIG_free(ecsig);

  if (x != NULL)
    BN_free(x);

  if (rp != NULL)
    EC_POINT_free(rp);

  if (rinv != NULL)
    BN_free(rinv);

  if (s1 != NULL)
    BN_free(s1);

  if (s2 != NULL)
    BN_free(s2);

  if (e != NULL)
    BN_free(e);

  if (Q != NULL)
    EC_POINT_free(Q);

  return 0;
}

int
bcrypto_ecdsa_derive(bcrypto_ecdsa_t *ec,
                     bcrypto_ecdsa_pubkey_t *out,
                     const bcrypto_ecdsa_pubkey_t *pub,
                     const uint8_t *priv) {
  BIGNUM *scalar = NULL;
  EC_POINT *point = NULL;
  EC_POINT *secret = NULL;

  scalar = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (scalar == NULL)
    goto fail;

  if (BN_is_zero(scalar) || BN_cmp(scalar, ec->n) >= 0)
    goto fail;

  point = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub);

  if (point == NULL)
    goto fail;

  secret = EC_POINT_new(ec->group);

  if (secret == NULL)
    goto fail;

  if (!EC_POINT_mul(ec->group, secret, NULL, point, scalar, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, out, secret))
    goto fail;

  BN_clear_free(scalar);
  EC_POINT_free(point);
  EC_POINT_clear_free(secret);

  return 1;

fail:
  if (scalar != NULL)
    BN_clear_free(scalar);

  if (point != NULL)
    EC_POINT_free(point);

  if (secret != NULL)
    EC_POINT_clear_free(secret);

  return 0;
}

/*
 * Schnorr
 */

void
bcrypto_schnorr_sig_encode(bcrypto_ecdsa_t *ec,
                         uint8_t *out,
                         const bcrypto_ecdsa_sig_t *sig) {
  memcpy(&out[0], &sig->r[0], ec->size);
  memcpy(&out[ec->size], &sig->s[0], ec->scalar_size);
}

int
bcrypto_schnorr_sig_decode(bcrypto_ecdsa_t *ec,
                         bcrypto_ecdsa_sig_t *sig,
                         const uint8_t *raw) {
  memcpy(&sig->r[0], &raw[0], ec->size);
  memcpy(&sig->s[0], &raw[ec->size], ec->scalar_size);

  return memcmp(sig->r, ec->prime, ec->size) < 0
      && memcmp(sig->s, ec->order, ec->scalar_size) < 0;
}

static BIGNUM *
schnorr_hash_am(bcrypto_ecdsa_t *ec,
                const uint8_t *key,
                const uint8_t *msg) {
  uint8_t out[EVP_MAX_MD_SIZE];
  EVP_MD_CTX *ctx = NULL;
  BIGNUM *k = NULL;
  unsigned int hash_size;

  ctx = EVP_MD_CTX_new();

  if (ctx == NULL)
    goto fail;

  if (!EVP_DigestInit(ctx, ec->hash))
    goto fail;

  if (!EVP_DigestUpdate(ctx, key, ec->scalar_size))
    goto fail;

  if (!EVP_DigestUpdate(ctx, msg, 32))
    goto fail;

  if (!EVP_DigestFinal(ctx, out, &hash_size))
    goto fail;

  k = BN_bin2bn(out, hash_size, BN_secure_new());

  if (k == NULL)
    goto fail;

  if (!BN_mod(k, k, ec->n, ec->ctx)) {
    BN_clear_free(k);
    k = NULL;
    goto fail;
  }

fail:
  if (ctx != NULL)
    EVP_MD_CTX_free(ctx);

  return k;
}

static BIGNUM *
schnorr_hash_ram(bcrypto_ecdsa_t *ec,
                 const uint8_t *r,
                 const bcrypto_ecdsa_pubkey_t *pub,
                 const uint8_t *msg) {
  uint8_t raw[BCRYPTO_ECDSA_MAX_PUB_SIZE];
  uint8_t out[EVP_MAX_MD_SIZE];
  EVP_MD_CTX *ctx = NULL;
  BIGNUM *e = NULL;
  size_t pub_size;
  unsigned int hash_size;

  ctx = EVP_MD_CTX_new();

  if (ctx == NULL)
    goto fail;

  if (!EVP_DigestInit(ctx, ec->hash))
    goto fail;

  if (!EVP_DigestUpdate(ctx, r, ec->size))
    goto fail;

  bcrypto_ecdsa_pubkey_encode(ec, raw, &pub_size, pub, 1);

  if (!EVP_DigestUpdate(ctx, raw, pub_size))
    goto fail;

  if (!EVP_DigestUpdate(ctx, msg, 32))
    goto fail;

  if (!EVP_DigestFinal(ctx, out, &hash_size))
    goto fail;

  e = BN_bin2bn(out, hash_size, NULL);

  if (e == NULL)
    goto fail;

  if (!BN_mod(e, e, ec->n, ec->ctx)) {
    BN_free(e);
    e = NULL;
    goto fail;
  }

fail:
  if (ctx != NULL)
    EVP_MD_CTX_free(ctx);

  return e;
}

static int
schnorr_lift_x(bcrypto_ecdsa_t *ec,
               EC_POINT *R,
               const BIGNUM *x,
               BIGNUM *ax,
               BIGNUM *y) {
  if (!BN_mod_mul(ax, ec->a, x, ec->p, ec->ctx))
    return 0;

  if (!BN_mod_sqr(y, x, ec->p, ec->ctx))
    return 0;

  if (!BN_mod_mul(y, y, x, ec->p, ec->ctx))
    return 0;

  if (!BN_mod_add(y, y, ax, ec->p, ec->ctx))
    return 0;

  if (!BN_mod_add(y, y, ec->b, ec->p, ec->ctx))
    return 0;

  if (!BN_mod_sqrt(y, y, ec->p, ec->ctx))
    return 0;

#if OPENSSL_VERSION_NUMBER >= 0x10200000L
  // Note: should be present with 1.1.1b
  if (!EC_POINT_set_affine_coordinates(ec->group, R, x, y, ec->ctx))
#else
  if (!EC_POINT_set_affine_coordinates_GFp(ec->group, R, x, y, ec->ctx))
#endif
    return 0;

  return 1;
}

int
bcrypto_schnorr_sign(bcrypto_ecdsa_t *ec,
                     bcrypto_ecdsa_sig_t *sig,
                     const uint8_t *msg,
                     const uint8_t *priv) {
  BIGNUM *a = NULL;
  BIGNUM *k = NULL;
  EC_POINT *R = NULL;
  BIGNUM *x = NULL;
  BIGNUM *y = NULL;
  EC_POINT *A = NULL;
  bcrypto_ecdsa_pubkey_t pub;
  BIGNUM *e = NULL;
  int r = 0;
  int j;

  if (!bcrypto_ecdsa_valid_scalar(ec, priv))
    goto fail;

  // The secret key d: an integer in the range 1..n-1.
  a = BN_bin2bn(priv, ec->scalar_size, BN_secure_new());

  if (a == NULL || BN_is_zero(a) || BN_cmp(a, ec->n) >= 0)
    goto fail;

  // Let k' = int(hash(bytes(d) || m)) mod n
  k = schnorr_hash_am(ec, priv, msg);

  // Fail if k' = 0.
  if (k == NULL || BN_is_zero(k))
    goto fail;

  // Let R = k'*G.
  R = EC_POINT_new(ec->group);

  if (R == NULL)
    goto fail;

  if (!EC_POINT_mul(ec->group, R, k, NULL, NULL, ec->ctx))
    goto fail;

  x = BN_new();
  y = BN_new();

  if (x == NULL || y == NULL)
    goto fail;

  // Encode x(R).
#if OPENSSL_VERSION_NUMBER >= 0x10200000L
  // Note: should be present with 1.1.1b
  if (!EC_POINT_get_affine_coordinates(ec->group, R, x, y, ec->ctx))
#else
  if (!EC_POINT_get_affine_coordinates_GFp(ec->group, R, x, y, ec->ctx))
#endif
    goto fail;

  assert(BN_bn2binpad(x, sig->r, ec->size) != -1);

  // Encode d*G.
  A = EC_POINT_new(ec->group);

  if (A == NULL)
    goto fail;

  if (!EC_POINT_mul(ec->group, A, a, NULL, NULL, ec->ctx))
    goto fail;

  if (!bcrypto_ecdsa_pubkey_from_ec_point(ec, &pub, A))
    goto fail;

  // Let e = int(hash(bytes(x(R)) || bytes(d*G) || m)) mod n.
  e = schnorr_hash_ram(ec, sig->r, &pub, msg);

  if (e == NULL)
    goto fail;

  j = BN_kronecker(y, ec->p, ec->ctx);

  if (j < -1)
    goto fail;

  // Let k = k' if jacobi(y(R)) = 1, otherwise let k = n - k'.
  if (j != 1)
    BN_sub(k, ec->n, k);

  // Let S = k + e*d mod n.
  if (!BN_mod_mul(e, e, a, ec->n, ec->ctx))
    goto fail;

  if (!BN_mod_add(e, k, e, ec->n, ec->ctx))
    goto fail;

  assert(BN_bn2binpad(e, sig->s, ec->scalar_size) != -1);

  r = 1;
fail:
  if (a != NULL)
    BN_clear_free(a);

  if (k != NULL)
    BN_clear_free(k);

  if (R != NULL)
    EC_POINT_free(R);

  if (x != NULL)
    BN_free(x);

  if (y != NULL)
    BN_free(y);

  if (A != NULL)
    EC_POINT_free(A);

  if (e != NULL)
    BN_free(e);

  return r;
}

int
bcrypto_schnorr_verify(bcrypto_ecdsa_t *ec,
                       const uint8_t *msg,
                       const bcrypto_ecdsa_sig_t *sig,
                       const bcrypto_ecdsa_pubkey_t *pub) {
  BIGNUM *Rx = NULL;
  BIGNUM *S = NULL;
  EC_POINT *A = NULL;
  BIGNUM *e = NULL;
  EC_POINT *R = NULL;
  BIGNUM *x = NULL;
  BIGNUM *y = NULL;
  BIGNUM *z = NULL;
  int r = 0;

  Rx = BN_bin2bn(sig->r, ec->size, NULL);
  S = BN_bin2bn(sig->s, ec->scalar_size, NULL);
  A = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub);
  e = schnorr_hash_ram(ec, sig->r, pub, msg);
  R = EC_POINT_new(ec->group);

  if (Rx == NULL || S == NULL || A == NULL || e == NULL || R == NULL)
    goto fail;

  // Let R = s*G - e*P.
  if (!BN_is_zero(e)) {
    if (!BN_sub(e, ec->n, e))
      goto fail;
  }

  if (!EC_POINT_mul(ec->group, R, S, A, e, ec->ctx))
    goto fail;

  x = BN_new();
  y = BN_new();
  z = BN_new();

  if (x == NULL || y == NULL || z == NULL)
    goto fail;

  if (!EC_POINT_get_Jprojective_coordinates_GFp(ec->group, R, x, y, z, ec->ctx))
    goto fail;

  // Check for point at infinity.
  if (BN_is_zero(z))
    goto fail;

  // Check for quadratic residue in the jacobian space.
  // Optimized as `jacobi(y(R) * z(R)) == 1`.
  if (!BN_mod_mul(e, y, z, ec->p, ec->ctx))
    goto fail;

  if (BN_kronecker(e, ec->p, ec->ctx) != 1)
    goto fail;

  // Check `x(R) == r` in the jacobian space.
  // Optimized as `x(R) == r * z(R)^2 mod p`.
  if (!BN_mod_sqr(e, z, ec->p, ec->ctx))
    goto fail;

  if (!BN_mod_mul(e, Rx, e, ec->p, ec->ctx))
    goto fail;

  if (BN_ucmp(x, e) != 0)
    goto fail;

  r = 1;
fail:
  if (Rx != NULL)
    BN_free(Rx);

  if (S != NULL)
    BN_free(S);

  if (A != NULL)
    EC_POINT_free(A);

  if (e != NULL)
    BN_free(e);

  if (R != NULL)
    EC_POINT_free(R);

  if (x != NULL)
    BN_free(x);

  if (y != NULL)
    BN_free(y);

  if (z != NULL)
    BN_free(z);

  return r;
}

int
bcrypto_schnorr_verify_batch(bcrypto_ecdsa_t *ec,
                             const uint8_t **msgs,
                             const bcrypto_ecdsa_sig_t *sigs,
                             const bcrypto_ecdsa_pubkey_t *pubs,
                             size_t length) {
  EC_POINT **points = NULL;
  BIGNUM **coeffs = NULL;
  BIGNUM *sum = NULL;
  BIGNUM *Rx_tmp = NULL;
  BIGNUM *S_tmp = NULL;
  BIGNUM *Rx = NULL;
  BIGNUM *S = NULL;
  EC_POINT *A = NULL;
  BIGNUM *e = NULL;
  EC_POINT *R = NULL;
  BIGNUM *a = NULL;
  BIGNUM *ax = NULL;
  BIGNUM *y = NULL;
  EC_POINT *res = NULL;
  int r = 0;
  size_t i = 0;

  if (length == 0)
    return 1;

  points = (EC_POINT **)OPENSSL_malloc(2 * length * sizeof(EC_POINT *));
  coeffs = (BIGNUM **)OPENSSL_malloc(2 * length * sizeof(BIGNUM *));
  sum = BN_new();
  Rx_tmp = BN_new();
  S_tmp = BN_new();
  ax = BN_new();
  y = BN_new();
  res = EC_POINT_new(ec->group);

  if (points == NULL || coeffs == NULL
      || sum == NULL || Rx_tmp == NULL
      || S_tmp == NULL || ax == NULL
      || y == NULL || res == NULL) {
    goto fail;
  }

  BN_zero(sum);

  bcrypto_poll();

  for (; i < length; i++) {
    const uint8_t *msg = msgs[i];
    const bcrypto_ecdsa_sig_t *sig = &sigs[i];
    const bcrypto_ecdsa_pubkey_t *pub = &pubs[i];

    Rx = BN_bin2bn(sig->r, ec->size, Rx_tmp);
    S = BN_bin2bn(sig->s, ec->scalar_size, S_tmp);
    A = bcrypto_ecdsa_pubkey_to_ec_point(ec, pub);
    e = schnorr_hash_ram(ec, sig->r, pub, msg);
    R = EC_POINT_new(ec->group);
    a = BN_new();

    if (Rx == NULL || S == NULL || A == NULL
        || e == NULL || R == NULL || a == NULL) {
      goto fail;
    }

    if (!schnorr_lift_x(ec, R, Rx, ax, y))
      goto fail;

    if (i == 0) {
      if (!BN_set_word(a, 1))
        goto fail;
    } else {
      if (!BN_rand_range(a, ec->ns1))
        goto fail;

      if (!BN_add_word(a, 1))
        goto fail;

      if (!BN_mod_mul(e, e, a, ec->n, ec->ctx))
        goto fail;

      if (!BN_mod_mul(S, S, a, ec->n, ec->ctx))
        goto fail;
    }

    if (!BN_mod_add(sum, sum, S, ec->n, ec->ctx))
      goto fail;

    points[i * 2 + 0] = R;
    coeffs[i * 2 + 0] = a;
    points[i * 2 + 1] = A;
    coeffs[i * 2 + 1] = e;

    R = NULL;
    a = NULL;
    A = NULL;
    e = NULL;
  }

  if (!BN_is_zero(sum)) {
    if (!BN_sub(sum, ec->n, sum))
      goto fail;
  }

  if (!EC_POINTs_mul(ec->group, res, sum, length * 2,
                     (const EC_POINT **)points,
                     (const BIGNUM **)coeffs,
                     ec->ctx)) {
    goto fail;
  }

  if (!EC_POINT_is_at_infinity(ec->group, res))
    goto fail;

  r = 1;
fail:
  if (sum != NULL)
    BN_free(sum);

  if (Rx_tmp != NULL)
    BN_free(Rx_tmp);

  if (S_tmp != NULL)
    BN_free(S_tmp);

  if (A != NULL)
    EC_POINT_free(A);

  if (e != NULL)
    BN_free(e);

  if (R != NULL)
    EC_POINT_free(R);

  if (a != NULL)
    BN_free(a);

  if (ax != NULL)
    BN_free(ax);

  if (y != NULL)
    BN_free(y);

  if (res != NULL)
    EC_POINT_free(res);

  while (i > 0) {
    EC_POINT_free(points[(i - 1) * 2 + 0]);
    EC_POINT_free(points[(i - 1) * 2 + 1]);
    BN_free(coeffs[(i - 1) * 2 + 0]);
    BN_free(coeffs[(i - 1) * 2 + 1]);
    i -= 1;
  }

  if (points != NULL)
    OPENSSL_free(points);

  if (coeffs != NULL)
    OPENSSL_free(coeffs);

  return r;
}

#endif
