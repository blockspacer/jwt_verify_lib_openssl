// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jwt_verify_lib/verify.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "jwt_verify_lib/check_audience.h"

#include "openssl/bn.h"
#include "openssl/ecdsa.h"
#include "openssl/err.h"
#include "openssl/evp.h"
#include "openssl/hmac.h"
//#include "openssl/mem.h"
#include "openssl/rsa.h"
#include "openssl/sha.h"

namespace google {
namespace jwt_verify {
namespace {

// A convenience inline cast function.
inline const uint8_t* castToUChar(const absl::string_view& str) {
  return reinterpret_cast<const uint8_t*>(str.data());
}

bool verifySignatureRSA(RSA* key, const EVP_MD* md, const uint8_t* signature,
                        size_t signature_len, const uint8_t* signed_data,
                        size_t signed_data_len) {
  if (key == nullptr || md == nullptr || signature == nullptr ||
      signed_data == nullptr) {
    return false;
  }
  bssl::UniquePtr<EVP_PKEY> evp_pkey(EVP_PKEY_new());
  if (EVP_PKEY_set1_RSA(evp_pkey.get(), key) != 1) {
    return false;
  }

  bssl::UniquePtr<EVP_MD_CTX> md_ctx(EVP_MD_CTX_create());
  if (EVP_DigestVerifyInit(md_ctx.get(), nullptr, md, nullptr,
                           evp_pkey.get()) == 1) {
    if (EVP_DigestVerifyUpdate(md_ctx.get(), signed_data, signed_data_len) ==
        1) {
      if (EVP_DigestVerifyFinal(md_ctx.get(), signature, signature_len) == 1) {
        return true;
      }
    }
  }
  ERR_clear_error();
  return false;
}

bool verifySignatureRSA(RSA* key, const EVP_MD* md, absl::string_view signature,
                        absl::string_view signed_data) {
  return verifySignatureRSA(key, md, castToUChar(signature), signature.length(),
                            castToUChar(signed_data), signed_data.length());
}

bool verifySignatureEC(EC_KEY* key, const EVP_MD* md, const uint8_t* signature,
                       size_t signature_len, const uint8_t* signed_data,
                       size_t signed_data_len) {
  if (key == nullptr || md == nullptr || signature == nullptr ||
      signed_data == nullptr) {
    return false;
  }
  bssl::UniquePtr<EVP_MD_CTX> md_ctx(EVP_MD_CTX_create());
  std::vector<uint8_t> digest(EVP_MAX_MD_SIZE);
  unsigned int digest_len = 0;

  if (EVP_DigestInit(md_ctx.get(), md) == 0) {
    return false;
  }

  if (EVP_DigestUpdate(md_ctx.get(), signed_data, signed_data_len) == 0) {
    return false;
  }

  if (EVP_DigestFinal(md_ctx.get(), digest.data(), &digest_len) == 0) {
    return false;
  }

  bssl::UniquePtr<ECDSA_SIG> ecdsa_sig(ECDSA_SIG_new());
  if (!ecdsa_sig) {
    return false;
  }

  //if (BN_bin2bn(signature, signature_len / 2, ecdsa_sig->r) == nullptr ||
  //    BN_bin2bn(signature + (signature_len / 2), signature_len / 2,
  //              ecdsa_sig->s) == nullptr) {
   const BIGNUM* r_bn;
   const BIGNUM* s_bn;
   ECDSA_SIG_get0(ecdsa_sig.get(), &r_bn, &s_bn);
   if (BN_bin2bn(signature, 32, const_cast<BIGNUM *>(r_bn)) == nullptr ||
       BN_bin2bn(signature + 32, 32, const_cast<BIGNUM *>(s_bn)) == nullptr) {
    return false;
  }

  if (ECDSA_do_verify(digest.data(), digest_len, ecdsa_sig.get(), key) == 1) {
    return true;
  }

  ERR_clear_error();
  return false;
}

bool verifySignatureEC(EC_KEY* key, const EVP_MD* md,
                       absl::string_view signature,
                       absl::string_view signed_data) {
  return verifySignatureEC(key, md, castToUChar(signature), signature.length(),
                           castToUChar(signed_data), signed_data.length());
}

bool verifySignatureOct(const uint8_t* key, size_t key_len, const EVP_MD* md,
                        const uint8_t* signature, size_t signature_len,
                        const uint8_t* signed_data, size_t signed_data_len) {
  if (key == nullptr || md == nullptr || signature == nullptr ||
      signed_data == nullptr) {
    return false;
  }

  std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
  unsigned int out_len = 0;
  if (HMAC(md, key, key_len, signed_data, signed_data_len, out.data(),
           &out_len) == nullptr) {
    ERR_clear_error();
    return false;
  }

  if (out_len != signature_len) {
    return false;
  }

  if (CRYPTO_memcmp(out.data(), signature, signature_len) == 0) {
    return true;
  }

  ERR_clear_error();
  return false;
}

bool verifySignatureOct(absl::string_view key, const EVP_MD* md,
                        absl::string_view signature,
                        absl::string_view signed_data) {
  return verifySignatureOct(castToUChar(key), key.length(), md,
                            castToUChar(signature), signature.length(),
                            castToUChar(signed_data), signed_data.length());
}

}  // namespace

Status verifyJwt(const Jwt& jwt, const Jwks& jwks) {
  return verifyJwt(jwt, jwks, absl::ToUnixSeconds(absl::Now()));
}

Status verifyJwt(const Jwt& jwt, const Jwks& jwks, uint64_t now) {
  // First check that the JWT has not expired (exp) and is active (nbf).
  if (now < jwt.nbf_) {
    return Status::JwtNotYetValid;
  }
  if (jwt.exp_ && now > jwt.exp_) {
    return Status::JwtExpired;
  }

  // Second verify signature.z
  std::string signed_data =
      jwt.header_str_base64url_ + '.' + jwt.payload_str_base64url_;
  bool kid_alg_matched = false;
  for (const auto& jwk : jwks.keys()) {
    // If kid is specified in JWT, JWK with the same kid is used for
    // verification.
    // If kid is not specified in JWT, try all JWK.
    if (!jwt.kid_.empty() && !jwk->kid_.empty() && jwk->kid_ != jwt.kid_) {
      continue;
    }

    // The same alg must be used.
    if (!jwk->alg_.empty() && jwk->alg_ != jwt.alg_) {
      continue;
    }
    kid_alg_matched = true;

    if (jwk->kty_ == "EC") {
      const EVP_MD* md;
      if (jwt.alg_ == "ES384") {
        md = EVP_sha384();
      } else if (jwt.alg_ == "ES512") {
        md = EVP_sha512();
      } else {
        // default to SHA256
        md = EVP_sha256();
      }

      if (verifySignatureEC(jwk->ec_key_.get(), md, jwt.signature_,
                            signed_data)) {
        // Verification succeeded.
        return Status::Ok;
      }
    } else if (jwk->kty_ == "RSA") {
      const EVP_MD* md;
      if (jwt.alg_ == "RS384") {
        md = EVP_sha384();
      } else if (jwt.alg_ == "RS512") {
        md = EVP_sha512();
      } else {
        // default to SHA256
        md = EVP_sha256();
      }

      if (verifySignatureRSA(jwk->rsa_.get(), md, jwt.signature_,
                             signed_data)) {
        // Verification succeeded.
        return Status::Ok;
      }
    } else if (jwk->kty_ == "oct") {
      const EVP_MD* md;
      if (jwt.alg_ == "HS384") {
        md = EVP_sha384();
      } else if (jwt.alg_ == "HS512") {
        md = EVP_sha512();
      } else {
        // default to SHA256
        md = EVP_sha256();
      }

      if (verifySignatureOct(jwk->hmac_key_, md, jwt.signature_, signed_data)) {
        // Verification succeeded.
        return Status::Ok;
      }
    }
  }

  // Verification failed.
  return kid_alg_matched ? Status::JwtVerificationFail
                         : Status::JwksKidAlgMismatch;
}

Status verifyJwt(const Jwt& jwt, const Jwks& jwks,
                 const std::vector<std::string>& audiences) {
  return verifyJwt(jwt, jwks, audiences, absl::ToUnixSeconds(absl::Now()));
}

Status verifyJwt(const Jwt& jwt, const Jwks& jwks,
                 const std::vector<std::string>& audiences, uint64_t now) {
  CheckAudience checker(audiences);
  if (!checker.areAudiencesAllowed(jwt.audiences_)) {
    return Status::JwtAudienceNotAllowed;
  }
  return verifyJwt(jwt, jwks, now);
}

}  // namespace jwt_verify
}  // namespace google
