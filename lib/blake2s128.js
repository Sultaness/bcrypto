/*!
 * blake2s128.js - BLAKE2s implementation for bcoin
 * Copyright (c) 2017-2019, Christopher Jeffrey (MIT License).
 * https://github.com/bcoin-org/bcrypto
 */

'use strict';

const BLAKE2s = require('./blake2s');

/**
 * BLAKE2s128
 */

class BLAKE2s128 extends BLAKE2s {
  /**
   * Create a BLAKE2s128 context.
   * @constructor
   */

  constructor() {
    super();
  }

  init(key = null) {
    return super.init(16, key);
  }

  static hash() {
    return new BLAKE2s128();
  }

  static hmac() {
    return super.hmac(16);
  }

  static digest(data, key = null) {
    return super.digest(data, 16, key);
  }

  static root(left, right) {
    return super.root(left, right, 16);
  }

  static multi(x, y, z) {
    return super.multi(x, y, z, 16);
  }

  static mac(data, key) {
    return super.mac(data, key, 16);
  }
}

/*
 * Static
 */

BLAKE2s128.native = BLAKE2s.native;
BLAKE2s128.id = 'BLAKE2S128';
BLAKE2s128.size = 16;
BLAKE2s128.bits = 128;
BLAKE2s128.blockSize = 64;
BLAKE2s128.zero = Buffer.alloc(16, 0x00);
BLAKE2s128.ctx = new BLAKE2s128();

/*
 * Expose
 */

module.exports = BLAKE2s128;
