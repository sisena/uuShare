/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
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

#include "stdinc.h"
#include "SSLSocket.h"

#include "CryptoManager.h"
#include "ResourceManager.h"
#include "LogManager.h"

#include <openssl/err.h>

namespace dcpp {

SSLSocket::SSLSocket(CryptoManager::SSLContext context, bool allowUntrusted, const string& expKP) : SSLSocket(context) {
	verifyData.reset(new CryptoManager::SSLVerifyData(allowUntrusted, expKP));
}

SSLSocket::SSLSocket(CryptoManager::SSLContext context) : ctx(NULL), ssl(NULL), verifyData(nullptr) {
	ctx = CryptoManager::getInstance()->getSSLContext(context);
}

void SSLSocket::connect(const string& aIp, uint16_t aPort) {
	Socket::connect(aIp, aPort);
	
	waitConnected(0);
}

bool SSLSocket::waitConnected(uint64_t millis) {
	if(!ssl) {
		if(!Socket::waitConnected(millis)) {
			return false;
		}
		ssl.reset(SSL_new(ctx));
		if(!ssl)
			checkSSL(-1);

		if(!verifyData) {
			SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
		} else SSL_set_ex_data(ssl, CryptoManager::idxVerifyData, verifyData.get());

		checkSSL(SSL_set_fd(ssl, sock));
	}

	if(SSL_is_init_finished(ssl)) {
		return true;
	}

	while(true) {
		// OpenSSL needs server handshake for NAT traversal
		int ret = ssl->server ? SSL_accept(ssl) : SSL_connect(ssl);
		if(ret == 1) {
			dcdebug("Connected to SSL server using %s as %s\n", SSL_get_cipher(ssl), ssl->server ? "server" : "client");
			return true;
		}
		if(!waitWant(ret, millis)) {
			return false;
		}
	}
}

void SSLSocket::accept(const Socket& listeningSocket) {
	Socket::accept(listeningSocket);

	waitAccepted(0);
}

bool SSLSocket::waitAccepted(uint64_t millis) {
	if(!ssl) {
		if(!Socket::waitAccepted(millis)) {
			return false;
		}
		ssl.reset(SSL_new(ctx));
		if(!ssl)
			checkSSL(-1);

		if(!verifyData) {
			SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
		} else SSL_set_ex_data(ssl, CryptoManager::idxVerifyData, verifyData.get());

		checkSSL(SSL_set_fd(ssl, sock));
	}

	if(SSL_is_init_finished(ssl)) {
		return true;
	}

	while(true) {
		int ret = SSL_accept(ssl);
		if(ret == 1) {
			dcdebug("Connected to SSL client using %s\n", SSL_get_cipher(ssl));
			return true;
		}
		if(!waitWant(ret, millis)) {
			return false;
		}
	}
}

bool SSLSocket::waitWant(int ret, uint64_t millis) {
	int err = SSL_get_error(ssl, ret);
	switch(err) {
	case SSL_ERROR_WANT_READ:
		return wait(millis, Socket::WAIT_READ) == WAIT_READ;
	case SSL_ERROR_WANT_WRITE:
		return wait(millis, Socket::WAIT_WRITE) == WAIT_WRITE;

	// Check if this is a fatal error...
	default: checkSSL(ret);
	}
	dcdebug("SSL: Unexpected fallthrough");
	// There was no error?
	return true;
}

int SSLSocket::read(void* aBuffer, int aBufLen) {
	if(!ssl) {
		return -1;
	}
	int len = checkSSL(SSL_read(ssl, aBuffer, aBufLen));

	if(len > 0) {
		stats.totalDown += len;
		//dcdebug("In(s): %.*s\n", len, (char*)aBuffer);
	}
	return len;
}

int SSLSocket::write(const void* aBuffer, int aLen) {
	if(!ssl) {
		return -1;
	}
	int ret = checkSSL(SSL_write(ssl, aBuffer, aLen));
	if(ret > 0) {
		stats.totalUp += ret;
		//dcdebug("Out(s): %.*s\n", ret, (char*)aBuffer);
	}
	return ret;
}

int SSLSocket::checkSSL(int ret) {
	if(!ssl) {
		return -1;
	}
	if(ret <= 0) {
		int err = SSL_get_error(ssl, ret);
		switch(err) {
			case SSL_ERROR_NONE:		// Fallthrough - YaSSL doesn't for example return an openssl compatible error on recv fail
			case SSL_ERROR_WANT_READ:	// Fallthrough
			case SSL_ERROR_WANT_WRITE:
				return -1;
			case SSL_ERROR_ZERO_RETURN:				
				throw SocketException(STRING(CONNECTION_CLOSED));
			default:
				{
					ssl.reset();
					// @todo replace 80 with MAX_ERROR_SZ or whatever's appropriate for yaSSL in some nice way...
					char errbuf[80];

					/* TODO: better message for SSL_ERROR_SYSCALL
					 * If the error queue is empty (i.e. ERR_get_error() returns 0), ret can be used to find out more about the error: 
					 * If ret == 0, an EOF was observed that violates the protocol. If ret == -1, the underlying BIO reported an I/O error 
					 * (for socket I/O on Unix systems, consult errno for details).
					 */
					int error = ERR_get_error();
					sprintf(errbuf, "%s %d: %s", CSTRING(SSL_ERROR), err, (error == 0) ? CSTRING(CONNECTION_CLOSED) : ERR_reason_error_string(error));
					throw SSLSocketException(errbuf);
				}
		}
	}
	return ret;
}

int SSLSocket::wait(uint64_t millis, int waitFor) {
	if(ssl && (waitFor & Socket::WAIT_READ)) {
		/** @todo Take writing into account as well if reading is possible? */
		char c;
		if(SSL_peek(ssl, &c, 1) > 0)
			return WAIT_READ;
	}
	return Socket::wait(millis, waitFor);
}

bool SSLSocket::isTrusted() noexcept {
	if(!ssl) {
		return false;
	}

	if(SSL_get_verify_result(ssl) != X509_V_OK) {
		return false;
	}

	X509* cert = SSL_get_peer_certificate(ssl);
	if(!cert) {
		return false;
	}

	X509_free(cert);

	return true;
}

string SSLSocket::getCipherName() noexcept {
	if(!ssl)
		return Util::emptyString;
	
	return SSL_get_cipher_name(ssl);
}

ByteVector SSLSocket::getKeyprint() const noexcept {
	if(!ssl)
		return ByteVector();

	X509* x509 = SSL_get_peer_certificate(ssl);

	if(!x509)
		return ByteVector();

	ByteVector res = ssl::X509_digest(x509, EVP_sha256());

	X509_free(x509);
	return res;
}

bool SSLSocket::verifyKeyprint(const string& expKP, bool allowUntrusted) noexcept {
	if(!ssl)
		return true;

	if(expKP.empty() || expKP.find("/") == string::npos)
		return allowUntrusted; 

	verifyData.reset(new CryptoManager::SSLVerifyData(allowUntrusted, expKP));
	SSL_set_ex_data(ssl, CryptoManager::idxVerifyData, verifyData.get());

	X509_STORE* store = X509_STORE_new();

	bool result = false;
	int err = SSL_get_verify_result(ssl);
	if(store) {
		X509_STORE_CTX* vrfy_ctx = X509_STORE_CTX_new();
		X509* cert = SSL_get_peer_certificate(ssl);

		if(vrfy_ctx && cert && X509_STORE_CTX_init(vrfy_ctx, store, cert, SSL_get_peer_cert_chain(ssl))) {
			X509_STORE_CTX_set_ex_data(vrfy_ctx, SSL_get_ex_data_X509_STORE_CTX_idx(), ssl);
			X509_STORE_CTX_set_verify_cb(vrfy_ctx, SSL_get_verify_callback(ssl));

			int verify_result = 0;
			if((verify_result = X509_verify_cert(vrfy_ctx)) >= 0) {
				err = X509_STORE_CTX_get_error(vrfy_ctx);

				// Watch out for weird library errors that might not set the context error code
				if(err == X509_V_OK && verify_result == 0)
					err = X509_V_ERR_UNSPECIFIED;

				// This is for people who don't restart their clients and have low expiration time on their cert
				result = (err == X509_V_OK || err == X509_V_ERR_CERT_HAS_EXPIRED) || (allowUntrusted && err != X509_V_ERR_APPLICATION_VERIFICATION);
			}
		}

		if(cert) X509_free(cert);
		if(vrfy_ctx) X509_STORE_CTX_free(vrfy_ctx);
		if(store) X509_STORE_free(store);
	}

	// KeyPrint is a strong indicator of trust
	SSL_set_verify_result(ssl, err);

	return result;
}

void SSLSocket::shutdown() noexcept {
	if(ssl)
		SSL_shutdown(ssl);
}

void SSLSocket::close() noexcept {
	if(ssl) {
		ssl.reset();
	}
	Socket::shutdown();
	Socket::close();
}

} // namespace dcpp