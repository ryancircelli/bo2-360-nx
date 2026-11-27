/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2022 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <array>
#include <bit>
#include <vector>

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
#include <windows.h>

#include <bcrypt.h>
#endif

#include "crypto/TinySHA1.hpp"
#include "crypto/des/des.cpp"
#include "crypto/des/des.h"
#include "crypto/des/des3.h"
#include "crypto/des/descbc.h"
#include "crypto/sha256.cpp"
#include "crypto/sha256.h"
#include "md5.h"
#include "sha512.h"
#include "rsa_public.h"

extern "C" {
#include "aes_128/aes.h"
}

namespace rex::kernel::xboxkrnl {

typedef struct {
  uint8_t S[256];  // 0x0
  uint8_t i;       // 0x100
  uint8_t j;       // 0x101
} XECRYPT_RC4_STATE;
static_assert_size(XECRYPT_RC4_STATE, 0x102);

void XeCryptRc4Key_entry(ppc_ptr_t<XECRYPT_RC4_STATE> rc4_ctx, mapped_void key, u32 key_size) {
  // Setup RC4 state
  rc4_ctx->i = rc4_ctx->j = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    rc4_ctx->S[x] = (uint8_t)x;
  }

  uint32_t idx = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    idx = (idx + rc4_ctx->S[x] + key[x % 0x10]) % 0x100;
    uint8_t temp = rc4_ctx->S[idx];
    rc4_ctx->S[idx] = rc4_ctx->S[x];
    rc4_ctx->S[x] = temp;
  }
}

void XeCryptRc4Ecb_entry(ppc_ptr_t<XECRYPT_RC4_STATE> rc4_ctx, mapped_void data, u32 size) {
  // Crypt data
  for (uint32_t idx = 0; idx < size; idx++) {
    rc4_ctx->i = (rc4_ctx->i + 1) % 0x100;
    rc4_ctx->j = (rc4_ctx->j + rc4_ctx->S[rc4_ctx->i]) % 0x100;
    uint8_t temp = rc4_ctx->S[rc4_ctx->i];
    rc4_ctx->S[rc4_ctx->i] = rc4_ctx->S[rc4_ctx->j];
    rc4_ctx->S[rc4_ctx->j] = temp;

    uint8_t a = data[idx];
    uint8_t b = rc4_ctx->S[(rc4_ctx->S[rc4_ctx->i] + rc4_ctx->S[rc4_ctx->j]) % 0x100];
    data[idx] = (uint8_t)(a ^ b);
  }
}

void XeCryptRc4_entry(mapped_void key, u32 key_size, mapped_void data, u32 size) {
  XECRYPT_RC4_STATE rc4_ctx;
  XeCryptRc4Key_entry(ppc_ptr_t<XECRYPT_RC4_STATE>::from_host(&rc4_ctx), key, key_size);
  XeCryptRc4Ecb_entry(ppc_ptr_t<XECRYPT_RC4_STATE>::from_host(&rc4_ctx), data, size);
}

typedef struct {
  rex::be<uint32_t> count;     // 0x0
  rex::be<uint32_t> state[5];  // 0x4
  uint8_t buffer[64];          // 0x18
} XECRYPT_SHA_STATE;
static_assert_size(XECRYPT_SHA_STATE, 0x58);

void InitSha1(sha1::SHA1* sha, const XECRYPT_SHA_STATE* state) {
  uint32_t digest[5];
  std::copy(std::begin(state->state), std::end(state->state), digest);

  sha->init(digest, state->buffer, state->count);
}

void StoreSha1(const sha1::SHA1* sha, XECRYPT_SHA_STATE* state) {
  std::copy_n(sha->getDigest(), rex::countof(state->state), state->state);

  state->count = static_cast<uint32_t>(sha->getByteCount());
  std::copy_n(sha->getBlock(), sha->getBlockByteIndex(), state->buffer);
}

void XeCryptShaInit_entry(ppc_ptr_t<XECRYPT_SHA_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x67452301;
  sha_state->state[1] = 0xEFCDAB89;
  sha_state->state[2] = 0x98BADCFE;
  sha_state->state[3] = 0x10325476;
  sha_state->state[4] = 0xC3D2E1F0;
}

void XeCryptShaUpdate_entry(ppc_ptr_t<XECRYPT_SHA_STATE> sha_state, mapped_void input,
                            u32 input_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  sha.processBytes(input, input_size);

  StoreSha1(&sha, sha_state);
}

void XeCryptShaFinal_entry(ppc_ptr_t<XECRYPT_SHA_STATE> sha_state, ppc_ptr_t<uint8_t> out,
                           u32 out_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  uint8_t digest[0x14];
  sha.finalize(digest);

  std::copy_n(digest, std::min<size_t>(rex::countof(digest), out_size), static_cast<uint8_t*>(out));
  std::copy_n(sha.getDigest(), rex::countof(sha_state->state), sha_state->state);
}

void XeCryptSha_entry(mapped_void input_1, u32 input_1_size, mapped_void input_2, u32 input_2_size,
                      mapped_void input_3, u32 input_3_size, mapped_void output, u32 output_size) {
  sha1::SHA1 sha;

  if (input_1 && input_1_size) {
    sha.processBytes(input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    sha.processBytes(input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    sha.processBytes(input_3, input_3_size);
  }

  uint8_t digest[0x14];
  sha.finalize(digest);
  std::copy_n(digest, std::min<size_t>(rex::countof(digest), output_size), output.as<uint8_t*>());
}

// TODO: Size of this struct hasn't been confirmed yet.
typedef struct {
  rex::be<uint32_t> count;     // 0x0
  rex::be<uint32_t> state[8];  // 0x4
  uint8_t buffer[64];          // 0x24
} XECRYPT_SHA256_STATE;

void XeCryptSha256Init_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x6a09e667;
  sha_state->state[1] = 0xbb67ae85;
  sha_state->state[2] = 0x3c6ef372;
  sha_state->state[3] = 0xa54ff53a;
  sha_state->state[4] = 0x510e527f;
  sha_state->state[5] = 0x9b05688c;
  sha_state->state[6] = 0x1f83d9ab;
  sha_state->state[7] = 0x5be0cd19;
}

// FIPS 180-4 section 5.3.2; continue with Sha256Update and a 28-byte Sha256Final.
void XeCryptSha224Init_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state) {
  sha_state.Zero();
  constexpr uint32_t iv[] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
                              0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};
  std::copy(std::begin(iv), std::end(iv), sha_state->state);
}

void XeCryptSha256Update_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state, mapped_void input,
                               u32 input_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state), sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer), sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  sha.add(input, input_size);

  std::copy_n(sha.getHashValues(), rex::countof(sha_state->state), sha_state->state);
  std::copy_n(sha.getBuffer(), rex::countof(sha_state->buffer), sha_state->buffer);
  sha_state->count = static_cast<uint32_t>(sha.getTotalSize());
}

void XeCryptSha256Final_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state, ppc_ptr_t<uint8_t> out,
                              u32 out_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state), sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer), sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  uint8_t hash[32];
  sha.getHash(hash);

  std::copy_n(hash, std::min<size_t>(rex::countof(hash), out_size), static_cast<uint8_t*>(out));
  std::copy(std::begin(hash), std::end(hash), sha_state->buffer);
}


// SHA-384 and SHA-512 share the guest ABI: byte count, reserved word,
// eight big-endian state words and a 128-byte pending block.
struct XECRYPT_SHA512_STATE {
  be_u32 count;
  be_u32 reserved;
  be_u64 state[8];
  uint8_t buffer[128];
};
static_assert_size(XECRYPT_SHA512_STATE, 0xC8);
static Sha512 LoadSha512(const uint8_t* blob, bool sha384 = false) {
  XECRYPT_SHA512_STATE guest;
  std::memcpy(&guest, blob, sizeof(guest));
  Sha512 sha;
  sha.count = guest.count;
  sha.is384 = sha384;
  std::copy(std::begin(guest.state), std::end(guest.state), sha.state);
  std::copy(std::begin(guest.buffer), std::end(guest.buffer), sha.buffer);
  return sha;
}
static void StoreSha512(const Sha512& sha, uint8_t* blob) {
  XECRYPT_SHA512_STATE guest{};
  guest.count = static_cast<uint32_t>(sha.count);
  std::copy(std::begin(sha.state), std::end(sha.state), guest.state);
  std::copy(std::begin(sha.buffer), std::end(sha.buffer), guest.buffer);
  std::memcpy(blob, &guest, sizeof(guest));
}
static void ShaWideInit(ppc_ptr_t<uint8_t> blob, bool sha384) {
  Sha512 sha;
  sha.Init(sha384);
  StoreSha512(sha, static_cast<uint8_t*>(blob));
}
static void ShaWideUpdate(ppc_ptr_t<uint8_t> blob, mapped_void input, u32 size) {
  Sha512 sha = LoadSha512(static_cast<uint8_t*>(blob));
  if (input && size) sha.Update(input.as<const uint8_t*>(), size);
  StoreSha512(sha, static_cast<uint8_t*>(blob));
}
static void ShaWideFinal(ppc_ptr_t<uint8_t> blob, ppc_ptr_t<uint8_t> out, u32 out_size, bool sha384) {
  Sha512 sha = LoadSha512(static_cast<uint8_t*>(blob), sha384);
  sha.Final(static_cast<uint8_t*>(out), out_size);
  StoreSha512(sha, static_cast<uint8_t*>(blob));
}
void XeCryptSha384Init_entry(ppc_ptr_t<uint8_t> s) { ShaWideInit(s, true); }
void XeCryptSha384Update_entry(ppc_ptr_t<uint8_t> s, mapped_void in, u32 n) { ShaWideUpdate(s, in, n); }
void XeCryptSha384Final_entry(ppc_ptr_t<uint8_t> s, ppc_ptr_t<uint8_t> out, u32 n) { ShaWideFinal(s, out, n, true); }
void XeCryptSha512Init_entry(ppc_ptr_t<uint8_t> s) { ShaWideInit(s, false); }
void XeCryptSha512Update_entry(ppc_ptr_t<uint8_t> s, mapped_void in, u32 n) { ShaWideUpdate(s, in, n); }
void XeCryptSha512Final_entry(ppc_ptr_t<uint8_t> s, ppc_ptr_t<uint8_t> out, u32 n) { ShaWideFinal(s, out, n, false); }

// Byteswaps each 8 bytes
void XeCryptBnQw_SwapDwQwLeBe_entry(ppc_ptr_t<uint64_t> qw_inp, ppc_ptr_t<uint64_t> qw_out,
                                    u32 size) {
  memory::copy_and_swap<uint64_t>(qw_out, qw_inp, size);
}

typedef struct {
  rex::be<uint32_t> size;  // size of modulus in 8 byte units
  rex::be<uint32_t> public_exponent;
  rex::be<uint64_t> pad_8;

  // followed by modulus, followed by any private-key data
} XECRYPT_RSA;
static_assert_size(XECRYPT_RSA, 0x10);

u32 XeCryptBnQwNeRsaPubCrypt_entry(ppc_ptr_t<uint64_t> qw_a, ppc_ptr_t<uint64_t> qw_b,
                                   ppc_ptr_t<XECRYPT_RSA> rsa) {
  // 0 indicates failure (but not a BOOL return value)
#if !REX_PLATFORM_WIN32
  const uint32_t words = rsa->size;
  if (!words || words > 128) return 0;
  const size_t bytes = size_t(words) * 8;
  return RsaPublicQwordNe(
      {reinterpret_cast<const uint8_t*>(static_cast<uint64_t*>(qw_a)), bytes},
      {reinterpret_cast<uint8_t*>(static_cast<uint64_t*>(qw_b)), bytes},
      {reinterpret_cast<const uint8_t*>(&rsa[1]), bytes}, rsa->public_exponent) ? 1 : 0;
#else
  uint32_t modulus_size = rsa->size * 8;

  // Convert XECRYPT blob into BCrypt format
  ULONG key_size = sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(uint32_t) + modulus_size;
  auto key_buf = std::make_unique<uint8_t[]>(key_size);
  auto* key_header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(key_buf.get());

  key_header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
  key_header->BitLength = modulus_size * 8;
  key_header->cbPublicExp = sizeof(uint32_t);
  key_header->cbModulus = modulus_size;
  key_header->cbPrime1 = key_header->cbPrime2 = 0;

  // Copy in exponent/modulus, luckily these are BE inside BCrypt blob
  uint32_t* key_exponent = reinterpret_cast<uint32_t*>(&key_header[1]);
  *key_exponent = rsa->public_exponent.value;

  // ...except modulus needs to be reversed in 64-bit chunks for BCrypt to make
  // use of it properly for some reason
  uint64_t* key_modulus = reinterpret_cast<uint64_t*>(&key_exponent[1]);
  uint64_t* xecrypt_modulus = reinterpret_cast<uint64_t*>(&rsa[1]);
  std::reverse_copy(xecrypt_modulus, xecrypt_modulus + rsa->size, key_modulus);

  BCRYPT_ALG_HANDLE hAlgorithm = NULL;
  NTSTATUS status =
      BCryptOpenAlgorithmProvider(&hAlgorithm, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);

  if (!BCRYPT_SUCCESS(status)) {
    REXKRNL_ERROR(
        "XeCryptBnQwNeRsaPubCrypt: BCryptOpenAlgorithmProvider failed with "
        "status {:#X}!",
        status);
    return 0;
  }

  BCRYPT_KEY_HANDLE hKey = NULL;
  status = BCryptImportKeyPair(hAlgorithm, NULL, BCRYPT_RSAPUBLIC_BLOB, &hKey, key_buf.get(),
                               key_size, 0);

  if (!BCRYPT_SUCCESS(status)) {
    REXKRNL_ERROR(
        "XeCryptBnQwNeRsaPubCrypt: BCryptImportKeyPair failed with status "
        "{:#X}!",
        status);

    if (hAlgorithm) {
      BCryptCloseAlgorithmProvider(hAlgorithm, 0);
    }

    return 0;
  }

  // Byteswap & reverse the input into output, as BCrypt wants MSB first
  uint64_t* output = qw_b;
  uint8_t* output_bytes = reinterpret_cast<uint8_t*>(output);
  memory::copy_and_swap<uint64_t>(output, qw_a, rsa->size);
  std::reverse(output_bytes, output_bytes + modulus_size);

  // BCryptDecrypt only works with private keys, fortunately BCryptEncrypt
  // performs the right actions needed for us to decrypt the input
  ULONG result_size = 0;
  status = BCryptEncrypt(hKey, output_bytes, modulus_size, nullptr, nullptr, 0, output_bytes,
                         modulus_size, &result_size, BCRYPT_PAD_NONE);

  assert(result_size == modulus_size);

  if (!BCRYPT_SUCCESS(status)) {
    REXKRNL_ERROR("XeCryptBnQwNeRsaPubCrypt: BCryptEncrypt failed with status {:#X}!", status);
  } else {
    // Reverse data & byteswap again so data is as game expects
    std::reverse(output_bytes, output_bytes + modulus_size);
    memory::copy_and_swap(output, output, rsa->size);
  }

  if (hKey) {
    BCryptDestroyKey(hKey);
  }
  if (hAlgorithm) {
    BCryptCloseAlgorithmProvider(hAlgorithm, 0);
  }

  return BCRYPT_SUCCESS(status) ? 1 : 0;
#endif
}
#if REX_PLATFORM_WIN32

#else

#endif

// Behaviour specified in the stream research ledger from RFC 8017 and the credited
// upstream ABI. The block is stored as a little-endian integer, including the hash.
void XeCryptBnDwLePkcs1Format_entry(mapped_void hash, u32 format, mapped_void sig, u32 size) {
  if (size < 39 || size > 512) return;
  // Invalid selectors have no documented encoding. Preserve the caller's buffer.
  if (format > 2) return;
  constexpr uint8_t standard[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
                                 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
  constexpr uint8_t short_form[] = {0x30, 0x1f, 0x30, 0x07, 0x06, 0x05, 0x2b,
                                   0x0e, 0x03, 0x02, 0x1a, 0x04, 0x14};
  // Save the hash first so the input may alias the output.
  std::array<uint8_t, 20> digest;
  std::copy_n(hash.as<const uint8_t*>(), digest.size(), digest.begin());
  auto* dst = sig.as<uint8_t*>();
  std::fill_n(dst, size, 0xff);
  std::reverse_copy(digest.begin(), digest.end(), dst);
  size_t separator = digest.size();
  if (format == 0) {
    std::reverse_copy(std::begin(standard), std::end(standard), dst + separator);
    separator += sizeof(standard);
  } else if (format == 1) {
    std::reverse_copy(std::begin(short_form), std::end(short_form), dst + separator);
    separator += sizeof(short_form);
  }
  dst[separator] = 0;
  dst[size - 2] = 1;
  dst[size - 1] = 0;
}

u32 XeCryptBnDwLePkcs1Verify_entry(mapped_void hash, mapped_void sig, u32 size) {
  if (size < 39 || size > 512) return 0;
  std::array<uint8_t, 512> expected;
  for (u32 format = 0; format < 3; ++format) {
    XeCryptBnDwLePkcs1Format_entry(hash, format, mapped_void::from_host(expected.data()), size);
    if (std::memcmp(expected.data(), sig.as<const uint8_t*>(), size) == 0) return 1;
  }
  return 0;
}

void XeCryptRandom_entry(mapped_void buf, u32 buf_size) {
  std::memset(buf, 0xFD, buf_size);
}

struct XECRYPT_DES_STATE {
  uint32_t keytab[16][2];
};

// Sets bit 0 to make the parity odd
void XeCryptDesParity_entry(mapped_void inp, u32 inp_size, mapped_void out_ptr) {
  DES::set_parity(inp, inp_size, out_ptr);
}

struct XECRYPT_DES3_STATE {
  XECRYPT_DES_STATE des_state[3];
};

void XeCryptDes3Key_entry(ppc_ptr_t<XECRYPT_DES3_STATE> state_ptr, mapped_u64 key) {
  DES3 des3(key[0], key[1], key[2]);
  DES* des = des3.getDES();

  // Store our DES state into the state.
  for (int i = 0; i < 3; i++) {
    std::memcpy(state_ptr->des_state[i].keytab, des[i].get_sub_key(), 128);
  }
}

void XeCryptDes3Ecb_entry(ppc_ptr_t<XECRYPT_DES3_STATE> state_ptr, mapped_u64 inp, mapped_u64 out,
                          u32 encrypt) {
  DES3 des3((ui64*)state_ptr->des_state[0].keytab, (ui64*)state_ptr->des_state[1].keytab,
            (ui64*)state_ptr->des_state[2].keytab);

  if (encrypt) {
    *out = des3.encrypt(*inp);
  } else {
    *out = des3.decrypt(*inp);
  }
}

void XeCryptDes3Cbc_entry(ppc_ptr_t<XECRYPT_DES3_STATE> state_ptr, mapped_u64 inp, u32 inp_size,
                          mapped_u64 out, mapped_u64 feed, u32 encrypt) {
  DES3 des3((ui64*)state_ptr->des_state[0].keytab, (ui64*)state_ptr->des_state[1].keytab,
            (ui64*)state_ptr->des_state[2].keytab);

  // DES can only do 8-byte chunks at a time!
  assert_true(inp_size % 8 == 0);

  uint64_t last_block = *feed;
  for (uint32_t i = 0; i < inp_size / 8; i++) {
    uint64_t block = inp[i];
    if (encrypt) {
      last_block = des3.encrypt(block ^ last_block);
      out[i] = last_block;
    } else {
      out[i] = des3.decrypt(block) ^ last_block;
      last_block = block;
    }
  }

  *feed = last_block;
}

struct XECRYPT_AES_STATE {
  uint8_t keytabenc[11][4][4];  // 0x0
  uint8_t keytabdec[11][4][4];  // 0xB0
};
static_assert_size(XECRYPT_AES_STATE, 0x160);

static inline uint8_t xeXeCryptAesMul2(uint8_t a) {
  return (a & 0x80) ? ((a << 1) ^ 0x1B) : (a << 1);
}

void XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE> state_ptr, mapped_void key) {
  aes_key_schedule_128(key, reinterpret_cast<uint8_t*>(state_ptr->keytabenc));
  // Decryption key schedule not needed by openluopworld/aes_128, but generated
  // to fill the context structure properly.
  std::memcpy(state_ptr->keytabdec[0], state_ptr->keytabenc[10], 16);
  // Inverse MixColumns.
  for (uint32_t i = 1; i < 10; ++i) {
    const uint8_t* enc = reinterpret_cast<const uint8_t*>(state_ptr->keytabenc[10 - i]);
    uint8_t* dec = reinterpret_cast<uint8_t*>(state_ptr->keytabdec[i]);
    uint8_t t, u, v;
    t = enc[0] ^ enc[1] ^ enc[2] ^ enc[3];
    dec[0] = t ^ enc[0] ^ xeXeCryptAesMul2(enc[0] ^ enc[1]);
    dec[1] = t ^ enc[1] ^ xeXeCryptAesMul2(enc[1] ^ enc[2]);
    dec[2] = t ^ enc[2] ^ xeXeCryptAesMul2(enc[2] ^ enc[3]);
    dec[3] = t ^ enc[3] ^ xeXeCryptAesMul2(enc[3] ^ enc[0]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[0] ^ enc[2]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[1] ^ enc[3]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[0] ^= t ^ u;
    dec[1] ^= t ^ v;
    dec[2] ^= t ^ u;
    dec[3] ^= t ^ v;
    t = enc[4] ^ enc[5] ^ enc[6] ^ enc[7];
    dec[4] = t ^ enc[4] ^ xeXeCryptAesMul2(enc[4] ^ enc[5]);
    dec[5] = t ^ enc[5] ^ xeXeCryptAesMul2(enc[5] ^ enc[6]);
    dec[6] = t ^ enc[6] ^ xeXeCryptAesMul2(enc[6] ^ enc[7]);
    dec[7] = t ^ enc[7] ^ xeXeCryptAesMul2(enc[7] ^ enc[4]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[4] ^ enc[6]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[5] ^ enc[7]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[4] ^= t ^ u;
    dec[5] ^= t ^ v;
    dec[6] ^= t ^ u;
    dec[7] ^= t ^ v;
    t = enc[8] ^ enc[9] ^ enc[10] ^ enc[11];
    dec[8] = t ^ enc[8] ^ xeXeCryptAesMul2(enc[8] ^ enc[9]);
    dec[9] = t ^ enc[9] ^ xeXeCryptAesMul2(enc[9] ^ enc[10]);
    dec[10] = t ^ enc[10] ^ xeXeCryptAesMul2(enc[10] ^ enc[11]);
    dec[11] = t ^ enc[11] ^ xeXeCryptAesMul2(enc[11] ^ enc[8]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[8] ^ enc[10]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[9] ^ enc[11]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[8] ^= t ^ u;
    dec[9] ^= t ^ v;
    dec[10] ^= t ^ u;
    dec[11] ^= t ^ v;
    t = enc[12] ^ enc[13] ^ enc[14] ^ enc[15];
    dec[12] = t ^ enc[12] ^ xeXeCryptAesMul2(enc[12] ^ enc[13]);
    dec[13] = t ^ enc[13] ^ xeXeCryptAesMul2(enc[13] ^ enc[14]);
    dec[14] = t ^ enc[14] ^ xeXeCryptAesMul2(enc[14] ^ enc[15]);
    dec[15] = t ^ enc[15] ^ xeXeCryptAesMul2(enc[15] ^ enc[12]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[12] ^ enc[14]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[13] ^ enc[15]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[12] ^= t ^ u;
    dec[13] ^= t ^ v;
    dec[14] ^= t ^ u;
    dec[15] ^= t ^ v;
  }
  std::memcpy(state_ptr->keytabdec[10], state_ptr->keytabenc[0], 16);
  // TODO(Triang3l): Verify the order in keytabenc and everything in keytabdec.
}

void XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE> state_ptr, mapped_void inp_ptr,
                         mapped_void out_ptr, u32 encrypt) {
  const uint8_t* keytab = reinterpret_cast<const uint8_t*>(state_ptr->keytabenc);
  if (encrypt) {
    aes_encrypt_128(keytab, inp_ptr, out_ptr);
  } else {
    aes_decrypt_128(keytab, inp_ptr, out_ptr);
  }
}

void XeCryptAesCbc_entry(ppc_ptr_t<XECRYPT_AES_STATE> state_ptr, mapped_void inp_ptr, u32 inp_size,
                         mapped_void out_ptr, mapped_void feed_ptr, u32 encrypt) {
  const uint8_t* keytab = reinterpret_cast<const uint8_t*>(state_ptr->keytabenc);
  const uint8_t* inp = inp_ptr.as<const uint8_t*>();
  uint8_t* out = out_ptr.as<uint8_t*>();
  uint8_t* feed = feed_ptr.as<uint8_t*>();
  if (encrypt) {
    for (uint32_t i = 0; i < inp_size; i += 16) {
      for (uint32_t j = 0; j < 16; ++j) {
        feed[j] ^= inp[j];
      }
      aes_encrypt_128(keytab, feed, feed);
      std::memcpy(out, feed, 16);
      inp += 16;
      out += 16;
    }
  } else {
    for (uint32_t i = 0; i < inp_size; i += 16) {
      // In case inp == out.
      uint8_t tmp[16];
      std::memcpy(tmp, inp, 16);
      aes_decrypt_128(keytab, inp, out);
      for (uint32_t j = 0; j < 16; ++j) {
        out[j] ^= feed[j];
      }
      std::memcpy(feed, tmp, 16);
      inp += 16;
      out += 16;
    }
  }
}

void XeCryptHmacSha_entry(mapped_void key, u32 key_size_in, mapped_void inp_1, u32 inp_1_size,
                          mapped_void inp_2, u32 inp_2_size, mapped_void inp_3, u32 inp_3_size,
                          mapped_void out, u32 out_size) {
  uint32_t key_size = key_size_in;
  sha1::SHA1 sha;
  uint8_t kpad_i[0x40];
  uint8_t kpad_o[0x40];
  uint8_t tmp_key[0x40];
  std::memset(kpad_i, 0x36, 0x40);
  std::memset(kpad_o, 0x5C, 0x40);

  // Setup HMAC key
  // If > block size, use its hash
  if (key_size > 0x40) {
    sha1::SHA1 sha_key;
    sha_key.processBytes(key, key_size);
    sha_key.finalize((uint8_t*)tmp_key);

    key_size = 0x14u;
  } else {
    std::memcpy(tmp_key, key, key_size);
  }

  for (uint32_t i = 0; i < key_size; i++) {
    kpad_i[i] = tmp_key[i] ^ 0x36;
    kpad_o[i] = tmp_key[i] ^ 0x5C;
  }

  // Inner
  sha.processBytes(kpad_i, 0x40);

  if (inp_1_size) {
    sha.processBytes(inp_1, inp_1_size);
  }

  if (inp_2_size) {
    sha.processBytes(inp_2, inp_2_size);
  }

  if (inp_3_size) {
    sha.processBytes(inp_3, inp_3_size);
  }

  uint8_t digest[0x14];
  sha.finalize(digest);
  sha.reset();

  // Outer
  sha.processBytes(kpad_o, 0x40);
  sha.processBytes(digest, 0x14);
  sha.finalize(digest);

  std::memcpy(out, digest, std::min((uint32_t)out_size, 0x14u));
}

// Keys
// TODO: Array of keys we need

// Retail key 0x19
static const uint8_t key19[] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                                0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};

u32 XeKeysHmacSha_entry(u32 key_num, mapped_void inp_1, u32 inp_1_size, mapped_void inp_2,
                        u32 inp_2_size, mapped_void inp_3, u32 inp_3_size, mapped_void out,
                        u32 out_size) {
  const uint8_t* key = nullptr;
  if (key_num == 0x19) {
    key = key19;
  }

  if (key) {
    XeCryptHmacSha_entry(mapped_void::from_host((void*)key), 0x10, inp_1, inp_1_size, inp_2,
                         inp_2_size, inp_3, inp_3_size, out, out_size);

    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

static const uint8_t xe_key_obfuscation_key[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

u32 XeKeysAesCbcUsingKey_entry(mapped_void obscured_key, mapped_void inp_ptr, u32 inp_size,
                               mapped_void out_ptr, mapped_void feed_ptr, u32 encrypt) {
  uint8_t key[16];

  // Deobscure key
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes),
                      mapped_void::from_host((void*)xe_key_obfuscation_key));
  XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), obscured_key,
                      mapped_void::from_host(key), 0);

  // Run CBC using deobscured key
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), mapped_void::from_host(key));
  XeCryptAesCbc_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), inp_ptr, inp_size, out_ptr,
                      feed_ptr, encrypt);

  return X_STATUS_SUCCESS;
}

u32 XeKeysObscureKey_entry(mapped_void input, mapped_void output) {
  // Based on HvxKeysObscureKey
  // Seems to encrypt input with per-console KEY_OBFUSCATION_KEY (key 0x18)

  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes),
                      mapped_void::from_host((void*)xe_key_obfuscation_key));
  XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), input, output, 1);

  return X_STATUS_SUCCESS;
}

u32 XeKeysHmacShaUsingKey_entry(mapped_void obscured_key, mapped_void inp_1, u32 inp_1_size,
                                mapped_void inp_2, u32 inp_2_size, mapped_void inp_3,
                                u32 inp_3_size, mapped_void out, u32 out_size) {
  if (!obscured_key) {
    return X_STATUS_INVALID_PARAMETER;
  }

  uint8_t key[16];

  // Deobscure key
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes),
                      mapped_void::from_host((void*)xe_key_obfuscation_key));
  XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), obscured_key,
                      mapped_void::from_host(key), 0);

  XeCryptHmacSha_entry(mapped_void::from_host(key), 0x10, inp_1, inp_1_size, inp_2, inp_2_size,
                       inp_3, inp_3_size, out, out_size);
  return X_STATUS_SUCCESS;
}

u32 XeKeysConsolePrivateKeySign_entry(mapped_void hash, mapped_void signature) {
  REXKRNL_DEBUG("XeKeysConsolePrivateKeySign - stub");
  return 0;  // Success
}

// Fixed fake console id (same values as xenia-canary); we have no key vault.
u32 XeKeysGetConsoleID_entry(mapped_void raw_bytes, mapped_void hex_string) {
  if (raw_bytes) {
    static const uint8_t kId[5] = {0x93, 0x01, 0x64, 0xE6, 0x07};
    std::memcpy(raw_bytes.as<uint8_t*>(), kId, sizeof(kId));
  }
  if (hex_string) std::memcpy(hex_string.as<char*>(), "245149100000", 12);
  return X_STATUS_SUCCESS;
}

u32 XeKeysConsoleSignatureVerification_entry(mapped_void hash, mapped_void signature,
                                             mapped_void pubkey) {
  REXKRNL_DEBUG("XeKeysConsoleSignatureVerification - stub");
  return 0;  // Success (signature valid)
}

// ---- Bignum helpers. Guest memory is big-endian; mapped_u64/mapped_u32 read through the swap. ----
void XeCryptBnDw_Zero_entry(mapped_void data, u32 dwords) { std::memset(data, 0, size_t{dwords} * 4); }
void XeCryptBnDw_Copy_entry(mapped_void src, mapped_void dst, u32 dwords) {
  std::memmove(dst, src, size_t{dwords} * 4);
}
void XeCryptBnDw_SwapLeBe_entry(mapped_void src, mapped_void dst, u32 dwords) {
  memory::copy_and_swap<uint32_t>(dst.as<uint32_t*>(), src.as<const uint32_t*>(), dwords);
}
void XeCryptBnQw_Zero_entry(mapped_void data, u32 qwords) { std::memset(data, 0, size_t{qwords} * 8); }
void XeCryptBnQw_Copy_entry(mapped_void src, mapped_void dst, u32 qwords) {
  std::memmove(dst, src, size_t{qwords} * 8);
}
void XeCryptBnQw_SwapLeBe_entry(mapped_void src, mapped_void dst, u32 qwords) {
  memory::copy_and_swap<uint64_t>(dst.as<uint64_t*>(), src.as<const uint64_t*>(), qwords);
}
// Swaps the two dwords inside each qword; no byte swap.
void XeCryptBnQw_SwapDwQw_entry(mapped_void src, mapped_void dst, u32 qwords) {
  auto* s = src.as<const uint32_t*>();
  auto* d = dst.as<uint32_t*>();
  for (u32 i = 0; i < qwords; ++i) {
    const uint32_t a = s[2 * i], b = s[2 * i + 1];
    d[2 * i] = b;
    d[2 * i + 1] = a;
  }
}
// Little-endian qword digits: compare from the most significant one down. 1 / -1 / 0.
i32 XeCryptBnQwNeCompare_entry(mapped_u64 a, mapped_u64 b, u32 qwords) {
  for (u32 i = qwords; i-- > 0;) {
    const uint64_t x = a[i], y = b[i];
    if (x != y) return x > y ? 1 : -1;
  }
  return 0;
}

// Ported from Xenia Canary (BSD-3-Clause), retaining its Montgomery conventions.
u64 XeCryptBnQwNeModInv_entry(u64 input) {
  uint64_t intermediate = input;
  // Compute the 2-adic of qw such that: result = -1 + input^2
  uint64_t result = (intermediate * 3) ^ 2;
  intermediate = 1 - (result * intermediate);

  // Raise it to another 32 such that: result = -1 + input^64
  for (uint32_t i = 5; i < 32; i <<= 1) {
    result = result * (intermediate + 1);
    intermediate = intermediate * intermediate;
  }

  // Done
  result = result * (intermediate + 1);
  return result;
}

void XeCryptBnQwNeModMul_entry(mapped_u64 qw_a, mapped_u64 qw_b, mapped_u64 qw_out,
                               u64 inverse, mapped_u64 modulus,
                               u32 size) {
  using u128 = unsigned __int128;
  const auto u128_mul64 = [](uint64_t a, uint64_t b) { return u128{a} * b; };
  const auto u128_add = [](u128 a, uint64_t b) { return a + b; };
  const auto u128_from = [](uint64_t a) { return u128{a}; };

  const uint32_t n = size;
  if (!n) {
    return;
  }

  std::vector<uint64_t> ab(n), mn(n);
  for (uint32_t i = 0; i < n; i++) {
    uint64_t b = qw_b[i];
    // Makes the low qwords of both sums equal, so they drop out of the shift
    uint64_t m = (ab[0] - mn[0] + b * uint64_t(qw_a[0])) * uint64_t(inverse);

    uint64_t ab_carry = 0;
    uint64_t mn_carry = 0;
    for (uint32_t j = 0; j < n; j++) {
      u128 t = u128_add(u128_add(u128_mul64(b, qw_a[j]), ab[j]), ab_carry);
      u128 u = u128_add(u128_add(u128_mul64(m, modulus[j]), mn[j]), mn_carry);
      if (j) {
        ab[j - 1] = static_cast<uint64_t>(t);
        mn[j - 1] = static_cast<uint64_t>(u);
      }
      ab_carry = static_cast<uint64_t>(t >> 64);
      mn_carry = static_cast<uint64_t>(u >> 64);
    }
    ab[n - 1] = ab_carry;
    mn[n - 1] = mn_carry;
  }

  bool greater = false;
  for (uint32_t i = n; i-- > 0;) {
    if (ab[i] != mn[i]) {
      greater = ab[i] > mn[i];
      break;
    }
  }

  // Inputs are only read above, so the output can overlap them
  uint64_t carry = 0;
  uint64_t borrow = 0;
  for (uint32_t i = 0; i < n; i++) {
    u128 sum = u128_add(
        u128_add(u128_from(ab[i]), greater ? 0 : uint64_t(modulus[i])), carry);
    carry = static_cast<uint64_t>(sum >> 64);
    uint64_t diff = static_cast<uint64_t>(sum) - mn[i];
    uint64_t next_borrow = static_cast<uint64_t>(sum) < mn[i] || diff < borrow;
    qw_out[i] = diff - borrow;
    borrow = next_borrow;
  }
}

// Original implementation from the written RotSumSha behaviour specification.
// The upstream RotSum helper cites ExCrypt; no code from that helper is retained.
void XeCryptRotSumSha_entry(mapped_void in1, u32 n1, mapped_void in2, u32 n2,
                            mapped_void out, u32 out_size) {
  uint64_t positive_carries = 0, positive = 0, negative_carries = 0, negative = 0;
  for (const auto& part : {std::pair{in1, n1}, std::pair{in2, n2}}) {
    const auto* bytes = part.first.as<const uint8_t*>();
    for (size_t offset = 0; offset + 8 <= part.second; offset += 8) {
      uint64_t word = 0;
      for (size_t byte = 0; byte != 8; ++byte) word = (word << 8) | bytes[offset + byte];
      const uint64_t added = positive + word;
      const uint64_t subtracted = negative - word;
      positive_carries += added < word;
      negative_carries -= subtracted > word;
      positive = std::rotl(added, 29);
      negative = std::rotl(subtracted, 31);
    }
  }
  std::array<uint8_t, 32> checksum;
  const uint64_t words[] = {positive_carries, positive, negative_carries, negative};
  for (size_t i = 0; i < checksum.size(); ++i) {
    checksum[i] = static_cast<uint8_t>(words[i / 8] >> (56 - 8 * (i % 8)));
  }
  sha1::SHA1 sha;
  for (int copy = 0; copy < 2; ++copy) sha.processBytes(checksum.data(), checksum.size());
  if (n1) sha.processBytes(in1.as<const uint8_t*>(), n1);
  if (n2) sha.processBytes(in2.as<const uint8_t*>(), n2);
  for (auto& byte : checksum) byte ^= 0xff;
  for (int copy = 0; copy < 2; ++copy) sha.processBytes(checksum.data(), checksum.size());
  uint8_t digest[20];
  sha.finalize(digest);
  std::copy_n(digest, std::min<size_t>(sizeof(digest), out_size), out.as<uint8_t*>());
}

// Guest layouts from the credited ReXGlue v0.11 ABI. Host hash objects must
// never be written directly into these smaller, big-endian state blocks.
struct XECRYPT_MD5_STATE {
  be_u32 count;
  be_u32 state[4];
  uint8_t buffer[64];
};
static_assert_size(XECRYPT_MD5_STATE, 0x54);
struct XECRYPT_HMACMD5_STATE {
  XECRYPT_MD5_STATE md5[2];
};
static_assert_size(XECRYPT_HMACMD5_STATE, 0xA8);

static Md5 LoadMd5(const XECRYPT_MD5_STATE& guest) {
  Md5 host;
  host.count = guest.count;
  std::copy(std::begin(guest.state), std::end(guest.state), host.state);
  std::copy(std::begin(guest.buffer), std::end(guest.buffer), host.buffer);
  return host;
}
static void StoreMd5(const Md5& host, XECRYPT_MD5_STATE& guest) {
  guest.count = static_cast<uint32_t>(host.count);
  std::copy(std::begin(host.state), std::end(host.state), guest.state);
  std::copy(std::begin(host.buffer), std::end(host.buffer), guest.buffer);
}
void XeCryptMd5Init_entry(ppc_ptr_t<XECRYPT_MD5_STATE> st) {
  Md5 m;
  m.Init();
  StoreMd5(m, *st);
}
void XeCryptMd5Update_entry(ppc_ptr_t<XECRYPT_MD5_STATE> st, mapped_void in, u32 n) {
  Md5 m = LoadMd5(*st);
  if (in && n) m.Update(in.as<const uint8_t*>(), n);
  StoreMd5(m, *st);
}
void XeCryptMd5Final_entry(ppc_ptr_t<XECRYPT_MD5_STATE> st, mapped_void out, u32 n) {
  Md5 m = LoadMd5(*st);
  m.Final(out.as<uint8_t*>(), n);
  StoreMd5(m, *st);
}
void XeCryptMd5_entry(mapped_void in1, u32 n1, mapped_void in2, u32 n2, mapped_void in3, u32 n3,
                      mapped_void out, u32 out_size) {
  Md5 m;
  m.Init();
  if (in1 && n1) m.Update(in1.as<const uint8_t*>(), n1);
  if (in2 && n2) m.Update(in2.as<const uint8_t*>(), n2);
  if (in3 && n3) m.Update(in3.as<const uint8_t*>(), n3);
  m.Final(out.as<uint8_t*>(), out_size);
}
void XeCryptHmacMd5Init_entry(ppc_ptr_t<XECRYPT_HMACMD5_STATE> st, mapped_void key, u32 key_size) {
  HmacMd5 h;
  h.Init(key.as<const uint8_t*>(), key_size);
  StoreMd5(h.inner, st->md5[0]);
  StoreMd5(h.outer, st->md5[1]);
}
void XeCryptHmacMd5Update_entry(ppc_ptr_t<XECRYPT_HMACMD5_STATE> st, mapped_void in, u32 n) {
  Md5 m = LoadMd5(st->md5[0]);
  if (in && n) m.Update(in.as<const uint8_t*>(), n);
  StoreMd5(m, st->md5[0]);
}
void XeCryptHmacMd5Final_entry(ppc_ptr_t<XECRYPT_HMACMD5_STATE> st, mapped_void out, u32 n) {
  HmacMd5 h;
  h.inner = LoadMd5(st->md5[0]);
  h.outer = LoadMd5(st->md5[1]);
  h.Final(out.as<uint8_t*>(), n);
  StoreMd5(h.inner, st->md5[0]);
  StoreMd5(h.outer, st->md5[1]);
}
void XeCryptHmacMd5_entry(mapped_void key, u32 key_size, mapped_void in1, u32 n1, mapped_void in2,
                          u32 n2, mapped_void in3, u32 n3, mapped_void out, u32 out_size) {
  HmacMd5 h;
  h.Init(key.as<const uint8_t*>(), key_size);
  if (in1 && n1) h.Update(in1.as<const uint8_t*>(), n1);
  if (in2 && n2) h.Update(in2.as<const uint8_t*>(), n2);
  if (in3 && n3) h.Update(in3.as<const uint8_t*>(), n3);
  h.Final(out.as<uint8_t*>(), out_size);
}

// ---- HMAC-SHA1 incremental: guest block = {inner SHA state, outer SHA state}. ----
struct XECRYPT_HMACSHA_STATE {
  XECRYPT_SHA_STATE sha[2];
};
static_assert_size(XECRYPT_HMACSHA_STATE, 0xB0);

void XeCryptHmacShaInit_entry(ppc_ptr_t<XECRYPT_HMACSHA_STATE> st, mapped_void key, u32 key_size) {
  uint8_t k[0x40] = {};
  if (key_size > 0x40) {
    sha1::SHA1 s;
    s.processBytes(key, key_size);
    s.finalize(k);
  } else if (key_size) {
    std::memcpy(k, key, key_size);
  }
  uint8_t ipad[0x40], opad[0x40];
  for (int i = 0; i < 0x40; ++i) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5C;
  }
  for (int i = 0; i < 2; ++i) {
    auto s = ppc_ptr_t<XECRYPT_SHA_STATE>::from_host(&st->sha[i]);
    XeCryptShaInit_entry(s);
    XeCryptShaUpdate_entry(s, mapped_void::from_host(i ? opad : ipad), 0x40);
  }
}
void XeCryptHmacShaUpdate_entry(ppc_ptr_t<XECRYPT_HMACSHA_STATE> st, mapped_void in, u32 n) {
  if (in && n) XeCryptShaUpdate_entry(ppc_ptr_t<XECRYPT_SHA_STATE>::from_host(&st->sha[0]), in, n);
}
void XeCryptHmacShaFinal_entry(ppc_ptr_t<XECRYPT_HMACSHA_STATE> st, mapped_void out, u32 out_size) {
  uint8_t digest[0x14];
  XeCryptShaFinal_entry(ppc_ptr_t<XECRYPT_SHA_STATE>::from_host(&st->sha[0]),
                        ppc_ptr_t<uint8_t>::from_host(digest), sizeof(digest));
  auto outer = ppc_ptr_t<XECRYPT_SHA_STATE>::from_host(&st->sha[1]);
  XeCryptShaUpdate_entry(outer, mapped_void::from_host(digest), sizeof(digest));
  XeCryptShaFinal_entry(outer, ppc_ptr_t<uint8_t>::from_host(out.as<uint8_t*>()), out_size);
}
// 1 when the first compare_size (<= 20) bytes of the HMAC match, else 0.
u32 XeCryptHmacShaVerify_entry(mapped_void key, u32 key_size, mapped_void in1, u32 n1,
                               mapped_void in2, u32 n2, mapped_void in3, u32 n3,
                               mapped_void compare, u32 compare_size) {
  uint8_t mac[0x14];
  XeCryptHmacSha_entry(key, key_size, in1, n1, in2, n2, in3, n3, mapped_void::from_host(mac),
                       sizeof(mac));
  return compare_size <= sizeof(mac) && !std::memcmp(mac, compare, compare_size);
}

// ---- One-shot SHA-2 over up to three inputs, built on the incremental entries above. ----
void XeCryptSha256_entry(mapped_void in1, u32 n1, mapped_void in2, u32 n2, mapped_void in3, u32 n3,
                         mapped_void out, u32 out_size) {
  XECRYPT_SHA256_STATE st;
  auto p = ppc_ptr_t<XECRYPT_SHA256_STATE>::from_host(&st);
  XeCryptSha256Init_entry(p);
  if (in1 && n1) XeCryptSha256Update_entry(p, in1, n1);
  if (in2 && n2) XeCryptSha256Update_entry(p, in2, n2);
  if (in3 && n3) XeCryptSha256Update_entry(p, in3, n3);
  XeCryptSha256Final_entry(p, ppc_ptr_t<uint8_t>::from_host(out.as<uint8_t*>()), out_size);
}
static void ShaWideOneShot(bool sha384, mapped_void in1, u32 n1, mapped_void in2, u32 n2,
                           mapped_void in3, u32 n3, mapped_void out, u32 out_size) {
  uint8_t blob[0xC8];
  auto p = ppc_ptr_t<uint8_t>::from_host(blob);
  ShaWideInit(p, sha384);
  ShaWideUpdate(p, in1, n1);
  ShaWideUpdate(p, in2, n2);
  ShaWideUpdate(p, in3, n3);
  ShaWideFinal(p, ppc_ptr_t<uint8_t>::from_host(out.as<uint8_t*>()), out_size, sha384);
}
void XeCryptSha384_entry(mapped_void in1, u32 n1, mapped_void in2, u32 n2, mapped_void in3, u32 n3,
                         mapped_void out, u32 out_size) {
  ShaWideOneShot(true, in1, n1, in2, n2, in3, n3, out, out_size);
}
void XeCryptSha512_entry(mapped_void in1, u32 n1, mapped_void in2, u32 n2, mapped_void in3, u32 n3,
                         mapped_void out, u32 out_size) {
  ShaWideOneShot(false, in1, n1, in2, n2, in3, n3, out, out_size);
}

// ---- Single DES, same shape as the existing DES3 entries (keytab = the 16 sub keys). ----
void XeCryptDesKey_entry(ppc_ptr_t<XECRYPT_DES_STATE> state_ptr, mapped_u64 key) {
  DES des(*key);
  std::memcpy(state_ptr->keytab, des.get_sub_key(), 128);
}
void XeCryptDesEcb_entry(ppc_ptr_t<XECRYPT_DES_STATE> state_ptr, mapped_u64 inp, mapped_u64 out,
                         u32 encrypt) {
  DES des(reinterpret_cast<ui64*>(state_ptr->keytab));
  *out = encrypt ? des.encrypt(*inp) : des.decrypt(*inp);
}
void XeCryptDesCbc_entry(ppc_ptr_t<XECRYPT_DES_STATE> state_ptr, mapped_u64 inp, u32 inp_size,
                         mapped_u64 out, mapped_u64 feed, u32 encrypt) {
  DES des(reinterpret_cast<ui64*>(state_ptr->keytab));
  assert_true(inp_size % 8 == 0);
  uint64_t last_block = *feed;
  for (uint32_t i = 0; i < inp_size / 8; i++) {
    uint64_t block = inp[i];
    if (encrypt) {
      last_block = des.encrypt(block ^ last_block);
      out[i] = last_block;
    } else {
      out[i] = des.decrypt(block) ^ last_block;
      last_block = block;
    }
  }
  *feed = last_block;
}


REX_EXPORT_STUB_RETURN(__imp__XeKeysGetConsoleCertificate, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnDwLeDhEqualBase, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnDwLeDhInvalBase, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnDwLeDhModExp, 0);
REX_EXPORT(__imp__XeCryptBnDw_Copy, rex::kernel::xboxkrnl::XeCryptBnDw_Copy_entry)
REX_EXPORT(__imp__XeCryptBnDw_SwapLeBe, rex::kernel::xboxkrnl::XeCryptBnDw_SwapLeBe_entry)
REX_EXPORT(__imp__XeCryptBnDw_Zero, rex::kernel::xboxkrnl::XeCryptBnDw_Zero_entry)
REX_EXPORT(__imp__XeCryptBnDwLePkcs1Format, rex::kernel::xboxkrnl::XeCryptBnDwLePkcs1Format_entry)
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwBeSigCreate, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwBeSigFormat, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwBeSigVerify, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwNeModExp, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwNeModExpRoot, 0);
REX_EXPORT(__imp__XeCryptBnQwNeModInv, rex::kernel::xboxkrnl::XeCryptBnQwNeModInv_entry)
REX_EXPORT(__imp__XeCryptBnQwNeModMul, rex::kernel::xboxkrnl::XeCryptBnQwNeModMul_entry)
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwNeRsaKeyGen, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptBnQwNeRsaPrvCrypt, 0);
REX_EXPORT(__imp__XeCryptBnQw_Copy, rex::kernel::xboxkrnl::XeCryptBnQw_Copy_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapDwQw, rex::kernel::xboxkrnl::XeCryptBnQw_SwapDwQw_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapLeBe, rex::kernel::xboxkrnl::XeCryptBnQw_SwapLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQw_Zero, rex::kernel::xboxkrnl::XeCryptBnQw_Zero_entry)
REX_EXPORT_STUB_RETURN(__imp__XeCryptChainAndSumMac, 0);
REX_EXPORT(__imp__XeCryptDesKey, rex::kernel::xboxkrnl::XeCryptDesKey_entry)
REX_EXPORT(__imp__XeCryptDesEcb, rex::kernel::xboxkrnl::XeCryptDesEcb_entry)
REX_EXPORT(__imp__XeCryptDesCbc, rex::kernel::xboxkrnl::XeCryptDesCbc_entry)
REX_EXPORT(__imp__XeCryptHmacMd5Init, rex::kernel::xboxkrnl::XeCryptHmacMd5Init_entry)
REX_EXPORT(__imp__XeCryptHmacMd5Update, rex::kernel::xboxkrnl::XeCryptHmacMd5Update_entry)
REX_EXPORT(__imp__XeCryptHmacMd5Final, rex::kernel::xboxkrnl::XeCryptHmacMd5Final_entry)
REX_EXPORT(__imp__XeCryptHmacMd5, rex::kernel::xboxkrnl::XeCryptHmacMd5_entry)
REX_EXPORT(__imp__XeCryptHmacShaInit, rex::kernel::xboxkrnl::XeCryptHmacShaInit_entry)
REX_EXPORT(__imp__XeCryptHmacShaUpdate, rex::kernel::xboxkrnl::XeCryptHmacShaUpdate_entry)
REX_EXPORT(__imp__XeCryptHmacShaFinal, rex::kernel::xboxkrnl::XeCryptHmacShaFinal_entry)
REX_EXPORT(__imp__XeCryptHmacShaVerify, rex::kernel::xboxkrnl::XeCryptHmacShaVerify_entry)
REX_EXPORT(__imp__XeCryptMd5Init, rex::kernel::xboxkrnl::XeCryptMd5Init_entry)
REX_EXPORT(__imp__XeCryptMd5Update, rex::kernel::xboxkrnl::XeCryptMd5Update_entry)
REX_EXPORT(__imp__XeCryptMd5Final, rex::kernel::xboxkrnl::XeCryptMd5Final_entry)
REX_EXPORT(__imp__XeCryptMd5, rex::kernel::xboxkrnl::XeCryptMd5_entry)
REX_EXPORT_STUB_RETURN(__imp__XeCryptParveEcb, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptParveCbcMac, 0);
REX_EXPORT(__imp__XeCryptRotSumSha, rex::kernel::xboxkrnl::XeCryptRotSumSha_entry)
REX_EXPORT(__imp__XeCryptSha256, rex::kernel::xboxkrnl::XeCryptSha256_entry)
REX_EXPORT(__imp__XeCryptSha384, rex::kernel::xboxkrnl::XeCryptSha384_entry)
REX_EXPORT(__imp__XeCryptSha512, rex::kernel::xboxkrnl::XeCryptSha512_entry)
REX_EXPORT(__imp__XeCryptBnQwNeCompare, rex::kernel::xboxkrnl::XeCryptBnQwNeCompare_entry)
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetFactoryChallenge, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSetFactoryResponse, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysInitializeFuses, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSaveBootLoader, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSaveKeyVault, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetStatus, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGeneratePrivateKey, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetKeyProperties, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSetKey, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGenerateRandomKey, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetKey, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetDigest, 0xC0000001);
REX_EXPORT(__imp__XeKeysGetConsoleID, rex::kernel::xboxkrnl::XeKeysGetConsoleID_entry)
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetConsoleType, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysQwNeRsaPrvCrypt, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysAesCbc, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDes2Cbc, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDesCbc, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSaveBootLoaderEx, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDes2CbcUsingKey, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDesCbcUsingKey, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysObfuscate, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysUnObfuscate, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysVerifyRSASignature, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSaveSystemUpdate, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysLockSystemUpdate, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysExecute, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetVersions, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSetRevocationList, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysExSaveKeyVault, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysExSetKey, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysExGetKey, 0xC0000225);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecurityInitialize, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecurityLoadSettings, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecuritySaveSettings, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecuritySetDetected, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecurityGetDetected, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecuritySetActivated, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecurityGetActivated, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDvdAuthAP25InstallTable, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDvdAuthAP25GetTableVersion, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetProtectedFlag, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSetProtectedFlag, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetUpdateSequence, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDvdAuthExActivate, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysRevokeSaveSettings, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetMediaID, 0xC0052000);
REX_EXPORT_STUB_RETURN(__imp__XeKeysLoadKeyVault, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysRevokeUpdateDynamic, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysSecuritySetStat, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysFcrtLoad, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysFcrtSave, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysFcrtSet, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysRevokeIsDeviceRevoked, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDvdAuthExSave, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysDvdAuthExInstall, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysObfuscateEx, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysUnObfuscateEx, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__XeKeysVerifyPIRSSignature, 1);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesCtr, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesCbcMac, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesDmMac, 0);
REX_EXPORT_STUB_RETURN(__imp__XeKeysGetTruncatedSecondaryConsoleId, 0);
REX_EXPORT(__imp__XeCryptSha224Init, rex::kernel::xboxkrnl::XeCryptSha224Init_entry)
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesCreateKeySchedule, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesEncryptOne, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesDecryptOne, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesCbcEncrypt, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesCbcDecrypt, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesGcmInitialize, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesGcmUpdate, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptAesGcmFinalize, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptEccGetCurveParameters, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptEccEcdhGenerateKeypair, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptEccEcdhExponentiate, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptEccEcdsaGenerateSignature, 0);
REX_EXPORT_STUB_RETURN(__imp__XeCryptEccEcdsaVerifySignature, 0);

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XeCryptRc4Key, rex::kernel::xboxkrnl::XeCryptRc4Key_entry)
REX_EXPORT(__imp__XeCryptRc4Ecb, rex::kernel::xboxkrnl::XeCryptRc4Ecb_entry)
REX_EXPORT(__imp__XeCryptRc4, rex::kernel::xboxkrnl::XeCryptRc4_entry)
REX_EXPORT(__imp__XeCryptShaInit, rex::kernel::xboxkrnl::XeCryptShaInit_entry)
REX_EXPORT(__imp__XeCryptShaUpdate, rex::kernel::xboxkrnl::XeCryptShaUpdate_entry)
REX_EXPORT(__imp__XeCryptShaFinal, rex::kernel::xboxkrnl::XeCryptShaFinal_entry)
REX_EXPORT(__imp__XeCryptSha, rex::kernel::xboxkrnl::XeCryptSha_entry)
REX_EXPORT(__imp__XeCryptSha256Init, rex::kernel::xboxkrnl::XeCryptSha256Init_entry)
REX_EXPORT(__imp__XeCryptSha256Update, rex::kernel::xboxkrnl::XeCryptSha256Update_entry)
REX_EXPORT(__imp__XeCryptSha256Final, rex::kernel::xboxkrnl::XeCryptSha256Final_entry)
REX_EXPORT(__imp__XeCryptSha384Init, rex::kernel::xboxkrnl::XeCryptSha384Init_entry)
REX_EXPORT(__imp__XeCryptSha384Update, rex::kernel::xboxkrnl::XeCryptSha384Update_entry)
REX_EXPORT(__imp__XeCryptSha384Final, rex::kernel::xboxkrnl::XeCryptSha384Final_entry)
REX_EXPORT(__imp__XeCryptSha512Init, rex::kernel::xboxkrnl::XeCryptSha512Init_entry)
REX_EXPORT(__imp__XeCryptSha512Update, rex::kernel::xboxkrnl::XeCryptSha512Update_entry)
REX_EXPORT(__imp__XeCryptSha512Final, rex::kernel::xboxkrnl::XeCryptSha512Final_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapDwQwLeBe, rex::kernel::xboxkrnl::XeCryptBnQw_SwapDwQwLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQwNeRsaPubCrypt, rex::kernel::xboxkrnl::XeCryptBnQwNeRsaPubCrypt_entry)
REX_EXPORT(__imp__XeCryptBnDwLePkcs1Verify, rex::kernel::xboxkrnl::XeCryptBnDwLePkcs1Verify_entry)
REX_EXPORT(__imp__XeCryptRandom, rex::kernel::xboxkrnl::XeCryptRandom_entry)
REX_EXPORT(__imp__XeCryptDesParity, rex::kernel::xboxkrnl::XeCryptDesParity_entry)
REX_EXPORT(__imp__XeCryptDes3Key, rex::kernel::xboxkrnl::XeCryptDes3Key_entry)
REX_EXPORT(__imp__XeCryptDes3Ecb, rex::kernel::xboxkrnl::XeCryptDes3Ecb_entry)
REX_EXPORT(__imp__XeCryptDes3Cbc, rex::kernel::xboxkrnl::XeCryptDes3Cbc_entry)
REX_EXPORT(__imp__XeCryptAesKey, rex::kernel::xboxkrnl::XeCryptAesKey_entry)
REX_EXPORT(__imp__XeCryptAesEcb, rex::kernel::xboxkrnl::XeCryptAesEcb_entry)
REX_EXPORT(__imp__XeCryptAesCbc, rex::kernel::xboxkrnl::XeCryptAesCbc_entry)
REX_EXPORT(__imp__XeCryptHmacSha, rex::kernel::xboxkrnl::XeCryptHmacSha_entry)
REX_EXPORT(__imp__XeKeysHmacSha, rex::kernel::xboxkrnl::XeKeysHmacSha_entry)
REX_EXPORT(__imp__XeKeysAesCbcUsingKey, rex::kernel::xboxkrnl::XeKeysAesCbcUsingKey_entry)
REX_EXPORT(__imp__XeKeysObscureKey, rex::kernel::xboxkrnl::XeKeysObscureKey_entry)
REX_EXPORT(__imp__XeKeysHmacShaUsingKey, rex::kernel::xboxkrnl::XeKeysHmacShaUsingKey_entry)
REX_EXPORT(__imp__XeKeysConsolePrivateKeySign,
           rex::kernel::xboxkrnl::XeKeysConsolePrivateKeySign_entry)
REX_EXPORT(__imp__XeKeysConsoleSignatureVerification,
           rex::kernel::xboxkrnl::XeKeysConsoleSignatureVerification_entry)

REX_EXPORT_STUB_RETURN(__imp__DevAuthGetStatistics, 0);
REX_EXPORT_STUB_RETURN(__imp__DevAuthShouldAlwaysEnforce, 0);
