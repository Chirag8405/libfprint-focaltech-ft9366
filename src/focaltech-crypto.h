#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <pk11pub.h>

G_BEGIN_DECLS

#define FT_CRYPTO_AES_KEY_SIZE 16
#define FT_CRYPTO_AES_IV_SIZE 16

typedef struct {
  gboolean initialized;
  guint8 key[FT_CRYPTO_AES_KEY_SIZE];
  guint8 iv[FT_CRYPTO_AES_IV_SIZE];
  PK11SlotInfo *slot;
} FtCryptoSession;

gboolean ft_crypto_session_init(FtCryptoSession *session,
                                const guint8 *key,
                                gsize key_len,
                                const guint8 *iv,
                                gsize iv_len,
                                GError **error);

gboolean ft_crypto_encrypt(FtCryptoSession *session,
                           const guint8 *plaintext,
                           gsize plaintext_len,
                           guint8 *ciphertext,
                           gsize ciphertext_len,
                           gsize *written,
                           GError **error);

gboolean ft_crypto_decrypt(FtCryptoSession *session,
                           const guint8 *ciphertext,
                           gsize ciphertext_len,
                           guint8 *plaintext,
                           gsize plaintext_len,
                           gsize *written,
                           GError **error);

void ft_crypto_session_clear(FtCryptoSession *session);

G_END_DECLS
