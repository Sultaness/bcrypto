/*!
 * eddsa.js - ed25519 for bcrypto
 * Copyright (c) 2019, Christopher Jeffrey (MIT License).
 * https://github.com/bcoin-org/bcrypto
 */

'use strict';

const assert = require('bsert');
const crypto = require('crypto');
const asn1 = require('../encoding/asn1');
const pkcs8 = require('../encoding/pkcs8');
const x509 = require('../encoding/x509');
const JS = require('../js/eddsa');

// This API is unstable prior to node 12.
if (!crypto.sign || !crypto.verify)
  throw new Error('EDDSA backend not supported.');

/*
 * ASN.1 Key Prefixes
 */

const prefixes = {
  pkcs8: {
    ED25519: Buffer.from('302e020100300506032b657004220420', 'hex'),
    ED448: Buffer.from('3047020100300506032b6571043b0439', 'hex')
  },
  spki: {
    ED25519: Buffer.from('302a300506032b6570032100', 'hex'),
    ED448: Buffer.from('3043300506032b6571033a00', 'hex')
  }
};

/*
 * EDDSA
 */

class EDDSA extends JS {
  constructor(id, xid, hash, pre) {
    super(id, xid, hash, pre);
    this.native = 1;
  }

  publicKeyCreate(secret) {
    assert(Buffer.isBuffer(secret));
    assert(secret.length === this.size);

    // Added in 11.6.0.
    // `key` allowed to be private in 11.7.0.
    // Note that this function is known to
    // crash prior to node 12.0.0.
    const pub = crypto.createPublicKey({
      key: exportPrivate(this.id, secret),
      format: 'der',
      type: 'pkcs8'
    });

    const raw = pub.export({
      format: 'der',
      type: 'spki'
    });

    return importPublic(this.id, raw);
  }

  _sign(msg, secret, ph, ctx) {
    if (this.curve.context && ph === false)
      ph = null;

    if (ph != null || (ctx && ctx.length > 0))
      return super._sign(msg, secret, ph, ctx);

    // Added in 12.0.0.
    return crypto.sign(null, msg, {
      key: exportPrivate(this.id, secret),
      format: 'der',
      type: 'pkcs8'
    });
  }

  _verify(msg, sig, key, ph, ctx) {
    if (this.curve.context && ph === false)
      ph = null;

    if (ph != null || (ctx && ctx.length > 0))
      return super._verify(msg, sig, key, ph, ctx);

    // Added in 12.0.0.
    return crypto.verify(null, msg, {
      key: exportPublic(this.id, key),
      format: 'der',
      type: 'spki'
    }, sig);
  }

  _verifyBatch(batch, ph, ctx) {
    if (this.curve.context && ph === false)
      ph = null;

    if (ph != null || (ctx && ctx.length > 0))
      return super._verifyBatch(batch, ph, ctx);

    for (const [msg, sig, key] of batch) {
      if (!this._verify(msg, sig, key, ph, ctx))
        return false;
    }

    return true;
  }
}

/*
 * Helpers
 */

function exportPrivate(id, secret) {
  const prefix = prefixes.pkcs8[id];

  if (prefix)
    return Buffer.concat([prefix, secret]);

  return new pkcs8.PrivateKeyInfo(
    0,
    asn1.objects.curves[id],
    new asn1.Null(),
    new asn1.OctString(secret).encode()
  ).encode();
}

function exportPublic(id, key) {
  if (isInfinity(key))
    throw new Error('Invalid point.');

  const prefix = prefixes.spki[id];

  if (prefix)
    return Buffer.concat([prefix, key]);

  return new x509.SubjectPublicKeyInfo(
    asn1.objects.curves[id],
    new asn1.Null(),
    key
  ).encode();
}

function importPublic(id, raw) {
  const prefix = prefixes.spki[id];

  let spki = null;
  let key;

  if (prefix) {
    key = raw.slice(prefix.length);
  } else {
    spki = x509.SubjectPublicKeyInfo.decode(raw);
    key = spki.publicKey.rightAlign();
  }

  if (isInfinity(key))
    throw new Error('Invalid point.');

  return key;
}

function isInfinity(key) {
  if (key[0] !== 0x01)
    return false;

  for (let i = 1; i < key.length; i++) {
    if (key[i] !== 0x00)
      return false;
  }

  return true;
}

/*
 * Expose
 */

module.exports = EDDSA;
