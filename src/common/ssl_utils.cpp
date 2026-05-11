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
#include <asio/ip/address.hpp>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

namespace cfrp {
namespace common {

namespace {

bool IsIpAddress(const std::string& value) {
    std::error_code ec;
    asio::ip::make_address(value, ec);
    return !ec;
}

bool IsLoopbackIp(const std::string& value) {
    std::error_code ec;
    auto address = asio::ip::make_address(value, ec);
    return !ec && address.is_loopback();
}

bool AddUniqueSan(std::vector<std::string>& sans, const std::string& san) {
    if (san.empty()) return false;
    if (std::find(sans.begin(), sans.end(), san) != sans.end()) return false;
    sans.push_back(san);
    return true;
}

} // namespace

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

bool SslUtils::IsServerCertIdentityValid(const std::string& cert_file, const std::vector<std::string>& expected_server_subject_alt_names) {
    if (!fs::exists(cert_file)) return false;

    std::vector<std::string> expected_dns_names;
    expected_dns_names.reserve(expected_server_subject_alt_names.size());
    for (const auto& san : expected_server_subject_alt_names) {
        if (san.rfind("DNS:", 0) == 0) {
            expected_dns_names.push_back(san.substr(4));
        }
    }

    if (expected_dns_names.empty()) {
        return true;
    }

    FILE* fp = fopen(cert_file.c_str(), "r");
    if (!fp) return false;

    X509* x509 = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (!x509) return false;

    char common_name[256] = {0};
    X509_NAME* subject = X509_get_subject_name(x509);
    int common_name_len = X509_NAME_get_text_by_NID(subject, NID_commonName, common_name, static_cast<int>(sizeof(common_name)));
    X509_free(x509);

    if (common_name_len <= 0) {
        return false;
    }

    const std::string cert_common_name(common_name, static_cast<size_t>(common_name_len));
    return std::find(expected_dns_names.begin(), expected_dns_names.end(), cert_common_name) != expected_dns_names.end();
}

bool SslUtils::EnsureCertificates(const CertConfig& config) {
    const bool ca_cert_exists = fs::exists(config.ca_cert_file);
    const bool server_cert_exists = fs::exists(config.server_cert_file);
    const bool ca_key_exists = fs::exists(config.ca_key_file);
    const bool server_key_exists = fs::exists(config.server_key_file);

    const bool ca_cert_valid = ca_cert_exists && IsCertValid(config.ca_cert_file);
    const bool server_cert_valid = server_cert_exists && IsCertValid(config.server_cert_file);
    const bool server_identity_valid = server_cert_valid && IsServerCertIdentityValid(config.server_cert_file, config.server_subject_alt_names);

    if (ca_cert_valid && server_cert_valid && server_identity_valid && ca_key_exists && server_key_exists) {
        return true;
    }

    std::vector<std::string> reasons;
    if (!ca_cert_exists) reasons.push_back("missing CA certificate");
    if (!server_cert_exists) reasons.push_back("missing server certificate");
    if (!ca_key_exists) reasons.push_back("missing CA key");
    if (!server_key_exists) reasons.push_back("missing server key");
    if (ca_cert_exists && !ca_cert_valid) reasons.push_back("expired or near-expiry CA certificate");
    if (server_cert_exists && !server_cert_valid) reasons.push_back("expired or near-expiry server certificate");
    if (server_cert_valid && !server_identity_valid) reasons.push_back("server certificate identity mismatch");

    std::string reason_text;
    for (size_t index = 0; index < reasons.size(); ++index) {
        if (index > 0) reason_text += ", ";
        reason_text += reasons[index];
    }

    if (reason_text.empty()) {
        reason_text = "unknown reason";
    }

    std::cout << "[Server] SSL/QUIC certificate regeneration triggered: " << reason_text << std::endl;
    if (GenerateFullChain(config)) {
        std::cout << "[Server] Generated new self-signed chain in " << fs::path(config.ca_cert_file).parent_path() << std::endl;
        std::cout << "[Server] TIP: You only need to copy '" << config.ca_cert_file << "' to your clients to enable 'verify_peer'." << std::endl;
        return true;
    }
    return false;
}

std::vector<std::string> SslUtils::DefaultServerSubjectAltNames(const std::string& bind_addr) {
    std::vector<std::string> sans;

    if (!bind_addr.empty() && bind_addr != "0.0.0.0" && bind_addr != "::") {
        if (IsIpAddress(bind_addr)) {
            AddUniqueSan(sans, "IP:" + bind_addr);
        } else {
            AddUniqueSan(sans, "DNS:" + bind_addr);
        }
    }

    AddUniqueSan(sans, "DNS:localhost");
    AddUniqueSan(sans, "DNS:cfrp");
    AddUniqueSan(sans, "IP:127.0.0.1");
    AddUniqueSan(sans, "IP:::1");

    return sans;
}

bool SslUtils::ConfigureClientTlsIdentity(WOLFSSL* ssl, const std::string& server_name, bool verify_peer) {
    if (!ssl || server_name.empty()) return false;

    if (SSL_set_tlsext_host_name(ssl, server_name.c_str()) != SSL_SUCCESS) {
        return false;
    }

    if (!verify_peer) {
        return true;
    }

    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
    if (!param) {
        return false;
    }

    if (IsLoopbackIp(server_name)) {
        return X509_VERIFY_PARAM_set1_host(param, "localhost", 0) == 1;
    }

    return X509_VERIFY_PARAM_set1_host(param, server_name.c_str(), 0) == 1;
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
    const std::string common_name = !config.server_subject_alt_names.empty() ? config.server_subject_alt_names.front().substr(4) : "localhost";
    X509_NAME_add_entry_by_txt(server_name, "CN", MBSTRING_ASC, (unsigned char*)common_name.c_str(), -1, -1, 0);
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
