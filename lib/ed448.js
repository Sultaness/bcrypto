/*!
 * ed448.js - ed448 for bcrypto
 * Copyright (c) 2017-2019, Christopher Jeffrey (MIT License).
 * https://github.com/bcoin-org/bcrypto
 */

'use strict';

try {
  module.exports = require('./native/ed448');
} catch (e) {
  try {
    module.exports = require('./node/ed448');
  } catch (e) {
    module.exports = require('./js/ed448');
  }
}
