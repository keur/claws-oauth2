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

#include "intl.h"
#include "utils.h"
#include "ssl.h"

SSL_CTX *ssl_ctx;

void ssl_init() {
    SSL_METHOD *meth;
		
    SSLeay_add_ssl_algorithms();
    meth = SSLv2_client_method();
    SSL_load_error_strings();
    ssl_ctx = SSL_CTX_new(meth);
    if(ssl_ctx == NULL) {
    	debug_print(_("SSL disabled\n"));
    } else {
	debug_print(_("SSL loaded\n"));
    }
}

void ssl_done() {
    if(ssl_ctx) {
	SSL_CTX_free(ssl_ctx);
    }
}

gboolean ssl_init_socket(SockInfo *sockinfo) {
    X509 *server_cert;

    if(ssl_ctx == NULL) {
	log_warning(_("SSL not available\n"));

	return FALSE;
    }

    sockinfo->ssl = SSL_new(ssl_ctx);
    if(sockinfo->ssl == NULL) {
	log_warning(_("Error creating ssl context\n"));

	return FALSE;
    }
    SSL_set_fd(sockinfo->ssl, sockinfo->sock);
    if(SSL_connect(sockinfo->ssl) == -1) {
	log_warning(_("SSL connect failed\n"));

	return FALSE;
    }

    /* Get the cipher */

    log_print(_("SSL connection using %s\n"), SSL_get_cipher(sockinfo->ssl));

    /* Get server's certificate (note: beware of dynamic allocation) */

    if((server_cert = SSL_get_peer_certificate(sockinfo->ssl)) != NULL) {
	char *str;
	
	log_print(_("Server certificate:\n"));

	if((str = X509_NAME_oneline(X509_get_subject_name (server_cert),0,0)) != NULL) {
		log_print(_("  Subject: %s\n"), str);
		free(str);
	}
	
	if((str = X509_NAME_oneline(X509_get_issuer_name  (server_cert),0,0)) != NULL) {
		log_print(_("  Issuer: %s\n"), str);
		free(str);
	}

	X509_free(server_cert);
    }
}

void ssl_done_socket(SockInfo *sockinfo) {
    if(sockinfo->ssl) {
	SSL_free(sockinfo->ssl);
    }
}

#endif /* USE_SSL */