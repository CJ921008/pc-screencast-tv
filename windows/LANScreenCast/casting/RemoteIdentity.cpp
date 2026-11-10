#define NOMINMAX
#include "RemoteAccess.h"
#include <array>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdexcept>
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace LANScreenCast::casting {
namespace {
void Check(bool success) {
    if (!success) throw std::runtime_error("Cannot create or read the remote access identity");
}

std::string Base64Url(std::array<unsigned char, 32> const& bytes) {
    unsigned char encoded[45]{};
    EVP_EncodeBlock(encoded, bytes.data(), static_cast<int>(bytes.size()));
    std::string result(reinterpret_cast<char*>(encoded), 43);
    for (auto& character : result) {
        if (character == '+') character = '-';
        if (character == '/') character = '_';
    }
    return result;
}

std::string Pem(BIO* bio) {
    char* data = nullptr;
    auto size = BIO_get_mem_data(bio, &data);
    Check(size > 0 && data);
    return std::string(data, size);
}
}

RemoteIdentity RemoteIdentity::Generate() {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    Check(key && certificate);
    auto cert = certificate.get();
    std::array<unsigned char, 16> serial{};
    Check(RAND_bytes(serial.data(), static_cast<int>(serial.size())) == 1);
    serial[0] = (serial[0] & 0x7f) | 1;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> serialNumber(
        BN_bin2bn(serial.data(), static_cast<int>(serial.size()), nullptr), BN_free);
    Check(serialNumber && BN_to_ASN1_INTEGER(serialNumber.get(), X509_get_serialNumber(cert)));
    Check(X509_set_version(cert, 2) == 1);
    Check(X509_gmtime_adj(X509_getm_notBefore(cert), -3600) != nullptr);
    Check(X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365 * 5) != nullptr);
    auto name = X509_get_subject_name(cert);
    Check(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<unsigned char const*>("LAN ScreenCast Remote"), -1, -1, 0) == 1);
    Check(X509_set_issuer_name(cert, name) == 1);
    Check(X509_set_pubkey(cert, key.get()) == 1);
    Check(X509_sign(cert, key.get(), EVP_sha256()) > 0);
    std::unique_ptr<BIO, decltype(&BIO_free)> certificatePem(BIO_new(BIO_s_mem()), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> keyPem(BIO_new(BIO_s_mem()), BIO_free);
    Check(certificatePem && keyPem);
    Check(PEM_write_bio_X509(certificatePem.get(), cert) == 1);
    Check(PEM_write_bio_PrivateKey(keyPem.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    std::array<unsigned char, 32> token{};
    Check(RAND_bytes(token.data(), static_cast<int>(token.size())) == 1);
    return {Pem(certificatePem.get()), Pem(keyPem.get()), Base64Url(token)};
}

std::string RemoteIdentity::PairingCode() const {
    std::unique_ptr<BIO, decltype(&BIO_free)> pem(
        BIO_new_mem_buf(certificate.data(), static_cast<int>(certificate.size())), BIO_free);
    Check(pem != nullptr);
    std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(pem.get(), nullptr, nullptr, nullptr), X509_free);
    std::array<unsigned char, 32> digest{};
    unsigned int length = 0;
    Check(cert && X509_digest(cert.get(), EVP_sha256(), digest.data(), &length) == 1 && length == digest.size());
    Check(token.size() == 43);
    return "LSC1." + Base64Url(digest) + "." + token;
}

bool RemoteIdentity::Authorizes(std::string_view raw) const {
    if (raw.size() > 512 || token.size() != 43) return false;
    try {
        auto message = nlohmann::json::parse(raw);
        if (!message.is_object() || message.size() != 3 ||
            message.at("type") != "remote_auth" || !message.at("version").is_number_integer() ||
            message.at("version") != 1) return false;
        auto const& presented = message.at("token").get_ref<std::string const&>();
        return presented.size() == token.size() && CRYPTO_memcmp(presented.data(), token.data(), token.size()) == 0;
    } catch (nlohmann::json::exception const&) {
        return false;
    }
}
}
