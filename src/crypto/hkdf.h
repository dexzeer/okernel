#ifndef HKDF_H
#define HKDF_H

#include <stdint.h>

// HKDF-SHA256 (RFC 5869): extract-then-expand.
// salt may be NULL (treated as 32 zero bytes per the RFC).
void hkdf_extract(const uint8_t* salt, uint32_t salt_len,
                  const uint8_t* ikm, uint32_t ikm_len,
                  uint8_t prk[32]);

// Returns 0 on success, -1 when okm_len exceeds 255*32 (RFC 5869 bound;
// callers must fail, not accept silently clamped output).
int hkdf_expand(const uint8_t prk[32], const uint8_t* info, uint32_t info_len,
                uint8_t* okm, uint32_t okm_len);

int hkdf(const uint8_t* salt, uint32_t salt_len,
          const uint8_t* ikm, uint32_t ikm_len,
          const uint8_t* info, uint32_t info_len,
          uint8_t* okm, uint32_t okm_len);

#endif
