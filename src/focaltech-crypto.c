#include "focaltech-crypto.h"

#include <nss.h>
#include <pk11pub.h>
#include <prerror.h>
#include <secitem.h>

static gsize nss_init_once = 0;
static gboolean nss_init_ok = FALSE;

static gboolean
ft_crypto_ensure_nss(GError **error)
{
  if (g_once_init_enter(&nss_init_once)) {
    nss_init_ok = (NSS_NoDB_Init(NULL) == SECSuccess);
    g_once_init_leave(&nss_init_once, 1);
  }

  if (!nss_init_ok) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "NSS_NoDB_Init failed (PR error %d)",
                PR_GetError());
    return FALSE;
  }

  return TRUE;
}

static gboolean
ft_crypto_cipher(FtCryptoSession *session,
                 const guint8 *input,
                 gsize input_len,
                 guint8 *output,
                 gsize output_len,
                 gsize *written,
                 CK_ATTRIBUTE_TYPE usage,
                 GError **error)
{
  SECItem key_item = {siBuffer, (guchar *) session->key, FT_CRYPTO_AES_KEY_SIZE};
  SECItem iv_item = {siBuffer, (guchar *) session->iv, FT_CRYPTO_AES_IV_SIZE};
  SECItem *params = NULL;
  PK11SymKey *sym_key = NULL;
  PK11Context *ctx = NULL;
  int out_len = 0;
  int final_len = 0;
  gboolean ret = FALSE;

  g_return_val_if_fail(session != NULL, FALSE);

  if (!session->initialized) {
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Crypto session not initialized");
    return FALSE;
  }

  if (input_len % FT_CRYPTO_AES_KEY_SIZE != 0U) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Input length (%zu) must be aligned to %d bytes",
                input_len,
                FT_CRYPTO_AES_KEY_SIZE);
    return FALSE;
  }

  if (output_len < input_len) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NO_SPACE,
                "Output buffer too small (%zu < %zu)",
                output_len,
                input_len);
    return FALSE;
  }

  sym_key = PK11_ImportSymKey(session->slot,
                              CKM_AES_CBC,
                              PK11_OriginUnwrap,
                              usage,
                              &key_item,
                              NULL);
  if (sym_key == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "PK11_ImportSymKey failed (PR error %d)",
                PR_GetError());
    goto out;
  }

  params = PK11_ParamFromIV(CKM_AES_CBC, &iv_item);
  if (params == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "PK11_ParamFromIV failed (PR error %d)",
                PR_GetError());
    goto out;
  }

  ctx = PK11_CreateContextBySymKey(CKM_AES_CBC, usage, sym_key, params);
  if (ctx == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "PK11_CreateContextBySymKey failed (PR error %d)",
                PR_GetError());
    goto out;
  }

  if (PK11_CipherOp(ctx,
                    output,
                    &out_len,
                    (int) output_len,
                    (guchar *) input,
                    (int) input_len) != SECSuccess) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "PK11_CipherOp failed (PR error %d)",
                PR_GetError());
    goto out;
  }

  if (PK11_DigestFinal(ctx,
                       output + out_len,
                       (unsigned int *) &final_len,
                       (unsigned int) (output_len - (gsize) out_len)) != SECSuccess) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "PK11_DigestFinal failed (PR error %d)",
                PR_GetError());
    goto out;
  }

  if (written != NULL)
    *written = (gsize) out_len + (gsize) final_len;

  ret = TRUE;

out:
  if (ctx != NULL)
    PK11_DestroyContext(ctx, PR_TRUE);
  if (params != NULL)
    SECITEM_FreeItem(params, PR_TRUE);
  if (sym_key != NULL)
    PK11_FreeSymKey(sym_key);

  return ret;
}

gboolean
ft_crypto_session_init(FtCryptoSession *session,
                       const guint8 *key,
                       gsize key_len,
                       const guint8 *iv,
                       gsize iv_len,
                       GError **error)
{
  g_return_val_if_fail(session != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(iv != NULL, FALSE);

  if (!ft_crypto_ensure_nss(error))
    return FALSE;

  if (key_len != FT_CRYPTO_AES_KEY_SIZE || iv_len != FT_CRYPTO_AES_IV_SIZE) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "AES-CBC requires %d-byte key and %d-byte IV",
                FT_CRYPTO_AES_KEY_SIZE,
                FT_CRYPTO_AES_IV_SIZE);
    return FALSE;
  }

  ft_crypto_session_clear(session);

  memcpy(session->key, key, FT_CRYPTO_AES_KEY_SIZE);
  memcpy(session->iv, iv, FT_CRYPTO_AES_IV_SIZE);

  session->slot = PK11_GetBestSlot(CKM_AES_CBC, NULL);
  if (session->slot == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "PK11_GetBestSlot failed (PR error %d)",
                PR_GetError());
    return FALSE;
  }

  session->initialized = TRUE;
  return TRUE;
}

gboolean
ft_crypto_encrypt(FtCryptoSession *session,
                  const guint8 *plaintext,
                  gsize plaintext_len,
                  guint8 *ciphertext,
                  gsize ciphertext_len,
                  gsize *written,
                  GError **error)
{
  g_return_val_if_fail(plaintext != NULL, FALSE);
  g_return_val_if_fail(ciphertext != NULL, FALSE);

  return ft_crypto_cipher(session,
                          plaintext,
                          plaintext_len,
                          ciphertext,
                          ciphertext_len,
                          written,
                          CKA_ENCRYPT,
                          error);
}

gboolean
ft_crypto_decrypt(FtCryptoSession *session,
                  const guint8 *ciphertext,
                  gsize ciphertext_len,
                  guint8 *plaintext,
                  gsize plaintext_len,
                  gsize *written,
                  GError **error)
{
  g_return_val_if_fail(ciphertext != NULL, FALSE);
  g_return_val_if_fail(plaintext != NULL, FALSE);

  return ft_crypto_cipher(session,
                          ciphertext,
                          ciphertext_len,
                          plaintext,
                          plaintext_len,
                          written,
                          CKA_DECRYPT,
                          error);
}

void
ft_crypto_session_clear(FtCryptoSession *session)
{
  if (session == NULL)
    return;

  if (session->slot != NULL)
    PK11_FreeSlot(session->slot);

  memset(session, 0, sizeof(*session));
}
