/*
 * Copyright (C) 2002-2017 ProcessOne, SARL. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <string.h>
#include <erl_nif.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <ctype.h>
#include "options.h"
#include "uthash.h"

#define BUF_SIZE 1024

#define enif_alloc malloc
#define enif_free free
#define enif_realloc realloc

typedef struct {
    BIO *bio_read;
    BIO *bio_write;
    SSL *ssl;
    int handshakes;
    ErlNifMutex *mtx;
    int valid;
    char *send_buffer;
    int send_buffer_size;
    int send_buffer_len;
    char *send_buffer2;
    int send_buffer2_size;
    int send_buffer2_len;
    char *cert_file;
    char *ciphers;
    char *dh_file;
    char *ca_file;
    long options;
    char *sni_error;
} state_t;

static int ssl_index;

#ifdef _WIN32
typedef unsigned __int32 uint32_t;
#endif

#ifndef SSL_OP_NO_TICKET
#define SSL_OP_NO_TICKET 0
#endif

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined LIBRESSL_VERSION_NUMBER
#define DH_set0_pqg(dh, dh_p, param, dh_g) (dh)->p = dh_p; (dh)->g = dh_g
#endif

void __free(void *ptr, size_t size) {
  enif_free(ptr);
}

#undef uthash_malloc
#undef uthash_free
#define uthash_malloc enif_alloc
#define uthash_free __free

#if OPENSSL_VERSION_NUMBER >= 0x10100000L || OPENSSL_VERSION_NUMBER < 0x10002000
#undef SSL_CTX_set_ecdh_auto
#define SSL_CTX_set_ecdh_auto(A, B) do {} while(0)
#endif

#define CIPHERS "DEFAULT:!EXPORT:!LOW:!RC4:!SSLv2"

static ErlNifResourceType *tls_state_t = NULL;
static ErlNifMutex **mtx_buf = NULL;

/**
 * Prepare the SSL options flag.
 **/
static int set_option_flag(const unsigned char *opt, size_t len, long *flag) {
    ssl_option_t *p;
    for (p = ssl_options; p->name; p++) {
        if (!memcmp(opt, p->name, len) && p->name[len] == '\0') {
            *flag |= p->code;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char *key;
    char *file;
    time_t key_mtime;
    time_t dh_mtime;
    time_t ca_mtime;
    SSL_CTX *ssl_ctx;
    UT_hash_handle hh;
} cert_info_t;

static cert_info_t *certs_map = NULL;
static cert_info_t *certfiles_map = NULL;

static ErlNifRWLock *certs_map_lock = NULL;
static ErlNifRWLock *certfiles_map_lock = NULL;

static void free_cert_info(cert_info_t *info) {
  if (info) {
    enif_free(info->key);
    enif_free(info->file);
    if (info->ssl_ctx)
      SSL_CTX_free(info->ssl_ctx);
    enif_free(info);
  }
}

static state_t *init_tls_state() {
    state_t *state = enif_alloc_resource(tls_state_t, sizeof(state_t));
    if (!state) return NULL;
    memset(state, 0, sizeof(state_t));
    state->mtx = enif_mutex_create("");
    if (!state->mtx) return NULL;
    state->valid = 1;
    return state;
}

static void destroy_tls_state(ErlNifEnv *env, void *data) {
    state_t *state = (state_t *) data;
    if (state) {
        if (state->ssl)
            SSL_free(state->ssl);
        if (state->mtx)
            enif_mutex_destroy(state->mtx);
        if (state->send_buffer)
            enif_free(state->send_buffer);
        if (state->send_buffer2)
            enif_free(state->send_buffer2);
	if (state->cert_file)
	    enif_free(state->cert_file);
        memset(state, 0, sizeof(state_t));
    }
}

static void locking_callback(int mode, int n, const char *file, int line) {
    if (mode & CRYPTO_LOCK)
        enif_mutex_lock(mtx_buf[n]);
    else
        enif_mutex_unlock(mtx_buf[n]);
}

static void thread_id_callback(CRYPTO_THREADID *id) {
    CRYPTO_THREADID_set_pointer(id, enif_thread_self());
}

static int atomic_add_callback(int *pointer, int amount, int type, const char *file, int line) {
    return __sync_add_and_fetch(pointer, amount);
}

static int load(ErlNifEnv *env, void **priv, ERL_NIF_TERM load_info) {
    int i;

    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();

    mtx_buf = enif_alloc(CRYPTO_num_locks() * sizeof(ErlNifMutex *));
    for (i = 0; i < CRYPTO_num_locks(); i++)
        mtx_buf[i] = enif_mutex_create("");

    CRYPTO_set_add_lock_callback(atomic_add_callback);
    CRYPTO_set_locking_callback(locking_callback);
    CRYPTO_THREADID_set_callback(thread_id_callback);

    certs_map_lock = enif_rwlock_create("certs_map_lock");
    certfiles_map_lock = enif_rwlock_create("certfiles_map_lock");

    ssl_index = SSL_get_ex_new_index(0, "ssl index", NULL, NULL, NULL);
    ErlNifResourceFlags flags = ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER;
    tls_state_t = enif_open_resource_type(env, NULL, "tls_state_t",
                                          destroy_tls_state,
                                          flags, NULL);
    return 0;
}

static void unload(ErlNifEnv *env, void *priv) {
    int i;
    cert_info_t *info = NULL;
    cert_info_t *tmp = NULL;

    enif_rwlock_rwlock(certs_map_lock);
    HASH_ITER(hh, certs_map, info, tmp) {
      HASH_DEL(certs_map, info);
      free_cert_info(info);
    }
    enif_rwlock_rwunlock(certs_map_lock);
    enif_rwlock_rwlock(certfiles_map_lock);
    HASH_ITER(hh, certfiles_map, info, tmp) {
      HASH_DEL(certfiles_map, info);
      free_cert_info(info);
    }
    enif_rwlock_rwunlock(certfiles_map_lock);
    enif_rwlock_destroy(certs_map_lock);
    enif_rwlock_destroy(certfiles_map_lock);
    certs_map = NULL;
    certs_map_lock = NULL;
    certfiles_map = NULL;
    certfiles_map_lock = NULL;
    for (i = 0; i < CRYPTO_num_locks(); i++)
        enif_mutex_destroy(mtx_buf[i]);
}

static int is_modified(char *file, time_t *known_mtime) {
    struct stat file_stat;

    if (file == NULL) {
        return 0;
    } else if (stat(file, &file_stat)) {
        *known_mtime = 0;
        return 1;
    } else {
        if (*known_mtime != file_stat.st_mtime) {
            *known_mtime = file_stat.st_mtime;
            return 1;
        } else
            return 0;
    }
}

static int verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
    return 1;
}

/*
 * ECDHE is enabled only on OpenSSL 1.0.0e and later.
 * See http://www.openssl.org/news/secadv_20110906.txt
 * for details.
 */
#ifndef OPENSSL_NO_ECDH

static void setup_ecdh(SSL_CTX *ctx) {
#if OPENSSL_VERSION_NUMBER < 0x10002000
    EC_KEY *ecdh;

    if (SSLeay() < 0x1000005fL) {
        return;
    }

    ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_ECDH_USE);
    SSL_CTX_set_tmp_ecdh(ctx, ecdh);

    EC_KEY_free(ecdh);
#else
    SSL_CTX_set_ecdh_auto(ctx, 1);
#endif
}

#endif

#ifndef OPENSSL_NO_DH
/*
1024-bit MODP Group with 160-bit prime order subgroup (RFC5114)
-----BEGIN DH PARAMETERS-----
MIIBDAKBgQCxC4+WoIDgHd6S3l6uXVTsUsmfvPsGo8aaap3KUtI7YWBz4oZ1oj0Y
mDjvHi7mUsAT7LSuqQYRIySXXDzUm4O/rMvdfZDEvXCYSI6cIZpzck7/1vrlZEc4
+qMaT/VbzMChUa9fDci0vUW/N982XBpl5oz9p21NpwjfH7K8LkpDcQKBgQCk0cvV
w/00EmdlpELvuZkF+BBN0lisUH/WQGz/FCZtMSZv6h5cQVZLd35pD1UE8hMWAhe0
sBuIal6RVH+eJ0n01/vX07mpLuGQnQ0iY/gKdqaiTAh6CR9THb8KAWm2oorWYqTR
jnOvoy13nVkY0IvIhY9Nzvl8KiSFXm7rIrOy5QICAKA=
-----END DH PARAMETERS-----
 */
static unsigned char dh1024_p[] = {
        0xB1, 0x0B, 0x8F, 0x96, 0xA0, 0x80, 0xE0, 0x1D, 0xDE, 0x92, 0xDE, 0x5E,
        0xAE, 0x5D, 0x54, 0xEC, 0x52, 0xC9, 0x9F, 0xBC, 0xFB, 0x06, 0xA3, 0xC6,
        0x9A, 0x6A, 0x9D, 0xCA, 0x52, 0xD2, 0x3B, 0x61, 0x60, 0x73, 0xE2, 0x86,
        0x75, 0xA2, 0x3D, 0x18, 0x98, 0x38, 0xEF, 0x1E, 0x2E, 0xE6, 0x52, 0xC0,
        0x13, 0xEC, 0xB4, 0xAE, 0xA9, 0x06, 0x11, 0x23, 0x24, 0x97, 0x5C, 0x3C,
        0xD4, 0x9B, 0x83, 0xBF, 0xAC, 0xCB, 0xDD, 0x7D, 0x90, 0xC4, 0xBD, 0x70,
        0x98, 0x48, 0x8E, 0x9C, 0x21, 0x9A, 0x73, 0x72, 0x4E, 0xFF, 0xD6, 0xFA,
        0xE5, 0x64, 0x47, 0x38, 0xFA, 0xA3, 0x1A, 0x4F, 0xF5, 0x5B, 0xCC, 0xC0,
        0xA1, 0x51, 0xAF, 0x5F, 0x0D, 0xC8, 0xB4, 0xBD, 0x45, 0xBF, 0x37, 0xDF,
        0x36, 0x5C, 0x1A, 0x65, 0xE6, 0x8C, 0xFD, 0xA7, 0x6D, 0x4D, 0xA7, 0x08,
        0xDF, 0x1F, 0xB2, 0xBC, 0x2E, 0x4A, 0x43, 0x71,
};
static unsigned char dh1024_g[] = {
        0xA4, 0xD1, 0xCB, 0xD5, 0xC3, 0xFD, 0x34, 0x12, 0x67, 0x65, 0xA4, 0x42,
        0xEF, 0xB9, 0x99, 0x05, 0xF8, 0x10, 0x4D, 0xD2, 0x58, 0xAC, 0x50, 0x7F,
        0xD6, 0x40, 0x6C, 0xFF, 0x14, 0x26, 0x6D, 0x31, 0x26, 0x6F, 0xEA, 0x1E,
        0x5C, 0x41, 0x56, 0x4B, 0x77, 0x7E, 0x69, 0x0F, 0x55, 0x04, 0xF2, 0x13,
        0x16, 0x02, 0x17, 0xB4, 0xB0, 0x1B, 0x88, 0x6A, 0x5E, 0x91, 0x54, 0x7F,
        0x9E, 0x27, 0x49, 0xF4, 0xD7, 0xFB, 0xD7, 0xD3, 0xB9, 0xA9, 0x2E, 0xE1,
        0x90, 0x9D, 0x0D, 0x22, 0x63, 0xF8, 0x0A, 0x76, 0xA6, 0xA2, 0x4C, 0x08,
        0x7A, 0x09, 0x1F, 0x53, 0x1D, 0xBF, 0x0A, 0x01, 0x69, 0xB6, 0xA2, 0x8A,
        0xD6, 0x62, 0xA4, 0xD1, 0x8E, 0x73, 0xAF, 0xA3, 0x2D, 0x77, 0x9D, 0x59,
        0x18, 0xD0, 0x8B, 0xC8, 0x85, 0x8F, 0x4D, 0xCE, 0xF9, 0x7C, 0x2A, 0x24,
        0x85, 0x5E, 0x6E, 0xEB, 0x22, 0xB3, 0xB2, 0xE5,
};

static int setup_dh(SSL_CTX *ctx, char *dh_file) {
    DH *dh;
    int res;

    if (dh_file != NULL) {
        BIO *bio = BIO_new_file(dh_file, "r");

        if (bio == NULL) {
            return 0;
        }
        dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
        BIO_free(bio);
        if (dh == NULL) {
            return 0;
        }
    } else {
        dh = DH_new();
        if (dh == NULL) {
            return 0;
        }
        BIGNUM *dh_p = BN_bin2bn(dh1024_p, sizeof(dh1024_p), NULL);
        BIGNUM *dh_g = BN_bin2bn(dh1024_g, sizeof(dh1024_g), NULL);
        if (dh_p == NULL || dh_g == NULL) {
            BN_free(dh_p);
            BN_free(dh_g);
            DH_free(dh);
            return 0;
        }

        DH_set0_pqg(dh, dh_p, NULL, dh_g);
    }

    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE);
    res = (int) SSL_CTX_set_tmp_dh(ctx, dh);

    DH_free(dh);
    return res;
}

#endif

static void ssl_info_callback(const SSL *s, int where, int ret) {
    state_t *d = (state_t *) SSL_get_ex_data(s, ssl_index);
    if ((where & SSL_CB_HANDSHAKE_START) && d->handshakes) {
        d->handshakes++;
    } else if ((where & SSL_CB_HANDSHAKE_DONE) && !d->handshakes) {
        d->handshakes++;
    }
}

static char *create_ssl_for_cert(char *, state_t *);

static cert_info_t *lookup_certfile(const char *domain) {
  cert_info_t *ret = NULL;
  cert_info_t *info = NULL;

  if (domain) {
    size_t len = strlen(domain);
    if (len) {
      char name[len+1];
      name[len] = 0;
      size_t i = 0;
      for (i=0; i<len; i++)
	name[i] = tolower(domain[i]);
      HASH_FIND_STR(certfiles_map, name, info);
      if (info && info->file)
	ret = info;
      else {
	/* Replace the first domain part with '*' and retry */
	char *dot = strchr(name, '.');
	if (dot != NULL && name[0] != '.') {
	  char *glob = dot - 1;
	  glob[0] = '*';
	  HASH_FIND_STR(certfiles_map, glob, info);
	  if (info && info->file)
	    ret = info;
	}
      }
    }
  }
  return ret;
}

static int ssl_sni_callback(const SSL *s, int *foo, void *data) {
  cert_info_t *info = NULL;
  char *err_str = NULL;
  const char *servername = NULL;
  int ret = SSL_TLSEXT_ERR_OK;
  state_t *state = (state_t *) SSL_get_ex_data(s, ssl_index);

  servername = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
  enif_rwlock_rlock(certfiles_map_lock);
  info = lookup_certfile(servername);
  if (info) {
    if (strcmp(info->file, state->cert_file))
      err_str = create_ssl_for_cert(info->file, state);
    if (err_str) {
      state->sni_error = err_str;
      ret = SSL_TLSEXT_ERR_ALERT_FATAL;
    }
  } else if (strlen(state->cert_file) == 0) {
    state->sni_error =
      "Failed to find a certificate matching the domain in SNI extension";
    ret = SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  enif_rwlock_runlock(certfiles_map_lock);

  return ret;
}

#define ERR_T(T) enif_make_tuple2(env, enif_make_atom(env, "error"), T)
#define OK_T(T) enif_make_tuple2(env, enif_make_atom(env, "ok"), T)
#define SEND_T(T) enif_make_tuple2(env, enif_make_atom(env, "send"), T)

#define SET_CERTIFICATE_FILE_ACCEPT 1
#define SET_CERTIFICATE_FILE_CONNECT 2
#define VERIFY_NONE 0x10000
#define COMPRESSION_NONE 0x100000

static ERL_NIF_TERM ssl_error(ErlNifEnv *env, const char *errstr) {
    size_t rlen;
    ErlNifBinary err;
    size_t errstrlen = strlen(errstr);
    unsigned long error_code = ERR_get_error();
    char *error_string = error_code ? ERR_error_string(error_code, NULL) : NULL;
    size_t error_string_length = error_string ? strlen(error_string) : 0;
    if (error_code)
        rlen = errstrlen + error_string_length + 2;
    else
        rlen = errstrlen;
    enif_alloc_binary(rlen, &err);
    memcpy(err.data, errstr, errstrlen);
    if (error_code) {
        memcpy(err.data + errstrlen, ": ", 2);
        memcpy(err.data + 2 + errstrlen, error_string, error_string_length);
    }
    return ERR_T(enif_make_binary(env, &err));
}

static SSL_CTX *create_new_ctx(char *cert_file, char *ciphers,
			       char *dh_file, char *ca_file,
			       char **err_str) {
  int res = 0;

  SSL_CTX *ctx = SSL_CTX_new(SSLv23_method());
  if (!ctx) {
    *err_str = "SSL_CTX_new failed";
    return NULL;
  }
  if (cert_file) {
    res = SSL_CTX_use_certificate_chain_file(ctx, cert_file);
    if (res <= 0) {
      SSL_CTX_free(ctx);
      *err_str = "SSL_CTX_use_certificate_file failed";
      return NULL;
    }
    res = SSL_CTX_use_PrivateKey_file(ctx, cert_file, SSL_FILETYPE_PEM);
    if (res <= 0) {
      SSL_CTX_free(ctx);
      *err_str = "SSL_CTX_use_PrivateKey_file failed";
      return NULL;
    }
    res = SSL_CTX_check_private_key(ctx);
    if (res <= 0) {
      SSL_CTX_free(ctx);
      *err_str = "SSL_CTX_check_private_key failed";
      return NULL;
    }
  }

  SSL_CTX_set_tlsext_servername_callback(ctx, &ssl_sni_callback);

  if (ciphers[0] == 0)
    SSL_CTX_set_cipher_list(ctx, CIPHERS);
  else
    SSL_CTX_set_cipher_list(ctx, ciphers);

#ifndef OPENSSL_NO_ECDH
  setup_ecdh(ctx);
#endif
#ifndef OPENSSL_NO_DH
  res = setup_dh(ctx, dh_file);
  if (res <= 0) {
    SSL_CTX_free(ctx);
    *err_str = "Setting DH parameters failed";
    return NULL;
  }
#endif

  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
  if (ca_file)
    SSL_CTX_load_verify_locations(ctx, ca_file, NULL);
  else
    SSL_CTX_set_default_verify_paths(ctx);

#ifdef SSL_MODE_RELEASE_BUFFERS
  SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
#endif
  SSL_CTX_set_verify(ctx,
		     SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE,
		     verify_callback);

  SSL_CTX_set_info_callback(ctx, &ssl_info_callback);

  *err_str = NULL;
  return ctx;
}

static void set_ctx(state_t *state, SSL_CTX *ctx) {
  if (state->ssl)
    SSL_set_SSL_CTX(state->ssl, ctx);
  else
    state->ssl = SSL_new(ctx);
}

static char *create_ssl_for_cert(char *cert_file, state_t *state) {
    char *ciphers = state->ciphers;
    char *dh_file = state->dh_file;
    char *ca_file = state->ca_file;
    long options = state->options;

    char *ret = NULL;
    cert_info_t *info = NULL;
    cert_info_t *new_info = NULL;
    cert_info_t *old_info = NULL;
    size_t key_size =
      strlen(cert_file) + strlen(ciphers) + 8 +
      strlen(dh_file) + strlen(ca_file) + 1;
    char key[key_size];
    sprintf(key, "%s%s%08lx%s%s", cert_file, ciphers,
            options, dh_file, ca_file);

    enif_rwlock_rlock(certs_map_lock);

    HASH_FIND_STR(certs_map, key, info);

    time_t key_mtime = info ? info->key_mtime : 0;
    time_t dh_mtime = info ? info->dh_mtime : 0;
    time_t ca_mtime = info ? info->ca_mtime : 0;

    if (strlen(cert_file) == 0) cert_file = NULL;
    if (strlen(dh_file) == 0) dh_file = NULL;
    if (strlen(ca_file) == 0) ca_file = NULL;

    if (is_modified(cert_file, &key_mtime) ||
        is_modified(dh_file, &dh_mtime) ||
        is_modified(ca_file, &ca_mtime) ||
        info == NULL) {
        enif_rwlock_runlock(certs_map_lock);

        enif_rwlock_rwlock(certs_map_lock);
	SSL_CTX *ctx = create_new_ctx(cert_file, ciphers, dh_file, ca_file, &ret);
	if (ret == NULL) {
	  new_info = enif_alloc(sizeof(cert_info_t));
	  if (new_info) {
	    memset(new_info, 0, sizeof(cert_info_t));
	    new_info->key = enif_alloc(key_size);
	    if (new_info->key) {
	      memcpy(new_info->key, key, key_size);
	      new_info->key_mtime = key_mtime;
	      new_info->dh_mtime = dh_mtime;
	      new_info->ca_mtime = ca_mtime;
	      new_info->ssl_ctx = ctx;
	      HASH_REPLACE_STR(certs_map, key, new_info, old_info);
	      free_cert_info(old_info);
	      set_ctx(state, ctx);
	    } else {
	      enif_free(new_info);
	      SSL_CTX_free(ctx);
	      ret = "Memory allocation failed";
	    }
	  } else {
	    SSL_CTX_free(ctx);
	    ret = "Memory allocation failed";
	  }
	}
	enif_rwlock_rwunlock(certs_map_lock);
    } else {
      set_ctx(state, info->ssl_ctx);
      enif_rwlock_runlock(certs_map_lock);
    }
    return ret;
}

static ERL_NIF_TERM open_nif(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]) {
    unsigned int command;
    unsigned int flags;
    char *cert_file = NULL, *ciphers = NULL;
    char *dh_file = NULL, *ca_file = NULL;
    char *sni = NULL;
    ErlNifBinary ciphers_bin;
    ErlNifBinary certfile_bin;
    ErlNifBinary protocol_options_bin;
    ErlNifBinary dhfile_bin;
    ErlNifBinary cafile_bin;
    ErlNifBinary sni_bin;
    ErlNifBinary alpn_bin;
    long options = 0L;
    state_t *state = NULL;

    ERR_clear_error();

    if (argc != 8)
        return enif_make_badarg(env);

    if (!enif_get_uint(env, argv[0], &flags))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[1], &certfile_bin))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[2], &ciphers_bin))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[3], &protocol_options_bin))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[4], &dhfile_bin))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[5], &cafile_bin))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[6], &sni_bin))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[7], &alpn_bin))
        return enif_make_badarg(env);

    command = flags & 0xffff;
    size_t po_len_left = protocol_options_bin.size;
    unsigned char *po = protocol_options_bin.data;

    while (po_len_left) {
        unsigned char *pos = memchr(po, '|', po_len_left);

        if (!pos) {
            set_option_flag(po, po_len_left, &options);
            break;
        }
        set_option_flag(po, pos - po, &options);
        po_len_left -= pos - po + 1;
        po = pos + 1;
    }

    cert_file = enif_alloc(certfile_bin.size + 1 +
			   ciphers_bin.size + 1 +
			   dhfile_bin.size + 1 +
			   cafile_bin.size + 1);
    ciphers = cert_file + certfile_bin.size + 1;
    dh_file = ciphers + ciphers_bin.size + 1;
    ca_file = dh_file + dhfile_bin.size + 1;
    if (!cert_file) {
        enif_free(cert_file);
        return enif_make_badarg(env);
    }

    if (sni_bin.size) {
      sni = enif_alloc(sni_bin.size + 1);
      if (!sni) {
        enif_free(cert_file);
	return enif_make_badarg(env);
      } else {
	sni[sni_bin.size] = 0;
      }
    }

    state = init_tls_state();
    if (!state) return ERR_T(enif_make_atom(env, "enomem"));

    memcpy(cert_file, certfile_bin.data, certfile_bin.size);
    cert_file[certfile_bin.size] = 0;
    memcpy(ciphers, ciphers_bin.data, ciphers_bin.size);
    ciphers[ciphers_bin.size] = 0;
    memcpy(dh_file, dhfile_bin.data, dhfile_bin.size);
    dh_file[dhfile_bin.size] = 0;
    memcpy(ca_file, cafile_bin.data, cafile_bin.size);
    ca_file[cafile_bin.size] = 0;
    memcpy(sni, sni_bin.data, sni_bin.size);

    state->cert_file = cert_file;
    state->ciphers = ciphers;
    state->dh_file = dh_file;
    state->ca_file = ca_file;
    state->options = options;

    char *err_str = create_ssl_for_cert(cert_file, state);
    if (err_str) {
        enif_free(cert_file);
	state->cert_file = NULL;
	enif_free(sni);
        return ssl_error(env, err_str);
    }

    if (!state->ssl) {
        enif_free(cert_file);
	state->cert_file = NULL;
	enif_free(sni);
        return ssl_error(env, "SSL_new failed");
    }

    if (flags & VERIFY_NONE)
        SSL_set_verify(state->ssl, SSL_VERIFY_NONE, verify_callback);

#ifdef SSL_OP_NO_COMPRESSION
    if (flags & COMPRESSION_NONE)
        SSL_set_options(state->ssl, SSL_OP_NO_COMPRESSION);
#endif

    SSL_set_ex_data(state->ssl, ssl_index, state);

    state->bio_read = BIO_new(BIO_s_mem());
    state->bio_write = BIO_new(BIO_s_mem());

    SSL_set_bio(state->ssl, state->bio_read, state->bio_write);

    if (command == SET_CERTIFICATE_FILE_ACCEPT) {
        options |= (SSL_OP_NO_TICKET | SSL_OP_ALL | SSL_OP_NO_SSLv2);

        SSL_set_options(state->ssl, options);

        SSL_set_accept_state(state->ssl);
    } else {
        options |= (SSL_OP_NO_TICKET | SSL_OP_NO_SSLv2);

        SSL_set_options(state->ssl, options);

	if (sni) SSL_set_tlsext_host_name(state->ssl, sni);

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
	if (alpn_bin.size)
	  SSL_set_alpn_protos(state->ssl, alpn_bin.data, alpn_bin.size);
#endif

        SSL_set_connect_state(state->ssl);
    }

    enif_free(sni);
    ERL_NIF_TERM result = enif_make_resource(env, state);
    enif_release_resource(state);
    return OK_T(result);
}

static ERL_NIF_TERM set_encrypted_input_nif(ErlNifEnv *env, int argc,
                                            const ERL_NIF_TERM argv[]) {
    state_t *state = NULL;
    ErlNifBinary input;

    if (argc != 2)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!enif_inspect_iolist_as_binary(env, argv[1], &input))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);
    enif_mutex_lock(state->mtx);

    if (!state->valid) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "closed"));
    }

    BIO_write(state->bio_read, input.data, input.size);
    enif_mutex_unlock(state->mtx);

    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM set_decrypted_output_nif(ErlNifEnv *env, int argc,
                                             const ERL_NIF_TERM argv[]) {
    state_t *state = NULL;
    int res;
    ErlNifBinary input;

    if (argc != 2)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!enif_inspect_iolist_as_binary(env, argv[1], &input))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);
    enif_mutex_lock(state->mtx);

    if (!state->valid) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "closed"));
    }

    if (input.size > 0) {
        ERR_clear_error();

        if (state->send_buffer != NULL) {
            if (state->send_buffer2 == NULL) {
                state->send_buffer2_len = input.size;
                state->send_buffer2_size = input.size;
                state->send_buffer2 = enif_alloc(state->send_buffer2_size);
                memcpy(state->send_buffer2, input.data, input.size);
            } else {
                if (state->send_buffer2_size <
                    state->send_buffer2_len + input.size) {
                    while (state->send_buffer2_size <
                           state->send_buffer2_len + input.size) {
                        state->send_buffer2_size *= 2;
                    }
                    state->send_buffer2 = enif_realloc(state->send_buffer2, state->send_buffer2_size);
                }
                memcpy(state->send_buffer2 + state->send_buffer2_len, input.data, input.size);
                state->send_buffer2_len += input.size;
            }
        } else {
            res = SSL_write(state->ssl, input.data, input.size);
            if (res <= 0) {
                res = SSL_get_error(state->ssl, res);
                if (res == SSL_ERROR_WANT_READ || res == SSL_ERROR_WANT_WRITE) {
                    state->send_buffer_len = input.size;
                    state->send_buffer_size = input.size;
                    state->send_buffer = enif_alloc(state->send_buffer_size);
                    memcpy(state->send_buffer, input.data, input.size);
                } else {
                    enif_mutex_unlock(state->mtx);
                    return ssl_error(env, "SSL_write failed");
                }
            }
        }
    }

    enif_mutex_unlock(state->mtx);
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM get_encrypted_output_nif(ErlNifEnv *env, int argc,
                                             const ERL_NIF_TERM argv[]) {
    state_t *state = NULL;
    size_t size;
    ErlNifBinary output;

    if (argc != 1)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);
    enif_mutex_lock(state->mtx);

    if (!state->valid) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "closed"));
    }

    ERR_clear_error();

    size = BIO_ctrl_pending(state->bio_write);
    if (!enif_alloc_binary(size, &output)) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "enomem"));
    }
    BIO_read(state->bio_write, output.data, size);
    enif_mutex_unlock(state->mtx);
    return OK_T(enif_make_binary(env, &output));
}

static ERL_NIF_TERM get_verify_result_nif(ErlNifEnv *env, int argc,
                                          const ERL_NIF_TERM argv[]) {
    long res;
    state_t *state = NULL;

    if (argc != 1)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);
    enif_mutex_lock(state->mtx);

    if (!state->valid) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "closed"));
    }

    ERR_clear_error();
    res = SSL_get_verify_result(state->ssl);
    enif_mutex_unlock(state->mtx);
    return OK_T(enif_make_long(env, res));
}

static ERL_NIF_TERM get_peer_certificate_nif(ErlNifEnv *env, int argc,
                                             const ERL_NIF_TERM argv[]) {
    X509 *cert = NULL;
    state_t *state = NULL;
    int rlen;
    ErlNifBinary output;

    if (argc != 1)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);
    enif_mutex_lock(state->mtx);

    if (!state->valid) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "closed"));
    }

    ERR_clear_error();

    cert = SSL_get_peer_certificate(state->ssl);
    if (!cert) {
        enif_mutex_unlock(state->mtx);
        return ssl_error(env, "SSL_get_peer_certificate failed");
    }
    rlen = i2d_X509(cert, NULL);
    if (rlen >= 0) {
        if (!enif_alloc_binary(rlen, &output)) {
            enif_mutex_unlock(state->mtx);
            return ERR_T(enif_make_atom(env, "enomem"));
        }
        i2d_X509(cert, &output.data);
        X509_free(cert);
        enif_mutex_unlock(state->mtx);
        return OK_T(enif_make_binary(env, &output));
    } else {
        X509_free(cert);
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "notfound"));
    }
}

static ERL_NIF_TERM get_decrypted_input_nif(ErlNifEnv *env, int argc,
                                            const ERL_NIF_TERM argv[]) {
    state_t *state = NULL;
    size_t rlen, size;
    int res;
    unsigned int req_size = 0;
    ErlNifBinary output;
    int retcode = 0;

    if (argc != 2)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!enif_get_uint(env, argv[1], &req_size))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);
    enif_mutex_lock(state->mtx);

    if (!state->valid) {
        enif_mutex_unlock(state->mtx);
        return ERR_T(enif_make_atom(env, "closed"));
    }

    ERR_clear_error();

    if (!SSL_is_init_finished(state->ssl)) {
        retcode = 2;
        res = SSL_do_handshake(state->ssl);
        if (res <= 0) {
            if (SSL_get_error(state->ssl, res) != SSL_ERROR_WANT_READ) {
                enif_mutex_unlock(state->mtx);
		int reason = ERR_GET_REASON(ERR_peek_error());
		if (reason == SSL_R_DATA_LENGTH_TOO_LONG ||
		    reason == SSL_R_PACKET_LENGTH_TOO_LONG ||
		    reason == SSL_R_UNKNOWN_PROTOCOL ||
		    reason == SSL_R_UNEXPECTED_MESSAGE ||
		    reason == SSL_R_WRONG_VERSION_NUMBER)
		  /* Do not report badly formed Client Hello */
		  return ERR_T(enif_make_atom(env, "closed"));
		else if (state->sni_error)
		  return ssl_error(env, state->sni_error);
		else
		  return ssl_error(env, "SSL_do_handshake failed");
            }
        }
    }
    if (SSL_is_init_finished(state->ssl)) {
        int i;
        for (i = 0; i < 2; i++)
            if (state->send_buffer != NULL) {
                res = SSL_write(state->ssl, state->send_buffer, state->send_buffer_len);
                if (res <= 0) {
                    char *error = "SSL_write failed";
                    enif_mutex_unlock(state->mtx);
                    return ERR_T(enif_make_string(env, error, ERL_NIF_LATIN1));
                }
                retcode = 2;
                enif_free(state->send_buffer);
                state->send_buffer = state->send_buffer2;
                state->send_buffer_len = state->send_buffer2_len;
                state->send_buffer_size = state->send_buffer2_size;
                state->send_buffer2 = NULL;
                state->send_buffer2_len = 0;
                state->send_buffer2_size = 0;
            }
        size = BUF_SIZE;
        rlen = 0;
        enif_alloc_binary(size, &output);
        res = 0;
        while ((req_size == 0 || rlen < req_size) &&
               (res = SSL_read(state->ssl,
                               output.data + rlen,
                               (req_size == 0 || req_size >= size) ?
                               size - rlen : req_size - rlen)) > 0) {
            //printf("%d bytes of decrypted data read from state machine\r\n",res);
            rlen += res;
            if (size - rlen < BUF_SIZE) {
                size *= 2;
                enif_realloc_binary(&output, size);
            }
        }

        if (state->handshakes > 1) {
            enif_release_binary(&output);
            char *error = "client renegotiations forbidden";
            enif_mutex_unlock(state->mtx);
            return ERR_T(enif_make_string(env, error, ERL_NIF_LATIN1));
        }

        if (res < 0) {
            int error = SSL_get_error(state->ssl, res);

            if (error == SSL_ERROR_WANT_READ) {
                //printf("SSL_read wants more data\r\n");
                //return 0;
            }
            // TODO
        }
        enif_realloc_binary(&output, rlen);
    } else {
        retcode = 2;
        enif_alloc_binary(0, &output);
    }
    enif_mutex_unlock(state->mtx);
    return retcode == 0 ? OK_T(enif_make_binary(env, &output))
                        : SEND_T(enif_make_binary(env, &output));
}

static ERL_NIF_TERM add_certfile_nif(ErlNifEnv *env, int argc,
				     const ERL_NIF_TERM argv[]) {
  ErlNifBinary domain, file;
  cert_info_t *info = NULL;
  cert_info_t *old_info = NULL;

  if (!enif_inspect_iolist_as_binary(env, argv[0], &domain))
    return enif_make_badarg(env);
  if (!enif_inspect_iolist_as_binary(env, argv[1], &file))
    return enif_make_badarg(env);

  info = enif_alloc(sizeof(cert_info_t));
  if (info) {
    memset(info, 0, sizeof(cert_info_t));
    info->key = enif_alloc(domain.size+1);
    info->file = enif_alloc(file.size+1);
    if (info->key && info->file) {
      memcpy(info->key, domain.data, domain.size);
      memcpy(info->file, file.data, file.size);
      info->key[domain.size] = 0;
      info->file[file.size] = 0;
      enif_rwlock_rwlock(certfiles_map_lock);
      HASH_REPLACE_STR(certfiles_map, key, info, old_info);
      free_cert_info(old_info);
      enif_rwlock_rwunlock(certfiles_map_lock);
    } else {
      free_cert_info(info);
    }
  }

  return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM delete_certfile_nif(ErlNifEnv *env, int argc,
					const ERL_NIF_TERM argv[]) {
  ErlNifBinary domain;
  char *ret = "false";
  cert_info_t *info = NULL;

  if (!enif_inspect_iolist_as_binary(env, argv[0], &domain))
    return enif_make_badarg(env);

  char key[domain.size+1];
  memcpy(key, domain.data, domain.size);
  key[domain.size] = 0;
  enif_rwlock_rwlock(certfiles_map_lock);
  HASH_FIND_STR(certfiles_map, key, info);
  if (info) {
    HASH_DEL(certfiles_map, info);
    free_cert_info(info);
    ret = "true";
  }
  enif_rwlock_rwunlock(certfiles_map_lock);

  return enif_make_atom(env, ret);
}

static ERL_NIF_TERM get_certfile_nif(ErlNifEnv *env, int argc,
				     const ERL_NIF_TERM argv[]) {
  ErlNifBinary domain;
  cert_info_t *info = NULL;
  ERL_NIF_TERM file, result;

  if (!enif_inspect_iolist_as_binary(env, argv[0], &domain))
    return enif_make_badarg(env);

  char key[domain.size+1];
  memcpy(key, domain.data, domain.size);
  key[domain.size] = 0;
  enif_rwlock_rlock(certfiles_map_lock);
  info = lookup_certfile(key);
  if (info) {
    unsigned char *tmp = enif_make_new_binary(env, strlen(info->file), &file);
    if (tmp) {
      memcpy(tmp, info->file, strlen(info->file));
      result = enif_make_tuple2(env, enif_make_atom(env, "ok"), file);
    } else
      result = enif_make_atom(env, "error");
  } else {
    result = enif_make_atom(env, "error");
  }
  enif_rwlock_runlock(certfiles_map_lock);

  return result;
}

static ERL_NIF_TERM invalidate_nif(ErlNifEnv *env, int argc,
                                   const ERL_NIF_TERM argv[]) {
    state_t *state = NULL;

    if (argc != 1)
        return enif_make_badarg(env);

    if (!enif_get_resource(env, argv[0], tls_state_t, (void *) &state))
        return enif_make_badarg(env);

    if (!state->mtx || !state->ssl) return enif_make_badarg(env);

    enif_mutex_lock(state->mtx);
    state->valid = 0;
    enif_mutex_unlock(state->mtx);

    return enif_make_atom(env, "ok");
}

static ErlNifFunc nif_funcs[] =
        {
                {"open_nif",                 8, open_nif},
                {"set_encrypted_input_nif",  2, set_encrypted_input_nif},
                {"set_decrypted_output_nif", 2, set_decrypted_output_nif},
                {"get_decrypted_input_nif",  2, get_decrypted_input_nif},
                {"get_encrypted_output_nif", 1, get_encrypted_output_nif},
                {"get_verify_result_nif",    1, get_verify_result_nif},
                {"get_peer_certificate_nif", 1, get_peer_certificate_nif},
		{"add_certfile_nif",         2, add_certfile_nif},
		{"delete_certfile_nif",      1, delete_certfile_nif},
		{"get_certfile_nif",         1, get_certfile_nif},
                {"invalidate_nif",           1, invalidate_nif}
        };

ERL_NIF_INIT(fast_tls, nif_funcs, load, NULL, NULL, unload)
