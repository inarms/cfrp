/*
 * Copyright 2026 inarms
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
 */

#include "common/ssl_utils.h"
#include <wolfssl/options.h>
#include <wolfssl/openssl/ssl.h>
#include <wolfssl/openssl/x509.h>
#include <wolfssl/openssl/evp.h>
#include <wolfssl/openssl/rsa.h>
#include <wolfssl/openssl/pem.h>
#include <iostream>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

namespace cfrp {
namespace common {

bool SslUtils::CreateDirectoryIfNotExists(const std::string& path) {
    try {
        fs::path p(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[SslUtils] Failed to create directory: " << e.what() << std::endl;
        return false;
    }
}

bool SslUtils::IsCertValid(const std::string& cert_file) {
    if (!fs::exists(cert_file)) return false;

    FILE* fp = fopen(cert_file.c_str(), "r");
    if (!fp) return false;

    X509* x509 = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (!x509) return false;

    // Check expiration
    // wolfSSL OpenSSL compat layer provides X509_get_notAfter
    ASN1_TIME* notAfter = X509_get_notAfter(x509);
    int day, sec;
    if (ASN1_TIME_diff(&day, &sec, NULL, notAfter) <= 0) {
        // Expired or error
        X509_free(x509);
        return false;
    }

    // Check if it expires in less than 7 days
    if (day < 7) {
        X509_free(x509);
        return false;
    }

    X509_free(x509);
    return true;
}

bool SslUtils::EnsureCertificates(const CertConfig& config) {
    if (IsCertValid(config.ca_cert_file) && IsCertValid(config.server_cert_file) &&
        fs::exists(config.ca_key_file) && fs::exists(config.server_key_file)) {
        return true;
    }

    std::cout << "[Server] SSL/QUIC certificates missing or expired. Generating new self-signed chain..." << std::endl;
    if (GenerateFullChain(config)) {
        std::cout << "[Server] Generated new self-signed chain in " << fs::path(config.ca_cert_file).parent_path() << std::endl;
        std::cout << "[Server] TIP: You only need to copy '" << config.ca_cert_file << "' to your clients to enable 'verify_peer'." << std::endl;
        return true;
    }
    return false;
}

// Helper to generate a key
static EVP_PKEY* GenerateKey() {
    EVP_PKEY* pkey = EVP_PKEY_new();
    BIGNUM* bn = BN_new();
    BN_set_word(bn, RSA_F4);
    RSA* rsa = RSA_new();
    if (RSA_generate_key_ex(rsa, 2048, bn, NULL) != 1) {
        RSA_free(rsa);
        BN_free(bn);
        EVP_PKEY_free(pkey);
        return nullptr;
    }
    EVP_PKEY_assign_RSA(pkey, rsa);
    BN_free(bn);
    return pkey;
}

bool SslUtils::GenerateFullChain(const CertConfig& config) {
    CreateDirectoryIfNotExists(config.ca_cert_file);
    CreateDirectoryIfNotExists(config.server_cert_file);

    EVP_PKEY* ca_key = GenerateKey();
    EVP_PKEY* server_key = GenerateKey();

    if (!ca_key || !server_key) return false;

    // 1. Generate CA Certificate
    X509* ca_cert = X509_new();
    X509_set_version(ca_cert, 2); // X509 v3
    ASN1_INTEGER_set(X509_get_serialNumber(ca_cert), 1);
    X509_gmtime_adj(X509_get_notBefore(ca_cert), 0);
    X509_gmtime_adj(X509_get_notAfter(ca_cert), config.ca_expiry_days * 24 * 3600);
    X509_set_pubkey(ca_cert, ca_key);

    X509_NAME* ca_name = X509_get_subject_name(ca_cert);
    X509_NAME_add_entry_by_txt(ca_name, "CN", MBSTRING_ASC, (unsigned char*)"cfrp Root CA", -1, -1, 0);
    X509_set_issuer_name(ca_cert, ca_name);

    if (!X509_sign(ca_cert, ca_key, EVP_sha256())) {
        X509_free(ca_cert);
        EVP_PKEY_free(ca_key);
        EVP_PKEY_free(server_key);
        return false;
    }

    // 2. Generate Server Certificate
    X509* server_cert = X509_new();
    X509_set_version(server_cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(server_cert), 2);
    X509_gmtime_adj(X509_get_notBefore(server_cert), 0);
    X509_gmtime_adj(X509_get_notAfter(server_cert), config.server_expiry_days * 24 * 3600);
    X509_set_pubkey(server_cert, server_key);

    X509_NAME* server_name = X509_get_subject_name(server_cert);
    X509_NAME_add_entry_by_txt(server_name, "CN", MBSTRING_ASC, (unsigned char*)"cfrp Server", -1, -1, 0);
    X509_set_issuer_name(server_cert, ca_name);

    if (!X509_sign(server_cert, ca_key, EVP_sha256())) {
        X509_free(server_cert);
        X509_free(ca_cert);
        EVP_PKEY_free(ca_key);
        EVP_PKEY_free(server_key);
        return false;
    }

    // Write CA
    BIO* bio = BIO_new_file(config.ca_cert_file.c_str(), "wb");
    if (bio) {
        PEM_write_bio_X509(bio, ca_cert);
        BIO_free(bio);
    }

    bio = BIO_new_file(config.ca_key_file.c_str(), "wb");
    if (bio) {
        RSA* rsa = EVP_PKEY_get1_RSA(ca_key);
        PEM_write_bio_RSAPrivateKey(bio, rsa, NULL, NULL, 0, NULL, NULL);
        RSA_free(rsa);
        BIO_free(bio);
    }

    // Write Server
    bio = BIO_new_file(config.server_cert_file.c_str(), "wb");
    if (bio) {
        PEM_write_bio_X509(bio, server_cert);
        BIO_free(bio);
    }

    bio = BIO_new_file(config.server_key_file.c_str(), "wb");
    if (bio) {
        RSA* rsa = EVP_PKEY_get1_RSA(server_key);
        PEM_write_bio_RSAPrivateKey(bio, rsa, NULL, NULL, 0, NULL, NULL);
        RSA_free(rsa);
        BIO_free(bio);
    }

    X509_free(server_cert);
    X509_free(ca_cert);
    EVP_PKEY_free(ca_key);
    EVP_PKEY_free(server_key);

    return true;
}

} // namespace common
} // namespace cfrp
