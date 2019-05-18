#ifndef _BCRYPTO_CIPHER_HH
#define _BCRYPTO_CIPHER_HH

#include <node.h>
#include <nan.h>

#include "hash/cipher.h"

class BCipherBase : public Nan::ObjectWrap {
public:
  static NAN_METHOD(New);
  static void Init(v8::Local<v8::Object> &target);

  BCipherBase();
  ~BCipherBase();

  int type;
  int mode;
  int encrypt;
  int first;
  int done;
  uint8_t tag[16];
  size_t tag_len;
  bcrypto_cipher_t ctx;

private:
  static NAN_METHOD(Init);
  static NAN_METHOD(Update);
  static NAN_METHOD(Final);
  static NAN_METHOD(SetAAD);
  static NAN_METHOD(GetAuthTag);
  static NAN_METHOD(SetAuthTag);
  static NAN_METHOD(HasCipher);
};
#endif
