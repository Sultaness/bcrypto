/*!
 * eddsa.js - ed25519 for bcrypto
 * Copyright (c) 2018-2019, Christopher Jeffrey (MIT License).
 * https://github.com/bcoin-org/bcrypto
 *
 * Parts of this software are based on indutny/elliptic:
 *   Copyright (c) 2014, Fedor Indutny (MIT License).
 *   https://github.com/indutny/elliptic
 *
 * Resources:
 *   https://en.wikipedia.org/wiki/EdDSA
 *   https://tools.ietf.org/html/rfc7748
 *   https://tools.ietf.org/html/rfc7748#section-5
 *   https://tools.ietf.org/html/rfc8032
 *   https://tools.ietf.org/html/rfc8032#appendix-A
 *   https://tools.ietf.org/html/rfc8032#appendix-B
 *   https://eprint.iacr.org/2015/625.pdf
 *   http://ed448goldilocks.sourceforge.net/
 *   git://git.code.sf.net/p/ed448goldilocks/code
 *   https://git.zx2c4.com/goldilocks/tree/src
 */

'use strict';

const assert = require('bsert');
const curves = require('./curves');
const eckey = require('../internal/eckey');
const asn1 = require('../encoding/asn1');
const pkcs8 = require('../encoding/pkcs8');
const x509 = require('../encoding/x509');
const BN = require('../bn.js');
const rng = require('../random');

/*
 * Constants
 */

const SLAB = Buffer.alloc(1);

/*
 * EDDSA
 */

class EDDSA {
  constructor(id, xid, hash, pre) {
    assert(typeof id === 'string');
    assert(typeof xid === 'string');
    assert(hash);

    this.id = id;
    this.type = 'edwards';
    this.xid = xid;
    this.hash = hash;
    this._pre = pre || null;
    this._curve = null;
    this._x = null;
    this.native = 0;
  }

  get curve() {
    if (!this._curve) {
      this._curve = new curves[this.id](this._pre);
      this._curve.precompute(rng);
      this._pre = null;
    }
    return this._curve;
  }

  get x() {
    if (!this._x) {
      this._x = new curves[this.xid]();
      this._x.precompute(rng);
    }
    return this._x;
  }

  get size() {
    return this.curve.fieldSize;
  }

  get bits() {
    return this.curve.fieldBits;
  }

  hashInt(ph, ctx, ...items) {
    assert(ph == null || typeof ph === 'boolean');
    assert(ctx == null || Buffer.isBuffer(ctx));
    assert(!ctx || ctx.length <= 255);

    // eslint-disable-next-line
    const h = new this.hash();

    h.init();

    if (this.curve.context || ph != null) {
      // Prefix.
      h.update(this.curve.prefix);

      // Pre-hash Flag.
      SLAB[0] = ph & 0xff;
      h.update(SLAB);

      // Context.
      if (ctx) {
        SLAB[0] = ctx.length;
        h.update(SLAB);
        h.update(ctx);
      } else {
        SLAB[0] = 0x00;
        h.update(SLAB);
      }
    } else {
      assert(ctx == null, 'Must pass pre-hash flag with context.');
    }

    // Integers.
    for (const item of items)
      h.update(item);

    const hash = h.final(this.curve.fieldSize * 2);
    const num = BN.decode(hash, this.curve.endian);

    return num.imod(this.curve.n);
  }

  privateKeyGenerate() {
    return rng.randomBytes(this.curve.fieldSize);
  }

  scalarGenerate() {
    const scalar = rng.randomBytes(this.curve.scalarSize);
    return this.curve.clamp(scalar);
  }

  privateKeyExpand(secret) {
    assert(Buffer.isBuffer(secret));
    assert(secret.length === this.curve.fieldSize);

    const hash = this.hash.digest(secret, this.curve.fieldSize * 2);

    return this.curve.splitHash(hash);
  }

  privateKeyConvert(secret) {
    const [key] = this.privateKeyExpand(secret);
    return key;
  }

  privateKeyVerify(secret) {
    assert(Buffer.isBuffer(secret));
    return secret.length === this.curve.fieldSize;
  }

  scalarVerify(scalar) {
    return this.curve.isClamped(scalar);
  }

  scalarClamp(scalar) {
    assert(Buffer.isBuffer(scalar));
    assert(scalar.length === this.curve.scalarSize);

    if (!this.scalarVerify(scalar)) {
      scalar = Buffer.from(scalar);
      scalar = this.curve.clamp(scalar);
    }

    return scalar;
  }

  privateKeyExport(secret) {
    if (!this.privateKeyVerify(secret))
      throw new Error('Invalid private key.');

    return new asn1.OctString(secret).encode();
  }

  privateKeyImport(raw) {
    const secret = asn1.OctString.decode(raw);

    if (!this.privateKeyVerify(secret.value))
      throw new Error('Invalid private key.');

    return secret.value;
  }

  privateKeyExportPKCS8(secret) {
    // https://tools.ietf.org/html/draft-ietf-curdle-pkix-eddsa-00
    // https://tools.ietf.org/html/rfc8410
    // https://tools.ietf.org/html/rfc5958
    // https://tools.ietf.org/html/rfc7468
    return new pkcs8.PrivateKeyInfo(
      0,
      asn1.objects.curves[this.id],
      new asn1.Null(),
      this.privateKeyExport(secret)
    ).encode();
  }

  privateKeyImportPKCS8(raw) {
    const pki = pkcs8.PrivateKeyInfo.decode(raw);
    const version = pki.version.toNumber();
    const {algorithm, parameters} = pki.algorithm;

    assert(version === 0 || version === 1);
    assert(algorithm.toString() === asn1.objects.curves[this.id]);
    assert(parameters.node.type === asn1.types.NULL);

    return this.privateKeyImport(pki.privateKey.value);
  }

  privateKeyExportJWK(secret) {
    return eckey.privateKeyExportJWK(this, secret);
  }

  privateKeyImportJWK(json) {
    return eckey.privateKeyImportJWK(this, json);
  }

  scalarTweakAdd(scalar, tweak) {
    const a = this.curve.decodeScalar(scalar);
    const t = this.curve.decodeScalar(tweak);
    const s = a.iadd(t).imod(this.curve.n);

    if (s.isZero())
      throw new Error('Invalid scalar.');

    return this.curve.encodeScalar(s);
  }

  scalarTweakMul(scalar, tweak) {
    const a = this.curve.decodeScalar(scalar);
    const t = this.curve.decodeScalar(tweak);
    const s = a.imul(t).imod(this.curve.n);

    if (s.isZero())
      throw new Error('Invalid scalar.');

    return this.curve.encodeScalar(s);
  }

  scalarReduce(scalar) {
    assert(Buffer.isBuffer(scalar));

    if (scalar.length > this.curve.scalarSize)
      scalar = scalar.slice(0, this.curve.scalarSize);

    const s = BN.decode(scalar, this.curve.endian).imod(this.curve.n);

    return this.curve.encodeScalar(s);
  }

  scalarNegate(scalar) {
    const a = this.curve.decodeScalar(scalar).imod(this.curve.n);
    const s = a.ineg().imod(this.curve.n);

    return this.curve.encodeScalar(s);
  }

  scalarInvert(scalar) {
    const a = this.curve.decodeScalar(scalar).imod(this.curve.n);

    if (a.isZero())
      throw new Error('Invalid scalar.');

    const s = a.invert(this.curve.n);

    if (s.isZero())
      throw new Error('Invalid scalar.');

    return this.curve.encodeScalar(s);
  }

  publicKeyCreate(secret) {
    const key = this.privateKeyConvert(secret);
    return this.publicKeyFromScalar(key);
  }

  publicKeyFromScalar(scalar) {
    const a = this.curve.decodeScalar(scalar).imod(this.curve.n);
    const A = this.curve.g.mulBlind(a);

    return A.encode();
  }

  publicKeyConvert(key) {
    const point = this.curve.decodePoint(key);
    return this.x.pointFromEdwards(point).encode();
  }

  publicKeyDeconvert(key, sign = false) {
    const point = this.x.decodePoint(key);
    return this.curve.pointFromMont(point, sign).encode();
  }

  publicKeyVerify(key) {
    assert(Buffer.isBuffer(key));

    try {
      this.curve.decodePoint(key);
    } catch (e) {
      return false;
    }

    return true;
  }

  publicKeyExport(key) {
    if (!this.publicKeyVerify(key))
      throw new Error('Invalid public key.');

    return Buffer.from(key);
  }

  publicKeyImport(raw) {
    if (!this.publicKeyVerify(raw))
      throw new Error('Invalid public key.');

    return Buffer.from(raw);
  }

  publicKeyExportSPKI(key) {
    // https://tools.ietf.org/html/rfc8410
    return new x509.SubjectPublicKeyInfo(
      asn1.objects.curves[this.id],
      new asn1.Null(),
      this.publicKeyExport(key)
    ).encode();
  }

  publicKeyImportSPKI(raw) {
    const spki = x509.SubjectPublicKeyInfo.decode(raw);
    const {algorithm, parameters} = spki.algorithm;

    assert(algorithm.toString() === asn1.objects.curves[this.id]);
    assert(parameters.node.type === asn1.types.NULL);

    return this.publicKeyImport(spki.publicKey.rightAlign());
  }

  publicKeyExportJWK(key) {
    return eckey.publicKeyExportJWK(this, key);
  }

  publicKeyImportJWK(json) {
    return eckey.publicKeyImportJWK(this, json, false);
  }

  publicKeyTweakAdd(key, tweak) {
    const t = this.curve.decodeScalar(tweak).imod(this.curve.n);
    const A = this.curve.decodePoint(key);
    const T = this.curve.g.mul(t);
    const point = T.add(A);

    return point.encode();
  }

  publicKeyTweakMul(key, tweak) {
    const t = this.curve.decodeScalar(tweak).imod(this.curve.n);
    const A = this.curve.decodePoint(key);
    const point = A.mul(t);

    return point.encode();
  }

  publicKeyAdd(key1, key2) {
    const A1 = this.curve.decodePoint(key1);
    const A2 = this.curve.decodePoint(key2);
    const point = A1.add(A2);

    return point.encode();
  }

  publicKeyCombine(keys) {
    assert(Array.isArray(keys));

    let acc = this.curve.point();

    for (const key of keys) {
      const point = this.curve.decodePoint(key);

      acc = acc.add(point);
    }

    return acc.encode();
  }

  publicKeyNegate(key) {
    const A = this.curve.decodePoint(key);
    const point = A.neg();

    return point.encode();
  }

  sign(msg, secret, ph, ctx) {
    const [key, prefix] = this.privateKeyExpand(secret);
    return this.signWithScalar(msg, key, prefix, ph, ctx);
  }

  signWithScalar(msg, scalar, prefix, ph, ctx) {
    assert(Buffer.isBuffer(msg));
    assert(Buffer.isBuffer(prefix));
    assert(prefix.length === this.curve.fieldSize);

    const N = this.curve.n;
    const G = this.curve.g;
    const k = this.hashInt(ph, ctx, prefix, msg);
    const Rraw = G.mulBlind(k).encode();
    const a = this.curve.decodeScalar(scalar);
    const Araw = G.mulBlind(a).encode();
    const e = this.hashInt(ph, ctx, Rraw, Araw, msg);

    // Scalar blinding factor.
    const [blind, unblind] = this.curve.getBlinding();

    // Blind.
    a.imul(blind).imod(N);
    k.imul(blind).imod(N);

    // S := (k + e * a) mod n
    const S = k.iadd(e.imul(a)).imod(N);

    // Unblind.
    S.imul(unblind).imod(N);

    // Note: S is technically a scalar, but
    // decode as field due to the useless byte.
    return Buffer.concat([Rraw, this.curve.encodeField(S)]);
  }

  signTweakAdd(msg, secret, tweak, ph, ctx) {
    const [key_, prefix_] = this.privateKeyExpand(secret);
    const key = this.scalarTweakAdd(key_, tweak);
    const expanded = this.hash.multi(prefix_, tweak, null,
                                     this.curve.fieldSize * 2);
    const prefix = expanded.slice(0, this.curve.fieldSize);

    return this.signWithScalar(msg, key, prefix, ph, ctx);
  }

  signTweakMul(msg, secret, tweak, ph, ctx) {
    const [key_, prefix_] = this.privateKeyExpand(secret);
    const key = this.scalarTweakMul(key_, tweak);
    const expanded = this.hash.multi(prefix_, tweak, null,
                                     this.curve.fieldSize * 2);
    const prefix = expanded.slice(0, this.curve.fieldSize);

    return this.signWithScalar(msg, key, prefix, ph, ctx);
  }

  verify(msg, sig, key, ph, ctx) {
    assert(Buffer.isBuffer(msg));
    assert(Buffer.isBuffer(sig));
    assert(Buffer.isBuffer(key));
    assert(ph == null || typeof ph === 'boolean');
    assert(ctx == null || Buffer.isBuffer(ctx));
    assert(!ctx || ctx.length <= 255);

    if (!this.curve.context && ctx != null)
      assert(ph != null, 'Must pass pre-hash flag with context.');

    if (sig.length !== this.curve.fieldSize * 2)
      return false;

    if (key.length !== this.curve.fieldSize)
      return false;

    try {
      return this._verify(msg, sig, key, ph, ctx);
    } catch (e) {
      return false;
    }
  }

  _verify(msg, sig, key, ph, ctx) {
    const N = this.curve.n;
    const G = this.curve.g;
    const Rraw = sig.slice(0, this.curve.fieldSize);
    const Sraw = sig.slice(this.curve.fieldSize);
    const R = this.curve.decodePoint(Rraw);
    const S = this.curve.decodeField(Sraw);
    const A = this.curve.decodePoint(key);

    // Note: S is technically a scalar, but
    // decode as field due to the useless byte.
    if (S.cmp(N) >= 0)
      return false;

    // e = H(R, A, m).
    const e = this.hashInt(ph, ctx, Rraw, key, msg);

    // The spec says to check:
    //   G*S == R + A*e
    // But we can use shamir's trick to check:
    //   R == G*S - A*e
    const Rp = G.mulAdd(S, A, e.ineg().imod(N));

    // We don't allow points at infinity.
    // This is technically enforced by the
    // decodePoint call above, but put it
    // here to be more explicit.
    if (Rp.isInfinity())
      return false;

    return Rp.eq(R);
  }

  verifyBatch(batch, ph, ctx) {
    assert(Array.isArray(batch));
    assert(ph == null || typeof ph === 'boolean');
    assert(ctx == null || Buffer.isBuffer(ctx));
    assert(!ctx || ctx.length <= 255);

    if (!this.curve.context && ctx != null)
      assert(ph != null, 'Must pass pre-hash flag with context.');

    for (const item of batch) {
      assert(Array.isArray(item) && item.length === 3);

      const [msg, sig, key] = item;

      assert(Buffer.isBuffer(msg));
      assert(Buffer.isBuffer(sig));
      assert(Buffer.isBuffer(key));

      if (sig.length !== this.curve.fieldSize * 2)
        return false;

      if (key.length !== this.curve.fieldSize)
        return false;
    }

    try {
      return this._verifyBatch(batch, ph, ctx);
    } catch (e) {
      return false;
    }
  }

  _verifyBatch(batch, ph, ctx) {
    const N = this.curve.n;
    const G = this.curve.g;
    const points = [];
    const coeffs = [];

    let sum = null;

    for (const [msg, sig, key] of batch) {
      const Rraw = sig.slice(0, this.curve.fieldSize);
      const Sraw = sig.slice(this.curve.fieldSize);
      const R = this.curve.decodePoint(Rraw);
      const S = this.curve.decodeField(Sraw);
      const A = this.curve.decodePoint(key);

      // Note: S is technically a scalar, but
      // decode as field due to the useless byte.
      if (S.cmp(N) >= 0)
        return false;

      const e = this.hashInt(ph, ctx, Rraw, key, msg);

      if (sum === null) {
        sum = S;
        points.push(R, A);
        coeffs.push(new BN(1), e);
        continue;
      }

      const a = BN.random(rng, 1, N);
      const ea = e.imul(a).imod(N);

      sum.iadd(S.imul(a)).imod(N);
      points.push(R, A);
      coeffs.push(a, ea);
    }

    if (sum === null)
      return true;

    const lhs = G.mul(sum);
    const rhs = this.curve.mulAll(points, coeffs);

    return lhs.eq(rhs);
  }

  derive(pub, secret) {
    const priv = this.privateKeyConvert(secret);
    return this.deriveWithScalar(pub, priv);
  }

  deriveWithScalar(pub, scalar) {
    const s = this.curve.decodeScalar(scalar).imod(this.curve.n);
    const A = this.curve.decodePoint(pub);
    const point = A.mulBlind(s, rng);

    return point.encode();
  }

  exchange(pub, secret) {
    const priv = this.privateKeyConvert(secret);
    return this.exchangeWithScalar(pub, priv);
  }

  exchangeWithScalar(pub, scalar) {
    const s = this.x.decodeScalar(scalar);
    const A = this.x.decodePoint(pub);
    const point = A.mul(s);

    return point.encode();
  }
}

/*
 * Expose
 */

module.exports = EDDSA;
