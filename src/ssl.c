/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2001 Hiroyuki Yamamoto
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#if USE_SSL

#include "defs.h"

#include <glib.h>

#include "intl.h"
#include "utils.h"
#include "ssl.h"

static SSL_CTX *ssl_ctx_SSLv23;
static SSL_CTX *ssl_ctx_TLSv1;

void ssl_init(void)
{
    SSL_library_init();
    SSL_load_error_strings();

    ssl_ctx_SSLv23 = SSL_CTX_new(SSLv23_client_method());
    if (ssl_ctx_SSLv23 == NULL) {
	debug_print(_("SSLv23 not available\n"));
    } else {
	debug_print(_("SSLv23 available\n"));
    }

    ssl_ctx_TLSv1 = SSL_CTX_new(TLSv1_client_method());
    if (ssl_ctx_TLSv1 == NULL) {
	debug_print(_("TLSv1 not available\n"));
    } else {
	debug_print(_("TLSv1 available\n"));
    }
}

void ssl_done(void)
{
    if (ssl_ctx_SSLv23) {
	SSL_CTX_free(ssl_ctx_SSLv23);
    }

    if (ssl_ctx_TLSv1) {
	SSL_CTX_free(ssl_ctx_TLSv1);
    }
}

gboolean ssl_init_socket(SockInfo *sockinfo)
{
    return ssl_init_socket_with_method(sockinfo, SSL_METHOD_SSLv23);
}

gboolean ssl_init_socket_with_method(SockInfo *sockinfo, SSLMethod method)
{
    X509 *server_cert;
    gint ret;

    switch (method) {
	case SSL_METHOD_SSLv23:
	    if (!ssl_ctx_SSLv23) {
		log_warning(_("SSL method not available\n"));
		return FALSE;
	    }
	    sockinfo->ssl = SSL_new(ssl_ctx_SSLv23);
	    break;
	case SSL_METHOD_TLSv1:
	    if (!ssl_ctx_TLSv1) {
		log_warning(_("SSL method not available\n"));
		return FALSE;
	    }
	    sockinfo->ssl = SSL_new(ssl_ctx_TLSv1);
	    break;
	default:
	    log_warning(_("Unknown SSL method *PROGRAM BUG*\n"));
	    return FALSE;
	    break;
    }

    if (sockinfo->ssl == NULL) {
	log_warning(_("Error creating ssl context\n"));
	return FALSE;
    }

    SSL_set_fd(sockinfo->ssl, sockinfo->sock);
    if ((ret = SSL_connect(sockinfo->ssl)) == -1) {
	log_warning(_("SSL connect failed (%s)\n"),
		    ERR_error_string(ERR_get_error(), NULL));
	return FALSE;
    }

    /* Get the cipher */

    log_print(_("SSL connection using %s\n"), SSL_get_cipher(sockinfo->ssl));

    /* Get server's certificate (note: beware of dynamic allocation) */

    if ((server_cert = SSL_get_peer_certificate(sockinfo->ssl)) != NULL) {
	gchar *str;

	log_print(_("Server certificate:\n"));

	if ((str = X509_NAME_oneline(X509_get_subject_name(server_cert), 0, 0)) != NULL) {
		log_print(_("  Subject: %s\n"), str);
		free(str);
	}

	if ((str = X509_NAME_oneline(X509_get_issuer_name(server_cert), 0, 0)) != NULL) {
		log_print(_("  Issuer: %s\n"), str);
		free(str);
	}

	X509_free(server_cert);
    }

    return TRUE;
}

void ssl_done_socket(SockInfo *sockinfo)
{
    if (sockinfo->ssl) {
	SSL_free(sockinfo->ssl);
    }
}

#endif /* USE_SSL */
