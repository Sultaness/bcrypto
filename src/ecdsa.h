#ifndef _BCRYPTO_ECDSA_HH
#define _BCRYPTO_ECDSA_HH

#include "compat.h"

#ifdef BCRYPTO_HAS_ECDSA

#include <node.h>
#include <nan.h>
#include "ecdsa/ecdsa.h"

class BECDSA : public Nan::ObjectWrap {
public:
  static NAN_METHOD(New);
  static void Init(v8::Local<v8::Object> &target);

  BECDSA();
  ~BECDSA();

  bcrypto_ecdsa_t ctx;

private:
  static NAN_METHOD(Size);
  static NAN_METHOD(Bits);
  static NAN_METHOD(PrivateKeyGenerate);
  static NAN_METHOD(PrivateKeyVerify);
  static NAN_METHOD(PrivateKeyExport);
  static NAN_METHOD(PrivateKeyImport);
  static NAN_METHOD(PrivateKeyExportPKCS8);
  static NAN_METHOD(PrivateKeyImportPKCS8);
  static NAN_METHOD(PrivateKeyTweakAdd);
  static NAN_METHOD(PrivateKeyTweakMul);
  static NAN_METHOD(PrivateKeyReduce);
  static NAN_METHOD(PrivateKeyNegate);
  static NAN_METHOD(PrivateKeyInvert);
  static NAN_METHOD(PublicKeyCreate);
  static NAN_METHOD(PublicKeyConvert);
  static NAN_METHOD(PublicKeyVerify);
  static NAN_METHOD(PublicKeyExportSPKI);
  static NAN_METHOD(PublicKeyImportSPKI);
  static NAN_METHOD(PublicKeyTweakAdd);
  static NAN_METHOD(PublicKeyTweakMul);
  static NAN_METHOD(PublicKeyAdd);
  static NAN_METHOD(PublicKeyCombine);
  static NAN_METHOD(PublicKeyNegate);
  static NAN_METHOD(SignatureNormalize);
  static NAN_METHOD(SignatureNormalizeDER);
  static NAN_METHOD(SignatureExport);
  static NAN_METHOD(SignatureImport);
  static NAN_METHOD(IsLowS);
  static NAN_METHOD(IsLowDER);
  static NAN_METHOD(Sign);
  static NAN_METHOD(SignRecoverable);
  static NAN_METHOD(SignDER);
  static NAN_METHOD(SignRecoverableDER);
  static NAN_METHOD(Verify);
  static NAN_METHOD(VerifyDER);
  static NAN_METHOD(Recover);
  static NAN_METHOD(RecoverDER);
  static NAN_METHOD(Derive);
  static NAN_METHOD(SchnorrSign);
  static NAN_METHOD(SchnorrVerify);
  static NAN_METHOD(SchnorrVerifyBatch);
};
#endif

#endif
