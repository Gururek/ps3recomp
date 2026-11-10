/*
 * ps3recomp - cellSsl HLE
 *
 * SSL/TLS stub. Provides the API surface for cellHttp's HTTPS support.
 * Actual TLS would require OpenSSL/mbedTLS integration.
 */

#ifndef PS3RECOMP_CELL_SSL_H
#define PS3RECOMP_CELL_SSL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
/* Official cellSsl error values used by the lifecycle and certificate loader. */
#define CELL_SSL_ERROR_NOT_INITIALIZED       0x80740001
#define CELL_SSL_ERROR_ALREADY_INITIALIZED   0x80740002
#define CELL_SSL_ERROR_NO_BUFFER             0x80740004
#define CELL_SSL_ERROR_UNRETRIEVABLE         0x80740006
#define CELL_SSL_ERROR_NOT_FOUND             0x80740008
#define CELL_SSL_ERROR_NO_MEMORY             0x80740035
#define CELL_SSL_ERROR_UNKNOWN_LOAD_CERT     0x80740037

/* Legacy helpers still used by the otherwise independent certificate getters. */
#define CELL_SSL_ERROR_INVALID_ARG           0x80720103
#define CELL_SSL_ERROR_HANDSHAKE_FAILED      0x80720105
#define CELL_SSL_ERROR_NOT_CONNECTED         0x80720106

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellSslCertId;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize);
s32 cellSslEnd(void);

/* Certificate management */
s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required);
s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize);
s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize);
s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time);
s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time);

/* Entropy / RNG */
s32 cellSslGetRandomNumber(u8* buf, u32 size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SSL_H */
