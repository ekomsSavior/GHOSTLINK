#pragma once
#include "ghost_protocol.h"
#ifdef GHOST_HOST_TEST
#include <openssl/evp.h>
#include <openssl/hmac.h>
#else
#include <mbedtls/md.h>
#include <mbedtls/gcm.h>
#endif
namespace ghost {
inline bool hmac256(const uint8_t* key, size_t keyLength, const uint8_t* data, size_t length, uint8_t out[32]) {
#ifdef GHOST_HOST_TEST
  unsigned resultLength = 0;
  return HMAC(EVP_sha256(), key, keyLength, data, length, out, &resultLength) && resultLength == 32;
#else
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLength, data, length, out) == 0;
#endif
}
inline bool equalTag(const uint8_t* a, const uint8_t* b, size_t length) {
  uint8_t diff = 0; for (size_t i = 0; i < length; ++i) diff |= a[i] ^ b[i]; return diff == 0;
}
inline bool deriveKey(const uint8_t* master, size_t length, const char* domain, uint8_t key[32]) {
  return hmac256(master, length, reinterpret_cast<const uint8_t*>(domain), std::strlen(domain), key);
}
inline bool gcm(bool encrypt, const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aadLength,
                uint8_t* data, size_t length, uint8_t tag[16]) {
#ifdef GHOST_HOST_TEST
  auto* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  int n = 0, end = 0; uint8_t output[MAX_TEXT + 16]{};
  bool ok = EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
      EVP_CipherInit_ex(ctx, nullptr, nullptr, key, nonce, encrypt) == 1 &&
      EVP_CipherUpdate(ctx, nullptr, &n, aad, aadLength) == 1 &&
      EVP_CipherUpdate(ctx, output, &n, data, length) == 1;
  if (!encrypt) ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1;
  ok = ok && EVP_CipherFinal_ex(ctx, output + n, &end) == 1;
  if (encrypt) ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
  if (ok) std::memcpy(data, output, length);
  EVP_CIPHER_CTX_free(ctx); return ok;
#else
  mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
  uint8_t output[MAX_TEXT]{};
  int result = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (!result) {
    if (encrypt) result = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, length, nonce, 12,
        aad, aadLength, data, output, 16, tag);
    else result = mbedtls_gcm_auth_decrypt(&ctx, length, nonce, 12, aad, aadLength, tag, 16, data, output);
  }
  mbedtls_gcm_free(&ctx);
  if (!result) std::memcpy(data, output, length);
  return result == 0;
#endif
}
class MeshCrypto {
 public:
  bool begin(uint16_t network, const uint8_t hop[32], const uint8_t chat[16]) {
    network_ = network;
    return deriveKey(hop, 32, "GHOSTLINK mesh transport v4", transport_) &&
        deriveKey(chat, 16, "GHOSTLINK chat master v4", chat_);
  }
  bool transportTag(const uint8_t* data, size_t length, uint8_t tag[16]) const {
    uint8_t digest[32]{};
    if (!hmac256(transport_, 32, data, length, digest)) return false;
    std::memcpy(tag, digest, 16); return true;
  }
  bool seal(Envelope& e) const { return crypt(e, true); }
  bool open(Envelope& e) const { return crypt(e, false); }
 private:
  bool crypt(Envelope& e, bool encrypt) const {
    if (!validEnvelope(e)) return false;
    uint8_t aad[sizeof(EnvelopeHeader) + 3]{};
    aad[0] = PROTOCOL_VERSION; aad[1] = network_; aad[2] = network_ >> 8;
    std::memcpy(aad + 3, &e.header, sizeof(e.header));
    if (e.header.type == uint8_t(MessageType::ACK)) {
      uint8_t key[32]{}, digest[32]{};
      if (!deriveKey(chat_, 32, "GHOSTLINK receipt v4", key) || !hmac256(key, 32, aad, sizeof(aad), digest)) return false;
      if (!encrypt) return equalTag(e.tag, digest, 16);
      std::memcpy(e.tag, digest, 16); return true;
    }
    // Separate direction keys; random 64-bit message session plus monotonic
    // 32-bit sequence form the nonce. Retries forward identical ciphertext.
    uint8_t direction[12]{}, key[32]{}, nonce[12]{};
    std::memcpy(direction, e.header.source.bytes, 6);
    std::memcpy(direction + 6, e.header.destination.bytes, 6);
    std::memcpy(nonce, &e.header.session, 8);
    std::memcpy(nonce + 8, &e.header.sequence, 4);
    if (!hmac256(chat_, 32, direction, sizeof(direction), key)) return false;
    return gcm(encrypt, key, nonce, aad, sizeof(aad), e.data, e.header.length, e.tag);
  }
  uint16_t network_ = 0;
  uint8_t transport_[32]{}, chat_[32]{};
};
inline size_t encodeFrame(FrameHeader header, const uint8_t* body, size_t length, const MeshCrypto& crypto, uint8_t out[MAX_FRAME]) {
  if (!length || length > MAX_BODY || !header.sender.valid() || !header.boot || !header.frame || header.hops >= MAX_HOPS) return 0;
  std::memcpy(header.magic, "GHST", 4); header.version = PROTOCOL_VERSION; header.length = length;
  std::memcpy(out, &header, sizeof(header)); std::memcpy(out + sizeof(header), body, length);
  return crypto.transportTag(out, sizeof(header) + length, out + sizeof(header) + length) ? sizeof(header) + length + 16 : 0;
}
inline bool decodeFrame(const uint8_t* data, size_t size, uint16_t network, const Mac& transmitter,
                        const MeshCrypto& crypto, FrameHeader& h) {
  if (size < sizeof(h) + 16 || size > MAX_FRAME) return false;
  std::memcpy(&h, data, sizeof(h));
  if (std::memcmp(h.magic, "GHST", 4) || h.version != PROTOCOL_VERSION || h.network != network ||
      !h.sender.valid() || h.sender != transmitter || !h.boot || !h.frame || h.hops >= MAX_HOPS ||
      size != sizeof(h) + h.length + 16 ||
      (h.type != uint8_t(FrameType::HELLO) && h.type != uint8_t(FrameType::ENVELOPE))) return false;
  uint8_t expected[16]{};
  return crypto.transportTag(data, size - 16, expected) && equalTag(expected, data + size - 16, 16);
}
} // namespace ghost
