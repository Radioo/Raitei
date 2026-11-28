#include "loopback_signaling.h"

#include <rtc/rtc.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

using raitei::testing::eventually;

namespace {

constexpr const char* host_name = "localhost";
constexpr const char* alternative_names = "DNS:localhost";
constexpr const char* authority_constraints = "critical,CA:TRUE";
constexpr long valid_from_seconds = -60;
constexpr long valid_for_seconds = 3600;
constexpr const char* bind_address = "127.0.0.1";
constexpr std::uint16_t any_port = 0;

struct CertificateFiles {
    std::filesystem::path directory;
    std::filesystem::path certificate;
    std::filesystem::path key;

    explicit CertificateFiles(std::filesystem::path folder)
        : directory(std::move(folder)), certificate(directory / "certificate.pem"), key(directory / "key.pem") {
        std::filesystem::create_directories(directory);
    }
    CertificateFiles(const CertificateFiles&) = delete;
    CertificateFiles& operator=(const CertificateFiles&) = delete;
    ~CertificateFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

template <typename Type, void (*Free)(Type*)>
using Owned = std::unique_ptr<Type, decltype([](Type* value) { Free(value); })>;

void add_extension(X509* certificate, int nid, const char* value) {
    X509V3_CTX context{};
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    const Owned<X509_EXTENSION, X509_EXTENSION_free> extension(X509V3_EXT_conf_nid(nullptr, &context, nid, value));
    REQUIRE(extension);
    REQUIRE(X509_add_ext(certificate, extension.get(), -1) == 1);
}

void write_pem(const std::filesystem::path& path, const std::function<int(BIO*)>& write) {
    const Owned<BIO, BIO_free_all> file(BIO_new_file(path.string().c_str(), "w"));
    REQUIRE(file);
    REQUIRE(write(file.get()) == 1);
}

std::unique_ptr<CertificateFiles> self_signed_localhost() {
    auto files = std::make_unique<CertificateFiles>(std::filesystem::temp_directory_path() / std::format("raitei-tls-{:08x}", std::random_device{}()));

    const Owned<EVP_PKEY, EVP_PKEY_free> key(EVP_EC_gen("P-256"));
    REQUIRE(key);
    const Owned<X509, X509_free> certificate(X509_new());
    REQUIRE(certificate);
    REQUIRE(X509_set_version(certificate.get(), 2) == 1);
    REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    REQUIRE(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), valid_from_seconds) != nullptr);
    REQUIRE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), valid_for_seconds) != nullptr);
    REQUIRE(X509_set_pubkey(certificate.get(), key.get()) == 1);
    X509_NAME* name = X509_get_subject_name(certificate.get());
    REQUIRE(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(host_name), -1, -1, 0) == 1);
    REQUIRE(X509_set_issuer_name(certificate.get(), name) == 1);
    add_extension(certificate.get(), NID_subject_alt_name, alternative_names);
    add_extension(certificate.get(), NID_basic_constraints, authority_constraints);
    REQUIRE(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);

    write_pem(files->certificate, [&](BIO* file) { return PEM_write_bio_X509(file, certificate.get()); });
    write_pem(files->key, [&](BIO* file) { return PEM_write_bio_PrivateKey(file, key.get(), nullptr, nullptr, 0, nullptr, nullptr); });
    return files;
}

class TlsServer {
public:
    explicit TlsServer(const CertificateFiles& files)
        : server_(rtc::WebSocketServer::Configuration{
              .port = any_port,
              .enableTls = true,
              .certificatePemFile = files.certificate.string(),
              .keyPemFile = files.key.string(),
              .bindAddress = bind_address,
          }) {
        server_.onClient([this](std::shared_ptr<rtc::WebSocket> client) {
            std::scoped_lock lock(mutex_);
            clients_.push_back(std::move(client));
        });
    }

    std::string url() const {
        return std::format("wss://{}:{}/", host_name, server_.port());
    }

private:
    rtc::WebSocketServer server_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<rtc::WebSocket>> clients_;
};

struct Outcome {
    std::atomic<bool> opened = false;
    std::atomic<bool> failed = false;
};

std::shared_ptr<Outcome> connect(rtc::WebSocket& client, const std::string& url) {
    auto outcome = std::make_shared<Outcome>();
    client.onOpen([outcome] { outcome->opened = true; });
    client.onError([outcome](const std::string&) { outcome->failed = true; });
    client.onClosed([outcome] { outcome->failed = true; });
    client.open(url);
    REQUIRE(eventually([&] { return outcome->opened || outcome->failed; }));
    return outcome;
}

}

TEST_CASE("a wss connection is refused when no trusted authority signed the server's certificate") {
    const auto files = self_signed_localhost();
    TlsServer server(*files);
    rtc::WebSocket client;

    CHECK_FALSE(connect(client, server.url())->opened);
}

TEST_CASE("a wss connection opens when the server's certificate is signed by a trusted authority") {
    const auto files = self_signed_localhost();
    TlsServer server(*files);
    rtc::WebSocketConfiguration configuration;
    configuration.caCertificatePemFile = files->certificate.string();
    rtc::WebSocket client(configuration);

    CHECK(connect(client, server.url())->opened);
}
