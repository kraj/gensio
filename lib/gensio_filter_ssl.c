/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#include <errno.h>

#include <gensio/gensio_class.h>

#include "gensio_filter_ssl.h"

#ifdef HAVE_OPENSSL

#include <assert.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

struct gensio_ssl_filter_data {
    struct gensio_os_funcs *o;
    bool is_client;
    char *CAfilepath;
    char *keyfile;
    char *certfile;
    gensiods max_read_size;
    gensiods max_write_size;
    bool allow_authfail;
};

static void
gensio_do_ssl_init(void *cb_data)
{
    SSL_library_init();
}

static struct gensio_once gensio_ssl_init_once;

static void
gensio_ssl_initialize(struct gensio_os_funcs *o)
{
    o->call_once(o, &gensio_ssl_init_once, gensio_do_ssl_init, NULL);
}

struct ssl_filter {
    struct gensio_filter filter;
    struct gensio_os_funcs *o;
    bool is_client;
    bool connected;
    bool finish_close_on_write;
    struct gensio_lock *lock;

    SSL_CTX *ctx;
    SSL *ssl;
    BIO *ssl_bio;
    BIO *io_bio;
    X509 *remcert;

    bool expect_peer_cert;
    bool allow_authfail;

    /* This is data from SSL_read() that is waiting to be sent to the user. */
    unsigned char *read_data;
    gensiods read_data_pos;
    gensiods read_data_len;
    gensiods max_read_size;

    /*
     * This is data from the user waiting to be sent to SSL_write().  This
     * is required because if SSL_write() return that it needs I/O, it must
     * be called again with exactly the same data.
     */
    unsigned char *write_data;
    gensiods max_write_size;
    gensiods write_data_len;

    /* This is data from BIO_read() waiting to be sent to the lower layer. */
    unsigned char xmit_buf[1024];
    gensiods xmit_buf_pos;
    gensiods xmit_buf_len;
};

#define filter_to_ssl(v) gensio_container_of(v, struct ssl_filter, filter)

static void
ssl_lock(struct ssl_filter *sfilter)
{
    sfilter->o->lock(sfilter->lock);
}

static void
ssl_unlock(struct ssl_filter *sfilter)
{
    sfilter->o->unlock(sfilter->lock);
}

static void
ssl_set_callbacks(struct gensio_filter *filter,
		  gensio_filter_cb cb, void *cb_data)
{
    /* We don't currently use callbacks. */
}

static bool
ssl_ul_read_pending(struct gensio_filter *filter)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    char buf[1];
    bool rv;

    ssl_lock(sfilter);
    rv = sfilter->read_data_len || SSL_peek(sfilter->ssl, buf, 1) > 0;
    ssl_unlock(sfilter);
    return rv;
}

static bool
ssl_ll_write_pending(struct gensio_filter *filter)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    bool rv;

    ssl_lock(sfilter);
    rv = BIO_pending(sfilter->io_bio) || sfilter->write_data_len ||
	sfilter->xmit_buf_len;
    ssl_unlock(sfilter);
    return rv;
}

static bool
ssl_ll_read_needed(struct gensio_filter *filter)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    bool rv;

    ssl_lock(sfilter);
    rv = BIO_should_read(sfilter->io_bio);
    ssl_unlock(sfilter);
    return rv;
}

static int
ssl_verify_cb(int preverify_ok, X509_STORE_CTX *x509_ctx)
{
    /* Always succeed, check the result in ssl_check_open_done(). */
    return 1;
}

static int
ssl_check_open_done(struct gensio_filter *filter, struct gensio *io)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    long verify_err;
    int rv = 0;

    ssl_lock(sfilter);
    if (sfilter->expect_peer_cert) {
	sfilter->remcert = SSL_get_peer_certificate(sfilter->ssl);
	if (!sfilter->remcert) {
	    rv = ENOKEY;
	    goto out_unlock;
	}

	verify_err = SSL_get_verify_result(sfilter->ssl);
	if (verify_err != X509_V_OK) {
	    X509_free(sfilter->remcert);
	    sfilter->remcert = NULL;
	    rv = EKEYREJECTED;
	} else {
	    gensio_set_is_authenticated(io, true);
	}
    }
 out_unlock:
    if (rv && sfilter->allow_authfail)
	rv = 0;
    ssl_unlock(sfilter);
    return rv;
}

static int
ssl_try_connect(struct gensio_filter *filter, struct timeval *timeout)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    int rv, success;

    ssl_lock(sfilter);
    if (sfilter->is_client)
	success = SSL_connect(sfilter->ssl);
    else
	success = SSL_accept(sfilter->ssl);

    if (!success) {
	rv = ECOMM;
    } else if (success == 1) {
	sfilter->connected = true;
	rv = 0;
    } else {
	int err = SSL_get_error(sfilter->ssl, success);

	switch (err) {
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
	    rv = EINPROGRESS;
	    break;

	default:
	    rv = ECOMM;
	}
    }
    ssl_unlock(sfilter);
    return rv;
}

static int
ssl_try_disconnect(struct gensio_filter *filter, struct timeval *timeout)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    int success;
    int rv = EINPROGRESS;

    ssl_lock(sfilter);
    if (sfilter->finish_close_on_write) {
	sfilter->finish_close_on_write = false;
	rv = 0;
    } else {
	sfilter->connected = false;
	success = SSL_shutdown(sfilter->ssl);
	if (success == 1 || success < 0) {
	    if (BIO_pending(sfilter->io_bio))
		sfilter->finish_close_on_write = true;
	    else
		rv = 0;
	}
    }
    ssl_unlock(sfilter);

    return rv;
}

static int
ssl_ul_write(struct gensio_filter *filter,
	     gensio_ul_filter_data_handler handler, void *cb_data,
	     gensiods *rcount,
	     const unsigned char *buf, gensiods buflen, const char *const *auxdata)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    int err = 0;

    ssl_lock(sfilter);
    if (sfilter->write_data_len || buflen == 0) {
	if (rcount)
	    *rcount = 0;
    } else {
	if (buflen > sfilter->max_write_size)
	    buflen = sfilter->max_write_size;
	memcpy(sfilter->write_data, buf, buflen);
	sfilter->write_data_len = buflen;
	*rcount = buflen;
	buflen = 0;
    }

 restart:
    if (sfilter->xmit_buf_len) {
	gensiods written;

	err = handler(cb_data, &written,
		      sfilter->xmit_buf + sfilter->xmit_buf_pos,
		      sfilter->xmit_buf_len - sfilter->xmit_buf_pos, NULL);
	if (err) {
	    sfilter->xmit_buf_len = 0;
	} else {
	    sfilter->xmit_buf_pos += written;
	    if (sfilter->xmit_buf_pos >= sfilter->xmit_buf_len)
		sfilter->xmit_buf_len = 0;
	}
    }

    if (!err && sfilter->xmit_buf_len == 0 && sfilter->write_data_len > 0) {
	err = SSL_write(sfilter->ssl, sfilter->write_data,
			sfilter->write_data_len);
	if (err <= 0) {
	    err = SSL_get_error(sfilter->ssl, err);
	    switch (err) {
	    case SSL_ERROR_WANT_READ:
	    case SSL_ERROR_WANT_WRITE:
		err = 0;
		break;

	    default:
		err = ECOMM;
	    }
	} else {
	    assert(err == sfilter->write_data_len);
	    sfilter->write_data_len = 0;
	    err = 0;
	}
    }

    if (!err && sfilter->xmit_buf_len == 0) {
	int rdlen = BIO_read(sfilter->io_bio, sfilter->xmit_buf,
			     sizeof(sfilter->xmit_buf));

	/* FIXME - error handling? */
	if (rdlen > 0) {
	    sfilter->xmit_buf_len = rdlen;
	    sfilter->xmit_buf_pos = 0;
	    goto restart;
	}
    }
    ssl_unlock(sfilter);

    return err;
}

static int
ssl_ll_write(struct gensio_filter *filter,
	     gensio_ll_filter_data_handler handler, void *cb_data,
	     gensiods *rcount,
	     unsigned char *buf, gensiods buflen,
	     const char *const *auxdata)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    int err = 0;

    ssl_lock(sfilter);
    if (buflen > 0) {
	int wrlen = BIO_write(sfilter->io_bio, buf, buflen);

	/* FIXME - do we need error handling? */
	if (wrlen < 0)
	    wrlen = 0;
	*rcount = wrlen;
    }

 process_more:
    if (!sfilter->read_data_len && sfilter->connected) {
	int rlen;

	rlen = SSL_read(sfilter->ssl, sfilter->read_data,
			sfilter->max_read_size);
	if (rlen > 0)
	    sfilter->read_data_len = rlen;
	sfilter->read_data_pos = 0;
    }

    if (sfilter->read_data_len) {
	gensiods count = 0;

	ssl_unlock(sfilter);
	err = handler(cb_data, &count,
		      sfilter->read_data + sfilter->read_data_pos,
		      sfilter->read_data_len, NULL);
	ssl_lock(sfilter);
	if (!err) {
	    if (count >= sfilter->read_data_len) {
		sfilter->read_data_len = 0;
		sfilter->read_data_pos = 0;
		goto process_more;
	    } else {
		sfilter->read_data_len -= count;
		sfilter->read_data_pos += count;
	    }
	}
    }
    ssl_unlock(sfilter);

    return err;
}

static int
ssl_setup(struct gensio_filter *filter)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    int success;
    gensiods bio_size = sfilter->max_read_size * 2;

    sfilter->ssl = SSL_new(sfilter->ctx);
    if (!sfilter->ssl)
	return ENOMEM;

    /* The BIO has to be large enough to hold a full SSL key transaction. */
    if (bio_size < 4096)
	bio_size = 4096;
    success = BIO_new_bio_pair(&sfilter->ssl_bio, bio_size,
			       &sfilter->io_bio, bio_size);
    if (!success) {
	SSL_free(sfilter->ssl);
	sfilter->ssl = NULL;
	return ENOMEM;
    }

    SSL_set_bio(sfilter->ssl, sfilter->ssl_bio, sfilter->ssl_bio);

    if (sfilter->is_client)
	SSL_set_connect_state(sfilter->ssl);
    else
	SSL_set_accept_state(sfilter->ssl);

    return 0;
}

static void
ssl_cleanup(struct gensio_filter *filter)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);

    if (sfilter->remcert)
	X509_free(sfilter->remcert);
    sfilter->remcert = NULL;
    if (sfilter->ssl)
	SSL_free(sfilter->ssl);
    sfilter->ssl = NULL;
    sfilter->ssl_bio = NULL;
    sfilter->io_bio = NULL;
    sfilter->read_data_len = 0;
    sfilter->read_data_pos = 0;
    sfilter->xmit_buf_len = 0;
    sfilter->xmit_buf_pos = 0;
    sfilter->write_data_len = 0;
}

static void
ssl_free(struct gensio_filter *filter)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);

    if (sfilter->remcert)
	X509_free(sfilter->remcert);
    if (sfilter->ssl)
	SSL_free(sfilter->ssl);
    if (sfilter->io_bio)
	BIO_destroy_bio_pair(sfilter->io_bio);
    if (sfilter->ctx)
	SSL_CTX_free(sfilter->ctx);
    if (sfilter->lock)
	sfilter->o->free_lock(sfilter->lock);
    if (sfilter->read_data)
	sfilter->o->free(sfilter->o, sfilter->read_data);
    if (sfilter->write_data)
	sfilter->o->free(sfilter->o, sfilter->write_data);
    sfilter->o->free(sfilter->o, sfilter);
}

static int
ssl_filter_control(struct gensio_filter *filter, bool get, int op, char *data)
{
    struct ssl_filter *sfilter = filter_to_ssl(filter);
    char *s, *n, *end;
    int index = -1, len, count, nid;
    X509_NAME *nm;
    X509_NAME_ENTRY *e;
    ASN1_STRING *as;
    unsigned char *obj;
    int objlen;

    switch (op) {
    case GENSIO_CONTROL_GET_PEER_CERT_NAME:
	if (!get)
	    return ENOTSUP;
	if (!sfilter->remcert)
	    return ENXIO;
	len = strlen(data) + 1;
	s = strchr(data, ' ');
	if (s)
	    *s = '\0';
	n = data;
	s = strchr(data, ',');
	if (s) {
	    index = strtol(data, &end, 0);
	    if (*end != ',')
		return EINVAL;
	    n = end + 1;
	}
	nid = OBJ_sn2nid(n);
	if (nid == NID_undef) {
	    nid = OBJ_ln2nid(data);
	    if (nid == NID_undef)
		return EINVAL;
	}
	nm = X509_get_subject_name(sfilter->remcert);
	index = X509_NAME_get_index_by_NID(nm, nid, index);
	if (index < 0)
	    return ENOENT;
	count = snprintf(data, len, "%d,", index);
	if (count < len) {
	    data += count;
	    len -= count;
	} else {
	    return 0;
	}
	e = X509_NAME_get_entry(nm, index);
	as = X509_NAME_ENTRY_get_data(e);
	objlen = ASN1_STRING_to_UTF8(&obj, as);
	if (objlen < 0)
	    return ENOMEM;
	if (objlen >= len)
	    objlen = len - 1;
	memcpy(data, obj, objlen);
	data[objlen] = '\0';
	OPENSSL_free(obj);
	return 0;

    default:
	return ENOTSUP;
    }
}

static int gensio_ssl_filter_func(struct gensio_filter *filter, int op,
				  const void *func, void *data,
				  gensiods *count,
				  void *buf, const void *cbuf,
				  gensiods buflen,
				  const char *const *auxdata)
{
    switch (op) {
    case GENSIO_FILTER_FUNC_SET_CALLBACK:
	ssl_set_callbacks(filter, func, data);
	return 0;

    case GENSIO_FILTER_FUNC_UL_READ_PENDING:
	return ssl_ul_read_pending(filter);

    case GENSIO_FILTER_FUNC_UL_WRITE_PENDING:
	return ssl_ll_write_pending(filter);

    case GENSIO_FILTER_FUNC_LL_READ_NEEDED:
	return ssl_ll_read_needed(filter);

    case GENSIO_FILTER_FUNC_CHECK_OPEN_DONE:
	return ssl_check_open_done(filter, data);

    case GENSIO_FILTER_FUNC_TRY_CONNECT:
	return ssl_try_connect(filter, data);

    case GENSIO_FILTER_FUNC_TRY_DISCONNECT:
	return ssl_try_disconnect(filter, data);

    case GENSIO_FILTER_FUNC_UL_WRITE:
	return ssl_ul_write(filter, func, data, count, cbuf, buflen, buf);

    case GENSIO_FILTER_FUNC_LL_WRITE:
	return ssl_ll_write(filter, func, data, count, buf, buflen, NULL);

    case GENSIO_FILTER_FUNC_SETUP:
	return ssl_setup(filter);

    case GENSIO_FILTER_FUNC_CLEANUP:
	ssl_cleanup(filter);
	return 0;

    case GENSIO_FILTER_FUNC_FREE:
	ssl_free(filter);
	return 0;

    case GENSIO_FILTER_FUNC_CONTROL:
	return ssl_filter_control(filter, *((bool *) cbuf), buflen, data);

    case GENSIO_FILTER_FUNC_TIMEOUT:
    default:
	return ENOTSUP;
    }
}

struct gensio_filter *
gensio_ssl_filter_raw_alloc(struct gensio_os_funcs *o,
			    bool is_client,
			    SSL_CTX *ctx,
			    bool expect_peer_cert,
			    bool allow_authfail,
			    gensiods max_read_size,
			    gensiods max_write_size)
{
    struct ssl_filter *sfilter;

    sfilter = o->zalloc(o, sizeof(*sfilter));
    if (!sfilter)
	return NULL;
    
    sfilter->o = o;
    sfilter->is_client = is_client;
    sfilter->ctx = ctx;
    sfilter->max_write_size = max_write_size;
    sfilter->max_read_size = max_read_size;
    sfilter->expect_peer_cert = expect_peer_cert;
    sfilter->allow_authfail = allow_authfail;

    sfilter->lock = o->alloc_lock(o);
    if (!sfilter->lock)
	goto out_nomem;

    sfilter->read_data = o->zalloc(o, max_read_size);
    if (!sfilter->read_data)
	goto out_nomem;

    sfilter->write_data = o->zalloc(o, max_write_size);
    if (!sfilter->read_data)
	goto out_nomem;

    sfilter->filter.func = gensio_ssl_filter_func;
    return &sfilter->filter;

 out_nomem:
    ssl_free(&sfilter->filter);
    return NULL;
}

int
gensio_ssl_filter_config(struct gensio_os_funcs *o,
			 const char * const args[],
			 bool default_is_client,
			 struct gensio_ssl_filter_data **rdata)
{
    unsigned int i;
    struct gensio_ssl_filter_data *data = o->zalloc(o, sizeof(*data));
    const char *CAfilepath = NULL, *keyfile = NULL, *certfile = NULL;

    data->o = o;
    data->is_client = default_is_client;
    data->max_write_size = SSL3_RT_MAX_PLAIN_LENGTH;
    data->max_read_size = SSL3_RT_MAX_PLAIN_LENGTH;
    bool noCA = false;

    for (i = 0; args && args[i]; i++) {
	if (gensio_check_keyvalue(args[i], "CA", &CAfilepath))
	    continue;
	if (gensio_check_keyvalue(args[i], "key", &keyfile))
	    continue;
	if (gensio_check_keyvalue(args[i], "cert", &certfile))
	    continue;
	if (gensio_check_keyds(args[i], "readbuf", &data->max_read_size) > 0)
	    continue;
	if (gensio_check_keyds(args[i], "writebuf", &data->max_write_size) > 0)
	    continue;
	if (gensio_check_keyboolv(args[i], "mode", "client", "server",
				  &data->is_client) > 0)
	    continue;
	if (gensio_check_keybool(args[i], "noCA", &noCA) > 0)
	    continue;
	if (gensio_check_keybool(args[i], "allow-authfail",
				 &data->allow_authfail) > 0)
	    continue;
	return EINVAL;
    }

    if (data->is_client) {
	if (!CAfilepath && !noCA)
	    return ENOKEY;
    } else {
	if (!keyfile)
	    return ENOKEY;
    }

    if (keyfile && !certfile)
	certfile = keyfile;

    if (CAfilepath) {
	data->CAfilepath = gensio_strdup(o, CAfilepath);
	if (!data->CAfilepath)
	    return ENOMEM;
    }

    if (keyfile) {
	data->keyfile = gensio_strdup(o, keyfile);
	if (!data->keyfile) {
	    o->free(o, data->CAfilepath);
	    return ENOMEM;
	}
    }

    if (certfile) {
	data->certfile = gensio_strdup(o, certfile);
	if (!data->certfile) {
	    o->free(o, data->keyfile);
	    o->free(o, data->CAfilepath);
	    return ENOMEM;
	}
    }

    *rdata = data;

    return 0;
}

void
gensio_ssl_filter_config_free(struct gensio_ssl_filter_data *data)
{
    struct gensio_os_funcs *o;

    if (!data)
	return;

    o = data->o;
    if (data->CAfilepath)
	o->free(o, data->CAfilepath);
    if (data->keyfile)
	o->free(o, data->keyfile);
    if (data->certfile)
	o->free(o, data->certfile);
    o->free(o, data);
}

int
gensio_ssl_filter_alloc(struct gensio_ssl_filter_data *data,
			struct gensio_filter **rfilter)
{
    struct gensio_os_funcs *o = data->o;
    SSL_CTX *ctx = NULL;
    struct gensio_filter *filter;

    gensio_ssl_initialize(o);

    if (data->is_client)
	ctx = SSL_CTX_new(SSLv23_client_method());
    else
	ctx = SSL_CTX_new(SSLv23_server_method());
    if (!ctx)
	return ENOMEM;

    if (!data->is_client)
	/*
	 * In server mode, the certificate will not be requested unless
	 * mode is SSL_VERIFY_PEER.  But in that mode, it terminates
	 * the connection if there is no certificate in the default
	 * verify callback.  So use the below so that the server mode
	 * works like client mode, request a certificate, but don't
	 * terminate the connection automatically if it is not there
	 * or fails.  We will do that in the check open call.
	 */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, ssl_verify_cb);

    if (data->CAfilepath) {
	char *CAfile = NULL, *CApath = NULL;

	if (data->CAfilepath[strlen(data->CAfilepath) - 1] == '/')
	    CApath = data->CAfilepath;
	else
	    CAfile = data->CAfilepath;
	if (!SSL_CTX_load_verify_locations(ctx, CAfile, CApath))
	    goto err;
    }

    if (data->certfile) {
	if (!SSL_CTX_use_certificate_chain_file(ctx, data->certfile))
	    goto err;
	if (!SSL_CTX_use_PrivateKey_file(ctx, data->keyfile, SSL_FILETYPE_PEM))
	    goto err;
	if (!SSL_CTX_check_private_key(ctx))
	    goto err;
    }

    filter = gensio_ssl_filter_raw_alloc(o, data->is_client, ctx,
					 data->CAfilepath != NULL,
					 data->allow_authfail,
					 data->max_read_size,
					 data->max_write_size);
    if (!filter) {
	SSL_CTX_free(ctx);
	return ENOMEM;
    }

    *rfilter = filter;
    return 0;

 err:
    SSL_CTX_free(ctx);
    return EINVAL;
}
#else /* HAVE_OPENSSL */

int
gensio_ssl_filter_alloc(struct gensio_os_funcs *o, char *args[],
			gensiods max_read_size,
			struct gensio_filter **rfilter)
{
    return ENOSUP;
}

#endif /* HAVE_OPENSSL */
