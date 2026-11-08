/**
 * @file sha256.h
 * @brief Hex SHA-256 helper shared with the legacy ULA derivation.
 */

#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
/* SHA-256 of buf, written as 64 lowercase hex chars + NUL into hex[65] */
void sha256_hex(const unsigned char *buf, size_t len, char hex[65]);
#endif
