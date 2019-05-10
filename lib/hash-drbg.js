/*!
 * hash-drbg.js - hash-drbg implementation for bcrypto
 * Copyright (c) 2018, Christopher Jeffrey (MIT License).
 * https://github.com/bcoin-org/bcrypto
 *
 * Parts of this software are based on cryptocoinjs/drbg.js:
 *   Copyright (c) 2016 Kirill Fomichev
 *   https://github.com/cryptocoinjs/drbg.js
 *
 * Resources:
 *   https://csrc.nist.gov/publications/detail/sp/800-90a/archive/2012-01-23
 *   https://github.com/cryptocoinjs/drbg.js/blob/master/hash.js
 */

'use strict';

const assert = require('bsert');

/*
 * Constants
 */

const RESEED_INTERVAL = 0x1000000000000;
const ONE = Buffer.from([0x01]);
const TWO = Buffer.from([0x02]);
const THREE = Buffer.from([0x03]);

/**
 * HashDRBG
 */

class HashDRBG {
  constructor(alg, entropy, nonce, pers) {
    assert(alg && typeof alg.id === 'string');

    this.alg = alg;
    this.minEntropy = alg.id === 'SHA1' ? 10 : 24;
    this.seedLen = alg.size <= 32 ? 55 : 111;

    this.V = Buffer.allocUnsafe(this.seedLen);
    this.C = Buffer.allocUnsafe(this.seedLen);
    this.len = Buffer.allocUnsafe(8);
    this.rounds = 0;

    if (entropy)
      this.init(entropy, nonce, pers);
  }

  init(entropy, nonce = null, pers = null) {
    assert(Buffer.isBuffer(entropy));
    assert(!nonce || Buffer.isBuffer(nonce));
    assert(!pers || Buffer.isBuffer(pers));

    const seed = concat(entropy, nonce, pers);

    if (seed.length < this.minEntropy)
      throw new Error('Not enough entropy.');

    this.V = this.hashdf(seed, this.seedLen, null);
    this.C = this.hashdf(this.V, this.seedLen, 0x00);
    this.rounds = 1;

    return this;
  }

  reseed(entropy, add) {
    assert(Buffer.isBuffer(entropy));
    assert(!add || Buffer.isBuffer(add));

    const seed = concat(this.V, entropy, add);

    if (seed.length < this.minEntropy)
      throw new Error('Not enough entropy.');

    this.V = this.hashdf(seed, this.seedLen, 0x01);
    this.C = this.hashdf(this.V, this.seedLen, 0x00);
    this.rounds = 1;

    return this;
  }

  generate(len, add) {
    assert((len >>> 0) === len);
    assert(!add || Buffer.isBuffer(add));

    if (this.rounds === 0)
      throw new Error('DRBG not initialized.');

    if (this.rounds > RESEED_INTERVAL)
      throw new Error('Reseed is required.');

    if (add && add.length !== 0)
      this.sum(this.V, this.alg.multi(TWO, this.V, add));

    const result = this.hashgen(len);
    const H = this.alg.multi(THREE, this.V);

    this.len.writeUInt32BE((this.rounds / 0x0100000000) >>> 0, 0);
    this.len.writeUInt32BE(this.rounds >>> 0, 4);

    this.sum(this.V, H, this.C, this.len);

    this.rounds += 1;

    return result;
  }

  randomBytes(size) {
    return this.generate(size);
  }

  hashdf(input, len, prepend) {
    assert(Buffer.isBuffer(input));
    assert((len >>> 0) === len);

    const p = prepend != null ? 1 : 0;
    const data = Buffer.allocUnsafe(5 + p + input.length);

    data[0] = 0x01;
    data[1] = len >>> 21;
    data[2] = len >>> 13;
    data[3] = len >>> 5;
    data[4] = (len & 0x1f) << 3;

    if (p)
      data[5] = prepend;

    input.copy(data, 5 + p);

    const rounds = Math.ceil(len / this.alg.size);
    const size = rounds * this.alg.size;
    const out = Buffer.allocUnsafe(size);

    for (let i = 0; i < rounds; i++) {
      this.alg.digest(data).copy(out, i * this.alg.size);
      data[0] += 1;
    }

    return out.slice(0, len);
  }

  hashgen(len) {
    assert((len >>> 0) === len);

    const data = Buffer.from(this.V);
    const rounds = Math.ceil(len / this.alg.size);
    const size = rounds * this.alg.size;
    const out = Buffer.allocUnsafe(size);

    for (let i = 0; i < rounds; i++) {
      this.alg.digest(data).copy(out, i * this.alg.size);
      this.sum(data, ONE);
    }

    return out.slice(0, len);
  }

  sum(dst, ...args) {
    for (const buf of args) {
      let j = buf.length - 1;
      let dj = dst.length - 1;
      let carry = 0;

      while (j >= 0) {
        carry += buf[j] + dst[dj];
        dst[dj] = carry & 0xff;
        carry >>>= 8;
        j -= 1;
        dj -= 1;
      }

      while (carry > 0 && dj >= 0) {
        carry += dst[dj];
        dst[dj] = carry & 0xff;
        carry >>>= 8;
        dj -= 1;
      }
    }

    return dst;
  }
}

HashDRBG.native = 0;

/*
 * Helpers
 */

function concat(a, b = null, c = null) {
  if (!b && !c)
    return a;

  let s = a.length;
  let p = 0;

  if (b)
    s += b.length;

  if (c)
    s += c.length;

  const d = Buffer.allocUnsafe(s);

  p += a.copy(d, p);

  if (b)
    p += b.copy(d, p);

  if (c)
    c.copy(d, p);

  return d;
}

/*
 * Expose
 */

module.exports = HashDRBG;