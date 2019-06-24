/*!
 * Parts of this software are based on cryptocoinjs/secp256k1-node:
 *
 * https://github.com/cryptocoinjs/secp256k1-node
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2016 secp256k1-node contributors
 *
 * Parts of this software are based on bn.js, elliptic, hash.js
 * Copyright (c) 2014-2016 Fedor Indutny
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Parts of this software are based on bitcoin-core/secp256k1:
 *
 * https://github.com/bitcoin-core/secp256k1
 *
 * Copyright (c) 2013 Pieter Wuille
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <node.h>
#include <nan.h>
#include <memory>

#include "secp256k1.h"
#include "secp256k1/include/secp256k1.h"
#include "secp256k1/include/secp256k1_ecdh.h"
#include "secp256k1/include/secp256k1_recovery.h"
#include "secp256k1/include/secp256k1_schnorrsig.h"
#include "secp256k1/contrib/lax_der_privatekey_parsing.h"
#include "secp256k1/contrib/lax_der_parsing.h"
#include "secp256k1/src/util.h"
#include "secp256k1/src/field_impl.h"
#include "secp256k1/src/scalar_impl.h"
#include "secp256k1/src/group_impl.h"
#include "secp256k1/src/ecmult_const_impl.h"
#include "secp256k1/src/ecmult_gen_impl.h"
#include "random/random.h"

#define COMPRESSED_TYPE_INVALID "compressed should be a boolean"

#define EC_PRIVATE_KEY_TYPE_INVALID "private key should be a Buffer"
#define EC_PRIVATE_KEY_LENGTH_INVALID "private key length is invalid"
#define EC_PRIVATE_KEY_RANGE_INVALID "private key range is invalid"
#define EC_PRIVATE_KEY_TWEAK_ADD_FAIL \
  "tweak out of range or resulting private key is invalid"
#define EC_PRIVATE_KEY_TWEAK_MUL_FAIL "tweak out of range"
#define EC_PRIVATE_KEY_EXPORT_DER_FAIL "couldn't export to DER format"
#define EC_PRIVATE_KEY_IMPORT_DER_FAIL "couldn't import from DER format"

#define EC_PUBLIC_KEYS_TYPE_INVALID "public keys should be an Array"
#define EC_PUBLIC_KEYS_LENGTH_INVALID \
  "public keys Array should have at least 1 element"
#define EC_PUBLIC_KEY_TYPE_INVALID "public key should be a Buffer"
#define EC_PUBLIC_KEY_LENGTH_INVALID "public key length is invalid"
#define EC_PUBLIC_KEY_PARSE_FAIL \
  "the public key could not be parsed or is invalid"
#define EC_PUBLIC_KEY_CREATE_FAIL "private was invalid, try again"
#define EC_PUBLIC_KEY_TWEAK_ADD_FAIL \
  "tweak out of range or resulting public key is invalid"
#define EC_PUBLIC_KEY_TWEAK_MUL_FAIL "tweak out of range"
#define EC_PUBLIC_KEY_COMBINE_FAIL "the sum of the public keys is not valid"
#define EC_PUBLIC_KEY_NEGATE_FAIL "public key negation failed"

#define ECDH_FAIL "scalar was invalid (zero or overflow)"

#define ECDSA_SIGNATURE_TYPE_INVALID "signature should be a Buffer"
#define ECDSA_SIGNATURE_LENGTH_INVALID "signature length is invalid"
#define ECDSA_SIGNATURE_PARSE_FAIL "couldn't parse signature"
#define ECDSA_SIGNATURE_PARSE_DER_FAIL "couldn't parse DER signature"
#define ECDSA_SIGNATURE_SERIALIZE_DER_FAIL \
  "couldn't serialize signature to DER format"

#define ECDSA_SIGN_FAIL \
  "nonce generation function failed or private key is invalid"
#define ECDSA_RECOVER_FAIL "couldn't recover public key from signature"

#define MSG32_TYPE_INVALID "message should be a Buffer"
#define MSG32_LENGTH_INVALID "message length is invalid"

#define OPTIONS_TYPE_INVALID "options should be an Object"
#define OPTIONS_DATA_TYPE_INVALID "options.data should be a Buffer"
#define OPTIONS_DATA_LENGTH_INVALID "options.data length is invalid"
#define OPTIONS_NONCEFN_TYPE_INVALID "options.noncefn should be a Function"

#define RECOVERY_ID_TYPE_INVALID "recovery should be a Number"
#define RECOVERY_ID_VALUE_INVALID "recovery should have value between -1 and 4"

#define TWEAK_TYPE_INVALID "tweak should be a Buffer"
#define TWEAK_LENGTH_INVALID "tweak length is invalid"

#define COPY_BUFFER(data, datalen) \
  Nan::CopyBuffer((const char *)data, (uint32_t)datalen).ToLocalChecked()

#define UPDATE_COMPRESSED_VALUE(compressed, value, v_true, v_false) {          \
  if (!value->IsUndefined() && !value->IsNull()) {                             \
    CHECK_TYPE_BOOLEAN(value, COMPRESSED_TYPE_INVALID);                        \
    compressed = Nan::To<bool>(value).FromJust() ? v_true : v_false;           \
  }                                                                            \
}

// TypeError
#define CHECK_TYPE_ARRAY(value, message) do {                                  \
  if (!value->IsArray())                                                       \
    return Nan::ThrowTypeError(message);                                       \
} while (0)

#define CHECK_TYPE_BOOLEAN(value, message) do {                                \
  if (!value->IsBoolean() && !value->IsBooleanObject())                        \
    return Nan::ThrowTypeError(message);                                       \
} while (0)

#define CHECK_TYPE_BUFFER(value, message) do {                                 \
  if (!node::Buffer::HasInstance(value))                                       \
    return Nan::ThrowTypeError(message);                                       \
} while (0)

#define CHECK_TYPE_FUNCTION(value, message) do {                               \
  if (!value->IsFunction())                                                    \
    return Nan::ThrowTypeError(message);                                       \
} while (0)

#define CHECK_TYPE_NUMBER(value, message) do {                                 \
  if (!value->IsNumber() && !value->IsNumberObject())                          \
    return Nan::ThrowTypeError(message);                                       \
} while (0)

#define CHECK_TYPE_OBJECT(value, message) do {                                 \
  if (!value->IsObject())                                                      \
    return Nan::ThrowTypeError(message);                                       \
} while (0)

// RangeError
#define CHECK_BUFFER_LENGTH(buffer, length, message) do {                      \
  if (node::Buffer::Length(buffer) != length)                                  \
    return Nan::ThrowRangeError(message);                                      \
} while (0)

#define CHECK_BUFFER_LENGTH2(buffer, length1, length2, message) do {           \
  if (node::Buffer::Length(buffer) != length1 &&                               \
      node::Buffer::Length(buffer) != length2) {                               \
    return Nan::ThrowRangeError(message);                                      \
  }                                                                            \
} while (0)

#define CHECK_BUFFER_LENGTH_GT_ZERO(buffer, message) do {                      \
  if (node::Buffer::Length(buffer) == 0)                                       \
    return Nan::ThrowRangeError(message);                                      \
} while (0)

#define CHECK_LENGTH_GT_ZERO(value, message) do {                              \
  if (value->Length() == 0)                                                    \
    return Nan::ThrowRangeError(message);                                      \
} while (0)

#define CHECK_NUMBER_IN_INTERVAL(number, x, y, message) do {                   \
  if (Nan::To<int64_t>(number).FromJust() <= x ||                              \
      Nan::To<int64_t>(number).FromJust() >= y) {                              \
    return Nan::ThrowRangeError(message);                                      \
  }                                                                            \
} while (0)

static Nan::Persistent<v8::FunctionTemplate> secp256k1_constructor;

BSecp256k1::BSecp256k1() {
  ctx = NULL;
}

BSecp256k1::~BSecp256k1() {
  if (ctx != NULL) {
    secp256k1_context_destroy(ctx);
    ctx = NULL;
  }
}

void
BSecp256k1::Init(v8::Local<v8::Object> &target) {
  Nan::HandleScope scope;

  v8::Local<v8::FunctionTemplate> tpl =
    Nan::New<v8::FunctionTemplate>(BSecp256k1::New);

  secp256k1_constructor.Reset(tpl);

  tpl->SetClassName(Nan::New("Secp256k1").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  // secret key
  Nan::SetPrototypeMethod(tpl, "privateKeyVerify", BSecp256k1::PrivateKeyVerify);
  Nan::SetPrototypeMethod(tpl, "privateKeyExport", BSecp256k1::PrivateKeyExport);
  Nan::SetPrototypeMethod(tpl, "privateKeyImport", BSecp256k1::PrivateKeyImport);
  Nan::SetPrototypeMethod(tpl, "privateKeyReduce", BSecp256k1::PrivateKeyReduce);
  Nan::SetPrototypeMethod(tpl, "privateKeyNegate", BSecp256k1::PrivateKeyNegate);
  Nan::SetPrototypeMethod(tpl, "privateKeyInvert", BSecp256k1::PrivateKeyInvert);
  Nan::SetPrototypeMethod(tpl, "privateKeyTweakAdd", BSecp256k1::PrivateKeyTweakAdd);
  Nan::SetPrototypeMethod(tpl, "privateKeyTweakMul", BSecp256k1::PrivateKeyTweakMul);

  // public key
  Nan::SetPrototypeMethod(tpl, "publicKeyCreate", BSecp256k1::PublicKeyCreate);
  Nan::SetPrototypeMethod(tpl, "publicKeyConvert", BSecp256k1::PublicKeyConvert);
  Nan::SetPrototypeMethod(tpl, "publicKeyVerify", BSecp256k1::PublicKeyVerify);
  Nan::SetPrototypeMethod(tpl, "publicKeyTweakAdd", BSecp256k1::PublicKeyTweakAdd);
  Nan::SetPrototypeMethod(tpl, "publicKeyTweakMul", BSecp256k1::PublicKeyTweakMul);
  Nan::SetPrototypeMethod(tpl, "publicKeyAdd", BSecp256k1::PublicKeyAdd);
  Nan::SetPrototypeMethod(tpl, "publicKeyCombine", BSecp256k1::PublicKeyCombine);
  Nan::SetPrototypeMethod(tpl, "publicKeyNegate", BSecp256k1::PublicKeyNegate);

  // signature
  Nan::SetPrototypeMethod(tpl, "signatureNormalize", BSecp256k1::SignatureNormalize);
  Nan::SetPrototypeMethod(tpl, "signatureNormalizeDER", BSecp256k1::SignatureNormalizeDER);
  Nan::SetPrototypeMethod(tpl, "signatureExport", BSecp256k1::SignatureExport);
  Nan::SetPrototypeMethod(tpl, "signatureImport", BSecp256k1::SignatureImport);
  Nan::SetPrototypeMethod(tpl, "isLowS", BSecp256k1::IsLowS);
  Nan::SetPrototypeMethod(tpl, "isLowDER", BSecp256k1::IsLowDER);

  // ecdsa
  Nan::SetPrototypeMethod(tpl, "sign", BSecp256k1::Sign);
  Nan::SetPrototypeMethod(tpl, "signRecoverable", BSecp256k1::SignRecoverable);
  Nan::SetPrototypeMethod(tpl, "signDER", BSecp256k1::SignDER);
  Nan::SetPrototypeMethod(tpl, "signRecoverableDER", BSecp256k1::SignRecoverableDER);
  Nan::SetPrototypeMethod(tpl, "verify", BSecp256k1::Verify);
  Nan::SetPrototypeMethod(tpl, "verifyDER", BSecp256k1::VerifyDER);
  Nan::SetPrototypeMethod(tpl, "recover", BSecp256k1::Recover);
  Nan::SetPrototypeMethod(tpl, "recoverDER", BSecp256k1::RecoverDER);

  // ecdh
  Nan::SetPrototypeMethod(tpl, "derive", BSecp256k1::Derive);

  // schnorr
  Nan::SetPrototypeMethod(tpl, "schnorrSign", BSecp256k1::SchnorrSign);
  Nan::SetPrototypeMethod(tpl, "schnorrVerify", BSecp256k1::SchnorrVerify);
  Nan::SetPrototypeMethod(tpl, "schnorrBatchVerify", BSecp256k1::SchnorrBatchVerify);

  v8::Local<v8::FunctionTemplate> ctor =
    Nan::New<v8::FunctionTemplate>(secp256k1_constructor);

  Nan::Set(target, Nan::New("Secp256k1").ToLocalChecked(),
    Nan::GetFunction(ctor).ToLocalChecked());
}

NAN_METHOD(BSecp256k1::New) {
  if (!info.IsConstructCall())
    return Nan::ThrowError("Could not create Secp256k1 instance.");

  secp256k1_context *ctx = secp256k1_context_create(
    SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

  if (ctx == NULL)
    return Nan::ThrowError("Could not create Secp256k1 instance.");

  // Use blinded multiplication as a final
  // defense against side-channel attacks.
  uint8_t seed[32];

  if (bcrypto_random(&seed[0], 32)) {
    if (secp256k1_context_randomize(ctx, seed) == 0) {
      secp256k1_context_destroy(ctx);
      return Nan::ThrowError("Could not randomize Secp256k1 instance.");
    }
  }

  BSecp256k1 *secp = new BSecp256k1();
  secp->ctx = ctx;
  secp->Wrap(info.This());

  info.GetReturnValue().Set(info.This());
}

NAN_METHOD(BSecp256k1::PrivateKeyVerify) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_key_buffer);

  if (node::Buffer::Length(private_key_buffer) != 32)
    return info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));

  int result = secp256k1_ec_seckey_verify(secp->ctx, private_key);

  info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::PrivateKeyExport) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_key_buffer);

  int compressed = 1;
  UPDATE_COMPRESSED_VALUE(compressed, info[1], 1, 0);

  unsigned char output[279];
  size_t output_length;

  if (ec_privkey_export_der(secp->ctx, &output[0], &output_length,
                            private_key, compressed) == 0) {
    return Nan::ThrowError(EC_PRIVATE_KEY_EXPORT_DER_FAIL);
  }

  info.GetReturnValue().Set(COPY_BUFFER(output, output_length));
}

NAN_METHOD(BSecp256k1::PrivateKeyImport) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH_GT_ZERO(input_buffer, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *input =
    (const unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  unsigned char private_key[32];

  if (ec_privkey_import_der(secp->ctx, &private_key[0],
                            input, input_length) == 0) {
    return Nan::ThrowError(EC_PRIVATE_KEY_IMPORT_DER_FAIL);
  }

  info.GetReturnValue().Set(COPY_BUFFER(private_key, 32));
}

NAN_METHOD(BSecp256k1::PrivateKeyReduce) {
  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);

  unsigned char private_key[32];

  const unsigned char *data =
    (const unsigned char *)node::Buffer::Data(private_key_buffer);

  size_t len = node::Buffer::Length(private_key_buffer);

  memset(&private_key[0], 0x00, 32);

  if (len > 32)
    len = 32;

  memcpy(&private_key[32 - len], data, len);

  secp256k1_scalar s;
  int overflow = 0;

  secp256k1_scalar_set_b32(&s, private_key, &overflow);
  secp256k1_scalar_get_b32(private_key, &s);
  secp256k1_scalar_clear(&s);

  info.GetReturnValue().Set(COPY_BUFFER(&private_key[0], 32));
}

NAN_METHOD(BSecp256k1::PrivateKeyNegate) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  unsigned char private_key[32];
  memcpy(&private_key[0], node::Buffer::Data(private_key_buffer), 32);

  secp256k1_ec_privkey_negate(secp->ctx, &private_key[0]);

  info.GetReturnValue().Set(COPY_BUFFER(&private_key[0], 32));
}

NAN_METHOD(BSecp256k1::PrivateKeyInvert) {
  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  unsigned char private_key[32];
  memcpy(&private_key[0], node::Buffer::Data(private_key_buffer), 32);

  secp256k1_scalar s;
  int overflow = 0;

  secp256k1_scalar_set_b32(&s, private_key, &overflow);

  if (overflow || secp256k1_scalar_is_zero(&s)) {
    secp256k1_scalar_clear(&s);
    return Nan::ThrowError(EC_PRIVATE_KEY_RANGE_INVALID);
  }

  secp256k1_scalar_inverse(&s, &s);

  secp256k1_scalar_get_b32(private_key, &s);
  secp256k1_scalar_clear(&s);

  info.GetReturnValue().Set(COPY_BUFFER(&private_key[0], 32));
}

NAN_METHOD(BSecp256k1::PrivateKeyTweakAdd) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  unsigned char private_key[32];
  memcpy(&private_key[0], node::Buffer::Data(private_key_buffer), 32);

  v8::Local<v8::Object> tweak_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(tweak_buffer, TWEAK_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(tweak_buffer, 32, TWEAK_LENGTH_INVALID);

  const unsigned char *tweak =
    (unsigned char *)node::Buffer::Data(tweak_buffer);

  if (secp256k1_ec_privkey_tweak_add(secp->ctx, &private_key[0], tweak) == 0)
    return Nan::ThrowError(EC_PRIVATE_KEY_TWEAK_ADD_FAIL);

  info.GetReturnValue().Set(COPY_BUFFER(&private_key[0], 32));
}

NAN_METHOD(BSecp256k1::PrivateKeyTweakMul) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  unsigned char private_key[32];
  memcpy(&private_key[0], node::Buffer::Data(private_key_buffer), 32);

  v8::Local<v8::Object> tweak_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(tweak_buffer, TWEAK_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(tweak_buffer, 32, TWEAK_LENGTH_INVALID);

  const unsigned char *tweak =
    (unsigned char *)node::Buffer::Data(tweak_buffer);

  if (secp256k1_ec_privkey_tweak_mul(secp->ctx, &private_key[0], tweak) == 0)
    return Nan::ThrowError(EC_PRIVATE_KEY_TWEAK_MUL_FAIL);

  info.GetReturnValue().Set(COPY_BUFFER(&private_key[0], 32));
}

NAN_METHOD(BSecp256k1::PublicKeyCreate) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> private_key_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_key_buffer);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[1], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_create(secp->ctx, &public_key, private_key) == 0)
    return Nan::ThrowError(EC_PUBLIC_KEY_CREATE_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::PublicKeyConvert) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(input_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[1], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key,
                                input, input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  unsigned char output[65];
  size_t output_length = 65;
  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::PublicKeyVerify) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, EC_PUBLIC_KEY_TYPE_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  secp256k1_pubkey public_key;

  int result = secp256k1_ec_pubkey_parse(secp->ctx, &public_key,
                                         input, input_length);

  info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::PublicKeyTweakAdd) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(input_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  v8::Local<v8::Object> tweak_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(tweak_buffer, TWEAK_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(tweak_buffer, 32, TWEAK_LENGTH_INVALID);

  const unsigned char *tweak =
    (const unsigned char *)node::Buffer::Data(tweak_buffer);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[2], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key,
                                input, input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  if (secp256k1_ec_pubkey_tweak_add(secp->ctx, &public_key, tweak) == 0)
    return Nan::ThrowError(EC_PUBLIC_KEY_TWEAK_ADD_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::PublicKeyTweakMul) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(input_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  v8::Local<v8::Object> tweak_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(tweak_buffer, TWEAK_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(tweak_buffer, 32, TWEAK_LENGTH_INVALID);

  const unsigned char *tweak =
    (const unsigned char *)node::Buffer::Data(tweak_buffer);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[2], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key,
                                input, input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  if (secp256k1_ec_pubkey_tweak_mul(secp->ctx, &public_key, tweak) == 0)
    return Nan::ThrowError(EC_PUBLIC_KEY_TWEAK_MUL_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::PublicKeyAdd) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input1_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input1_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(input1_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *input1 =
    (unsigned char *)node::Buffer::Data(input1_buffer);
  size_t input1_length = node::Buffer::Length(input1_buffer);

  v8::Local<v8::Object> input2_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(input2_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(input2_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *input2 =
    (unsigned char *)node::Buffer::Data(input2_buffer);
  size_t input2_length = node::Buffer::Length(input2_buffer);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[2], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_pubkey public_key1;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key1,
                                input1, input1_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  secp256k1_pubkey public_key2;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key2,
                                input2, input2_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  secp256k1_pubkey *ins[2];
  ins[0] = &public_key1;
  ins[1] = &public_key2;

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_combine(secp->ctx, &public_key, ins, 2) == 0)
    return Nan::ThrowError(EC_PUBLIC_KEY_COMBINE_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::PublicKeyCombine) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Array> input_buffers = info[0].As<v8::Array>();
  CHECK_TYPE_ARRAY(input_buffers, EC_PUBLIC_KEYS_TYPE_INVALID);
  CHECK_LENGTH_GT_ZERO(input_buffers, EC_PUBLIC_KEYS_LENGTH_INVALID);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[1], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  std::unique_ptr<secp256k1_pubkey[]>
    public_keys(new secp256k1_pubkey[input_buffers->Length()]);

  std::unique_ptr<secp256k1_pubkey*[]>
    ins(new secp256k1_pubkey*[input_buffers->Length()]);

  for (unsigned int i = 0; i < input_buffers->Length(); ++i) {
    v8::Local<v8::Object> public_key_buffer =
      Nan::Get(input_buffers, i).ToLocalChecked().As<v8::Object>();

    CHECK_TYPE_BUFFER(public_key_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
    CHECK_BUFFER_LENGTH2(public_key_buffer, 33, 65,
                         EC_PUBLIC_KEY_LENGTH_INVALID);

    const unsigned char *input =
      (unsigned char *)node::Buffer::Data(public_key_buffer);
    size_t input_length = node::Buffer::Length(public_key_buffer);

    if (secp256k1_ec_pubkey_parse(secp->ctx, &public_keys[i],
                                  input, input_length) == 0) {
      return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
    }

    ins[i] = &public_keys[i];
  }

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_combine(secp->ctx, &public_key, ins.get(),
                                  input_buffers->Length()) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_COMBINE_FAIL);
  }

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::PublicKeyNegate) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(input_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[1], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key,
                                input, input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  if (secp256k1_ec_pubkey_negate(secp->ctx, &public_key) == 0)
    return Nan::ThrowError(EC_PUBLIC_KEY_NEGATE_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::SignatureNormalize) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(input_buffer, 64, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);

  secp256k1_ecdsa_signature sigin;

  if (secp256k1_ecdsa_signature_parse_compact(secp->ctx, &sigin, input) == 0)
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);

  secp256k1_ecdsa_signature sigout;
  secp256k1_ecdsa_signature_normalize(secp->ctx, &sigout, &sigin);

  unsigned char output[64];

  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &output[0], &sigout);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], 64));
}

NAN_METHOD(BSecp256k1::SignatureNormalizeDER) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH_GT_ZERO(input_buffer, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *input =
    (const unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  secp256k1_ecdsa_signature sigin;

  if (ecdsa_signature_parse_der_lax(secp->ctx, &sigin,
                                    input, input_length) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_DER_FAIL);
  }

  secp256k1_ecdsa_signature sigout;
  secp256k1_ecdsa_signature_normalize(secp->ctx, &sigout, &sigin);

  unsigned char output[72];
  size_t output_length = 72;

  if (secp256k1_ecdsa_signature_serialize_der(secp->ctx, &output[0],
                                              &output_length, &sigout) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_SERIALIZE_DER_FAIL);
  }

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::SignatureExport) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(input_buffer, 64, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *input =
    (unsigned char *)node::Buffer::Data(input_buffer);

  secp256k1_ecdsa_signature sig;

  if (secp256k1_ecdsa_signature_parse_compact(secp->ctx, &sig, input) == 0)
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);

  unsigned char output[72];
  size_t output_length = 72;

  if (secp256k1_ecdsa_signature_serialize_der(secp->ctx, &output[0],
                                              &output_length, &sig) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_SERIALIZE_DER_FAIL);
  }

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::SignatureImport) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH_GT_ZERO(input_buffer, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *input =
    (const unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  secp256k1_ecdsa_signature sig;

  if (ecdsa_signature_parse_der_lax(secp->ctx, &sig,
                                    input, input_length) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_DER_FAIL);
  }

  unsigned char output[64];

  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &output[0], &sig);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], 64));
}

NAN_METHOD(BSecp256k1::IsLowS) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);

  const unsigned char *input =
    (const unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  if (input_length != 64)
    return info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));

  secp256k1_ecdsa_signature sig;

  if (secp256k1_ecdsa_signature_parse_compact(secp->ctx, &sig, input) == 0)
    return info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));

  unsigned char output[64];

  secp256k1_ecdsa_signature_normalize(secp->ctx, &sig, &sig);
  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &output[0], &sig);

  int result = memcmp(&input[32], &output[32], 32) == 0;

  return info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::IsLowDER) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> input_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);

  const unsigned char *input =
    (const unsigned char *)node::Buffer::Data(input_buffer);
  size_t input_length = node::Buffer::Length(input_buffer);

  if (input_length == 0)
    return info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));

  secp256k1_ecdsa_signature sig;

  if (ecdsa_signature_parse_der_lax(secp->ctx, &sig, input, input_length) == 0)
    return info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));

  unsigned char input_[64];
  unsigned char output[64];

  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &input_[0], &sig);
  secp256k1_ecdsa_signature_normalize(secp->ctx, &sig, &sig);
  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &output[0], &sig);

  int result = memcmp(&input_[32], &output[32], 32) == 0;

  return info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::Sign) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> private_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_buffer);

  secp256k1_nonce_function noncefn = secp256k1_nonce_function_rfc6979;
  void *data = NULL;
  secp256k1_ecdsa_signature sig;

  if (secp256k1_ecdsa_sign(secp->ctx, &sig, msg32,
                           private_key, noncefn, data) == 0) {
    return Nan::ThrowError(ECDSA_SIGN_FAIL);
  }

  unsigned char output[64];

  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &output[0], &sig);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], 64));
}

NAN_METHOD(BSecp256k1::SignRecoverable) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> private_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_buffer);

  secp256k1_nonce_function noncefn = secp256k1_nonce_function_rfc6979;
  void* data = NULL;
  secp256k1_ecdsa_recoverable_signature sig;

  if (secp256k1_ecdsa_sign_recoverable(secp->ctx, &sig, msg32,
                                       private_key, noncefn, data) == 0) {
    return Nan::ThrowError(ECDSA_SIGN_FAIL);
  }

  int recid;
  unsigned char output[64];

  secp256k1_ecdsa_recoverable_signature_serialize_compact(secp->ctx, &output[0],
                                                          &recid, &sig);

  v8::Local<v8::Array> ret = Nan::New<v8::Array>();

  Nan::Set(ret, 0, COPY_BUFFER(&output[0], 64));
  Nan::Set(ret, 1, Nan::New<v8::Number>(recid));

  info.GetReturnValue().Set(ret);
}

NAN_METHOD(BSecp256k1::SignDER) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> private_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_buffer);

  secp256k1_nonce_function noncefn = secp256k1_nonce_function_rfc6979;
  void* data = NULL;
  secp256k1_ecdsa_signature sig;

  if (secp256k1_ecdsa_sign(secp->ctx, &sig, msg32,
                           private_key, noncefn, data) == 0) {
    return Nan::ThrowError(ECDSA_SIGN_FAIL);
  }

  unsigned char output[72];
  size_t output_length = 72;

  if (secp256k1_ecdsa_signature_serialize_der(secp->ctx, &output[0],
                                              &output_length, &sig) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_SERIALIZE_DER_FAIL);
  }

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::SignRecoverableDER) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> private_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_buffer);

  secp256k1_nonce_function noncefn = secp256k1_nonce_function_rfc6979;
  void* data = NULL;
  secp256k1_ecdsa_recoverable_signature sig;

  if (secp256k1_ecdsa_sign_recoverable(secp->ctx, &sig, msg32,
                                       private_key, noncefn, data) == 0) {
    return Nan::ThrowError(ECDSA_SIGN_FAIL);
  }

  int recid;
  unsigned char output[72];
  size_t output_length = 72;

  secp256k1_ecdsa_recoverable_signature_serialize_compact(secp->ctx, &output[0],
                                                          &recid, &sig);

  secp256k1_ecdsa_signature sig_;

  if (secp256k1_ecdsa_signature_parse_compact(secp->ctx, &sig_, &output[0]) == 0)
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);

  if (secp256k1_ecdsa_signature_serialize_der(secp->ctx, &output[0],
                                              &output_length, &sig_) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_SERIALIZE_DER_FAIL);
  }

  v8::Local<v8::Array> ret = Nan::New<v8::Array>();

  Nan::Set(ret, 0, COPY_BUFFER(&output[0], output_length));
  Nan::Set(ret, 1, Nan::New<v8::Number>(recid));

  info.GetReturnValue().Set(ret);
}

NAN_METHOD(BSecp256k1::Verify) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> sig_input_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(sig_input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(sig_input_buffer, 64, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *sig_input =
    (unsigned char *)node::Buffer::Data(sig_input_buffer);

  v8::Local<v8::Object> public_key_buffer = info[2].As<v8::Object>();
  CHECK_TYPE_BUFFER(public_key_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(public_key_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *public_key_input =
    (unsigned char *)node::Buffer::Data(public_key_buffer);
  size_t public_key_input_length = node::Buffer::Length(public_key_buffer);

  secp256k1_ecdsa_signature sig;

  if (secp256k1_ecdsa_signature_parse_compact(secp->ctx, &sig, sig_input) == 0)
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key, public_key_input,
                                public_key_input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  // Normalize signature (ensure low S value).
  secp256k1_ecdsa_signature_normalize(secp->ctx, &sig, &sig);

  int result = secp256k1_ecdsa_verify(secp->ctx, &sig, msg32, &public_key);

  info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::VerifyDER) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> sig_input_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(sig_input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH_GT_ZERO(sig_input_buffer, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *sig_input =
    (unsigned char *)node::Buffer::Data(sig_input_buffer);
  size_t sig_input_length = node::Buffer::Length(sig_input_buffer);

  v8::Local<v8::Object> public_key_buffer = info[2].As<v8::Object>();
  CHECK_TYPE_BUFFER(public_key_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(public_key_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *public_key_input =
    (unsigned char *)node::Buffer::Data(public_key_buffer);
  size_t public_key_input_length = node::Buffer::Length(public_key_buffer);

  secp256k1_ecdsa_signature sig;

  if (ecdsa_signature_parse_der_lax(secp->ctx, &sig,
                                    sig_input, sig_input_length) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_DER_FAIL);
  }

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key, public_key_input,
                                public_key_input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  // Normalize signature (ensure low S value).
  secp256k1_ecdsa_signature_normalize(secp->ctx, &sig, &sig);

  int result = secp256k1_ecdsa_verify(secp->ctx, &sig, msg32, &public_key);

  info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::Recover) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> sig_input_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(sig_input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(sig_input_buffer, 64, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *sig_input =
    (unsigned char *)node::Buffer::Data(sig_input_buffer);

  v8::Local<v8::Object> recid_object = info[2].As<v8::Object>();
  CHECK_TYPE_NUMBER(recid_object, RECOVERY_ID_TYPE_INVALID);
  CHECK_NUMBER_IN_INTERVAL(recid_object, -1, 4, RECOVERY_ID_VALUE_INVALID);

  int recid = (int)Nan::To<int64_t>(recid_object).FromJust();

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[3], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_ecdsa_recoverable_signature sig;

  if (secp256k1_ecdsa_recoverable_signature_parse_compact(secp->ctx,
                                                          &sig,
                                                          sig_input,
                                                          recid) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);
  }

  secp256k1_pubkey public_key;

  if (secp256k1_ecdsa_recover(secp->ctx, &public_key, &sig, msg32) == 0)
    return Nan::ThrowError(ECDSA_RECOVER_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::RecoverDER) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> sig_input_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(sig_input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH_GT_ZERO(sig_input_buffer, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *sig_input =
    (unsigned char *)node::Buffer::Data(sig_input_buffer);
  size_t sig_input_length = node::Buffer::Length(sig_input_buffer);

  v8::Local<v8::Object> recid_object = info[2].As<v8::Object>();
  CHECK_TYPE_NUMBER(recid_object, RECOVERY_ID_TYPE_INVALID);
  CHECK_NUMBER_IN_INTERVAL(recid_object, -1, 4, RECOVERY_ID_VALUE_INVALID);

  int recid = (int)Nan::To<int64_t>(recid_object).FromJust();

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[3], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  secp256k1_ecdsa_signature sig_;

  if (ecdsa_signature_parse_der_lax(secp->ctx, &sig_,
                                    sig_input, sig_input_length) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_DER_FAIL);
  }

  unsigned char sig_input_[64];

  secp256k1_ecdsa_signature_serialize_compact(secp->ctx, &sig_input_[0], &sig_);

  secp256k1_ecdsa_recoverable_signature sig;

  if (secp256k1_ecdsa_recoverable_signature_parse_compact(secp->ctx,
                                                          &sig,
                                                          sig_input_,
                                                          recid) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);
  }

  secp256k1_pubkey public_key;

  if (secp256k1_ecdsa_recover(secp->ctx, &public_key, &sig, msg32) == 0)
    return Nan::ThrowError(ECDSA_RECOVER_FAIL);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ec_pubkey_serialize(secp->ctx, &output[0], &output_length,
                                &public_key, flags);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

static int
ecdh_hash_function_raw(unsigned char *output,
                       const unsigned char *x,
                       const unsigned char *y,
                       void *data) {
  unsigned int flags = *((unsigned int *)data);

  if (flags == SECP256K1_EC_COMPRESSED) {
    output[0] = 0x02 | (y[31] & 1);
    memcpy(&output[1], &x[0], 32);
  } else {
    output[0] = 0x04;
    memcpy(&output[1], &x[0], 32);
    memcpy(&output[33], &y[0], 32);
  }

  return 1;
}

NAN_METHOD(BSecp256k1::Derive) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> pubkey_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(pubkey_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(pubkey_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *public_key_input =
    (unsigned char *)node::Buffer::Data(pubkey_buffer);
  size_t public_key_input_length = node::Buffer::Length(pubkey_buffer);

  v8::Local<v8::Object> private_key_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_key_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_key_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_key_buffer);

  secp256k1_pubkey public_key;

  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key, public_key_input,
                                public_key_input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  unsigned int flags = SECP256K1_EC_COMPRESSED;
  UPDATE_COMPRESSED_VALUE(flags, info[2], SECP256K1_EC_COMPRESSED,
                                          SECP256K1_EC_UNCOMPRESSED);

  unsigned char output[65];
  size_t output_length = 65;

  secp256k1_ecdh_hash_function hashfp = ecdh_hash_function_raw;

  if (!secp256k1_ecdh(secp->ctx, &output[0],
                      &public_key, private_key, hashfp, &flags)) {
    return Nan::ThrowError(ECDH_FAIL);
  }

  if (output[0] != 0x04)
    output_length = 33;

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], output_length));
}

NAN_METHOD(BSecp256k1::SchnorrSign) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> private_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(private_buffer, EC_PRIVATE_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(private_buffer, 32, EC_PRIVATE_KEY_LENGTH_INVALID);

  const unsigned char *private_key =
    (const unsigned char *)node::Buffer::Data(private_buffer);

  secp256k1_nonce_function noncefn = NULL;
  void *data = NULL;
  secp256k1_schnorrsig sig;

  if (secp256k1_schnorrsig_sign(secp->ctx, &sig, NULL, msg32,
                                private_key, noncefn, data) == 0) {
    return Nan::ThrowError(ECDSA_SIGN_FAIL);
  }

  unsigned char output[64];

  secp256k1_schnorrsig_serialize(secp->ctx, &output[0], &sig);

  info.GetReturnValue().Set(COPY_BUFFER(&output[0], 64));
}

NAN_METHOD(BSecp256k1::SchnorrVerify) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  v8::Local<v8::Object> msg32_buffer = info[0].As<v8::Object>();
  CHECK_TYPE_BUFFER(msg32_buffer, MSG32_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(msg32_buffer, 32, MSG32_LENGTH_INVALID);

  const unsigned char *msg32 =
    (const unsigned char *)node::Buffer::Data(msg32_buffer);

  v8::Local<v8::Object> sig_input_buffer = info[1].As<v8::Object>();
  CHECK_TYPE_BUFFER(sig_input_buffer, ECDSA_SIGNATURE_TYPE_INVALID);
  CHECK_BUFFER_LENGTH(sig_input_buffer, 64, ECDSA_SIGNATURE_LENGTH_INVALID);

  const unsigned char *sig_input =
    (unsigned char *)node::Buffer::Data(sig_input_buffer);

  v8::Local<v8::Object> public_key_buffer = info[2].As<v8::Object>();
  CHECK_TYPE_BUFFER(public_key_buffer, EC_PUBLIC_KEY_TYPE_INVALID);
  CHECK_BUFFER_LENGTH2(public_key_buffer, 33, 65, EC_PUBLIC_KEY_LENGTH_INVALID);

  const unsigned char *public_key_input =
    (unsigned char *)node::Buffer::Data(public_key_buffer);
  size_t public_key_input_length = node::Buffer::Length(public_key_buffer);

  secp256k1_schnorrsig sig;
  if (secp256k1_schnorrsig_parse(secp->ctx, &sig, sig_input) == 0) {
    return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);
  }

  secp256k1_pubkey public_key;
  if (secp256k1_ec_pubkey_parse(secp->ctx, &public_key, public_key_input,
                                public_key_input_length) == 0) {
    return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
  }

  int result = secp256k1_schnorrsig_verify(secp->ctx, &sig, msg32, &public_key);

  info.GetReturnValue().Set(Nan::New<v8::Boolean>(result));
}

NAN_METHOD(BSecp256k1::SchnorrBatchVerify) {
  BSecp256k1 *secp = ObjectWrap::Unwrap<BSecp256k1>(info.Holder());

  if (!info[0]->IsArray())
    return Nan::ThrowTypeError("batch should be an Array");

  v8::Local<v8::Array> batch = info[0].As<v8::Array>();

  size_t len = (size_t)batch->Length();

  if (len == 0)
    return info.GetReturnValue().Set(Nan::New<v8::Boolean>(true));

  const uint8_t **msgs =
    (const uint8_t **)malloc(len * sizeof(const uint8_t **));

  if (msgs == NULL)
    return Nan::ThrowError("allocation failed");

  secp256k1_schnorrsig *sigs =
    (secp256k1_schnorrsig *)malloc(len * sizeof(secp256k1_schnorrsig));

  if (sigs == NULL) {
    free(msgs);
    return Nan::ThrowError("allocation failed");
  }

  secp256k1_pubkey *pubs =
    (secp256k1_pubkey *)malloc(len * sizeof(secp256k1_pubkey));

  if (pubs == NULL) {
    free(sigs);
    free(msgs);
    return Nan::ThrowError("allocation failed");
  }

#define FREE_BATCH (free(msgs), free(sigs), free(pubs))

  for (size_t i = 0; i < len; i++) {
    if (!Nan::Get(batch, i).ToLocalChecked()->IsArray()) {
      FREE_BATCH;
      return Nan::ThrowTypeError("batch item should be an Array");
    }

    v8::Local<v8::Array> item = Nan::Get(batch, i).ToLocalChecked()
                                                  .As<v8::Array>();

    if (item->Length() != 3) {
      FREE_BATCH;
      return Nan::ThrowTypeError("batch item must consist of 3 members");
    }

    v8::Local<v8::Object> mbuf = Nan::Get(item, 0).ToLocalChecked()
                                                  .As<v8::Object>();
    v8::Local<v8::Object> sbuf = Nan::Get(item, 1).ToLocalChecked()
                                                  .As<v8::Object>();
    v8::Local<v8::Object> pbuf = Nan::Get(item, 2).ToLocalChecked()
                                                  .As<v8::Object>();

    if (!node::Buffer::HasInstance(mbuf)) {
      FREE_BATCH;
      return Nan::ThrowTypeError(MSG32_TYPE_INVALID);
    }

    if (!node::Buffer::HasInstance(sbuf)) {
      FREE_BATCH;
      return Nan::ThrowTypeError(ECDSA_SIGNATURE_TYPE_INVALID);
    }

    if (!node::Buffer::HasInstance(pbuf)) {
      FREE_BATCH;
      return Nan::ThrowTypeError(EC_PUBLIC_KEY_TYPE_INVALID);
    }

    const uint8_t *msg = (const uint8_t *)node::Buffer::Data(mbuf);
    size_t msg_len = node::Buffer::Length(mbuf);

    const uint8_t *sig = (const uint8_t *)node::Buffer::Data(sbuf);
    size_t sig_len = node::Buffer::Length(sbuf);

    const uint8_t *pub = (const uint8_t *)node::Buffer::Data(pbuf);
    size_t pub_len = node::Buffer::Length(pbuf);

    if (msg_len != 32) {
      FREE_BATCH;
      return Nan::ThrowRangeError(MSG32_LENGTH_INVALID);
    }

    if (sig_len != 64) {
      FREE_BATCH;
      return Nan::ThrowRangeError(ECDSA_SIGNATURE_LENGTH_INVALID);
    }

    if (pub_len != 33 && pub_len != 65) {
      FREE_BATCH;
      return Nan::ThrowRangeError(EC_PUBLIC_KEY_LENGTH_INVALID);
    }

    msgs[i] = msg;

    if (secp256k1_schnorrsig_parse(secp->ctx, &sigs[i], sig) == 0) {
      FREE_BATCH;
      return Nan::ThrowError(ECDSA_SIGNATURE_PARSE_FAIL);
    }

    if (secp256k1_ec_pubkey_parse(secp->ctx, &pubs[i], pub, pub_len) == 0) {
      FREE_BATCH;
      return Nan::ThrowError(EC_PUBLIC_KEY_PARSE_FAIL);
    }
  }

  // Todo: investigate scratch API:
  // size_t size1 = secp256k1_strauss_scratch_size(n_points);
  // size_t size2 = secp256k1_pippenger_scratch_size(n_points,
  //                  secp256k1_pippenger_bucket_window(n_points));
  // size_t max_size = size1 > size2 ? size1 : size2;
  // secp256k1_scratch_space *scratch =
  //   secp256k1_scratch_space_create(secp->ctx, max_size);
  // secp256k1_scratch_space_destroy(scratch);
  //
  // int result = secp256k1_schnorrsig_verify_batch(secp->ctx, NULL,
  //                                                sigs, msgs, pubs, len);

  int result = 1;
  size_t i = 0;

  for (; i < len; i++) {
    if (secp256k1_schnorrsig_verify(secp->ctx, &sigs[i],
                                    msgs[i], &pubs[i]) == 0) {
      result = 0;
      break;
    }
  }

  FREE_BATCH;

#undef FREE_BATCH

  info.GetReturnValue().Set(Nan::New<v8::Boolean>(result == 1));
}
