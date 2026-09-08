#include "tls.h"
#include "settings.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/provider.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sd::native {
namespace {
int nibble(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
std::shared_ptr<SAPreSharedKey> key_decode(const std::string& hex) {
    if(hex.empty() || hex.size()%2 || hex.size()>512)
        throw Failure(SD_INVALID_ARGUMENT,"PSK requires 1..256 bytes encoded as an even-length hexadecimal string");
    auto key=std::make_shared<SAPreSharedKey>();key->bytes.reserve(hex.size()/2);
    for(size_t i=0;i<hex.size();i+=2) {
        const int hi=nibble(hex[i]),lo=nibble(hex[i+1]);
        if(hi<0 || lo<0) throw Failure(SD_INVALID_ARGUMENT,"PSK contains a non-hexadecimal character");
        key->bytes.push_back(uint8_t(hi*16+lo));
    }
    return key;
}
[[noreturn]] void tls_error(const char* operation) {
    const auto code=ERR_get_error();
    // No identity/key/certificate contents or configurable text enters diagnostics.
    const auto reason=code?ERR_reason_error_string(code):nullptr;
    char message[256]{};std::snprintf(message,sizeof(message),"%s failed%s%s",operation,reason?": ":"",reason?reason:"");
    ERR_clear_error();throw Failure(SD_TLS_ERROR,message);
}
void check(int result,const char* operation) { if(result!=1) tls_error(operation); }
}
SASecureSocketLayerConfig SASecureSocketLayerConfig::FromSettings(const SASettings& s) {
    SASecureSocketLayerConfig config;config.mode=uint32_t(s.Integer("security.encryption"));
    const auto& password=s.Text("security.password");const auto& key=s.Text("security.psk_key");
    const auto& identity=s.Text("security.psk_identity");
    if(!config.mode) {
        if(!password.empty() || !key.empty() || !identity.empty())
            throw Failure(SD_INVALID_ARGUMENT,"TLS credentials require security.encryption=1 or 2");
        return config;
    }
    if(config.mode>SD_TLS_1_3) throw Failure(SD_INVALID_ARGUMENT,"Unknown TLS mode");
    if(!password.empty() && !key.empty())
        throw Failure(SD_INVALID_ARGUMENT,"Choose either security.password or security.psk_key");
    if(!password.empty()) {
        if(password.size()!=8) throw Failure(SD_INVALID_ARGUMENT,"spacedesk password must contain exactly eight hexadecimal digits");
        auto decoded=key_decode(password);decoded->bytes.insert(decoded->bytes.end(),16,0x1a);config.key=std::move(decoded);
    } else config.key=key_decode(key);
    if(!identity.empty()) config.identity=identity;
    if(config.identity.size()>255) throw Failure(SD_INVALID_ARGUMENT,"TLS PSK identity exceeds 255 UTF-8 bytes");
    return config;
}

struct SASecureSocketLayer::Impl {
    SASecureSocketLayer& owner;
    OSSL_LIB_CTX* library=nullptr;
    OSSL_PROVIDER* provider=nullptr;
    SSL_CTX* context=nullptr;
    SSL* connection=nullptr;
    uint8_t record_header[5]{};
    size_t header_bytes=0,body_bytes=0;
    ULONGLONG incomplete_since=0;
    explicit Impl(SASecureSocketLayer& value):owner(value) {}
    ~Impl() {
        SSL_free(connection);SSL_CTX_free(context);OSSL_PROVIDER_unload(provider);OSSL_LIB_CTX_free(library);
    }
    static unsigned int Psk(SSL* ssl,const char*,char* identity,unsigned int identity_capacity,
                            unsigned char* key,unsigned int key_capacity) noexcept {
        auto self=static_cast<Impl*>(SSL_get_app_data(ssl));
        if(!self || !self->owner.config_.key) return 0;
        const auto& config=self->owner.config_;const auto& bytes=config.key->bytes;
        if(config.identity.size()+1>identity_capacity || bytes.size()>key_capacity) return 0;
        std::memcpy(identity,config.identity.c_str(),config.identity.size()+1);
        std::memcpy(key,bytes.data(),bytes.size());return unsigned(bytes.size());
    }
    static void Info(const SSL* ssl,int where,int value) noexcept {
        auto self=static_cast<Impl*>(SSL_get_app_data(ssl));
        if(self && (where&SSL_CB_ALERT) && (where&SSL_CB_READ)) self->owner.state_.last_alert=uint32_t(value&255);
    }
    void ObserveEncrypted(const uint8_t* bytes,size_t count) noexcept {
        // Only frame the public TLS record envelope. OpenSSL authenticates and
        // validates it. Tracking boundaries distinguishes a stalled partial
        // record from a fully processed ticket/control record on an idle link.
        while(count) {
            if(!header_bytes && !body_bytes) incomplete_since=GetTickCount64();
            if(header_bytes<5) {
                const auto n=std::min(count,5-header_bytes);
                std::memcpy(record_header+header_bytes,bytes,n);header_bytes+=n;bytes+=n;count-=n;
                if(header_bytes<5) break;
                body_bytes=size_t(record_header[3])*256+record_header[4];
            }
            const auto n=std::min(count,body_bytes);body_bytes-=n;bytes+=n;count-=n;
            if(!body_bytes) { header_bytes=0;incomplete_since=0; }
        }
    }
    static long BioCallback(BIO* bio,int operation,const char* data,size_t,int,long,int result,size_t* processed) noexcept {
        if((operation&BIO_CB_RETURN) && (operation&~BIO_CB_RETURN)==BIO_CB_READ && result>0 && processed && data) {
            auto self=reinterpret_cast<Impl*>(BIO_get_callback_arg(bio));
            if(self) self->ObserveEncrypted(reinterpret_cast<const uint8_t*>(data),*processed);
        }
        return result;
    }
    SATlsProgress Result(int result,const char* operation) {
        const auto error=SSL_get_error(connection,result);
        if(error==SSL_ERROR_WANT_READ) return SATlsProgress::Read;
        if(error==SSL_ERROR_WANT_WRITE) return SATlsProgress::Write;
        if(error==SSL_ERROR_ZERO_RETURN) {
            owner.state_.connected=0;owner.state_.peer_close_notify=1;return SATlsProgress::Closed;
        }
        if(error==SSL_ERROR_SYSCALL || (error==SSL_ERROR_SSL &&
           ERR_GET_REASON(ERR_peek_error())==SSL_R_UNEXPECTED_EOF_WHILE_READING)) {
            // A transport loss must reach the existing reconnect FSM as I/O,
            // while failed authentication/cipher negotiation remains fatal TLS.
            // This is not clean EOF: discard the interrupted stream completely.
            owner.state_.connected=0;ERR_clear_error();
            throw Failure(SD_IO_ERROR,"TLS transport connection was lost");
        }
        owner.state_.connected=0;tls_error(operation);
    }
};
SASecureSocketLayer::SASecureSocketLayer(SASecureSocketLayerConfig config):config_(std::move(config)) {
    state_.struct_size=sizeof(state_);state_.configured=config_.mode;
}
SASecureSocketLayer::~SASecureSocketLayer()=default;
void SASecureSocketLayer::Initialize(SOCKET socket) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(impl_) throw Failure(SD_INTERNAL_ERROR,"TLS connection already initialized");
    // The SDK's static OpenSSL is private. Ignore OPENSSL_CONF and application
    // provider search paths; the default provider is linked into libcrypto.
    check(OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG,nullptr),"OpenSSL initialization");
    ERR_clear_error();auto impl=std::make_unique<Impl>(*this);
    impl->library=OSSL_LIB_CTX_new();if(!impl->library) tls_error("TLS library context creation");
    impl->provider=OSSL_PROVIDER_load(impl->library,"default");if(!impl->provider) tls_error("TLS built-in provider creation");
    impl->context=SSL_CTX_new_ex(impl->library,"provider=default",TLS_client_method());
    if(!impl->context) tls_error("SASecureSocketLayerContextCreate");
    check(SSL_CTX_set_min_proto_version(impl->context,TLS1_2_VERSION),"TLS minimum version");
    check(SSL_CTX_set_max_proto_version(impl->context,config_.mode==SD_TLS_1_2?TLS1_2_VERSION:TLS1_3_VERSION),"TLS maximum version");
    if(config_.mode==SD_TLS_1_2)
        check(SSL_CTX_set_cipher_list(impl->context,"ECDHE-PSK-CHACHA20-POLY1305"),"TLS 1.2 cipher selection");
    else {
        // APK advertises 1.3 and 1.2 versions, but only cipher 0x1303. Clearing
        // all pre-1.3 ciphers preserves that offer and cannot downgrade to 1.2.
        SSL_CTX_set_cipher_list(impl->context,"");ERR_clear_error();
    }
    check(SSL_CTX_set_ciphersuites(impl->context,"TLS_CHACHA20_POLY1305_SHA256"),"TLS 1.3 cipher selection");
    const char* groups=config_.mode==SD_TLS_1_2?"X25519:X448:P-256:P-384":"X25519:X448:P-256:P-384:ffdhe2048:ffdhe3072:ffdhe4096";
    check(SSL_CTX_set1_groups_list(impl->context,groups),"TLS key exchange groups");
    SSL_CTX_set_options(impl->context,SSL_OP_NO_COMPRESSION|SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_clear_options(impl->context,SSL_OP_ALLOW_NO_DHE_KEX);
    SSL_CTX_set_session_cache_mode(impl->context,SSL_SESS_CACHE_OFF);
    SSL_CTX_set_psk_client_callback(impl->context,&Impl::Psk);
    impl->connection=SSL_new(impl->context);if(!impl->connection) tls_error("TLS connection creation");
    SSL_set_app_data(impl->connection,impl.get());SSL_set_info_callback(impl->connection,&Impl::Info);
    check(SSL_set_fd(impl->connection,int(socket)),"TLS socket association");
    auto* bio=SSL_get_rbio(impl->connection);
    BIO_set_callback_arg(bio,reinterpret_cast<char*>(impl.get()));BIO_set_callback_ex(bio,&Impl::BioCallback);
    SSL_set_connect_state(impl->connection);impl_=std::move(impl);
}
SATlsProgress SASecureSocketLayer::Connect() {
    std::lock_guard<std::mutex> lock(mutex_);ERR_clear_error();
    const int result=SSL_connect(impl_->connection);
    if(result!=1) return impl_->Result(result,"SASecureSocketLayerConnectionConnect");
    const auto* cipher=SSL_get_current_cipher(impl_->connection);
    state_.protocol_version=uint32_t(SSL_version(impl_->connection));
    state_.cipher_suite=SSL_CIPHER_get_protocol_id(cipher);
    state_.session_reused=SSL_session_reused(impl_->connection)!=0;
    std::snprintf(state_.cipher_name,sizeof(state_.cipher_name),"%s",SSL_CIPHER_get_name(cipher));
    const auto expected=config_.mode==SD_TLS_1_2?TLS1_2_VERSION:TLS1_3_VERSION;
    const auto expected_cipher=config_.mode==SD_TLS_1_2?0xccac:0x1303;
    if(state_.protocol_version!=uint32_t(expected) || state_.cipher_suite!=uint32_t(expected_cipher) ||
       !SSL_get_negotiated_group(impl_->connection)) throw Failure(SD_TLS_ERROR,"Server did not negotiate the requested PSK-DHE profile");
    state_.connected=1;return SATlsProgress::Complete;
}
SATlsIo SASecureSocketLayer::Read(uint8_t* data,size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);ERR_clear_error();size_t read=0;
    const int result=SSL_read_ex(impl_->connection,data,count,&read);
    if(result==1) return {read,SATlsProgress::Complete};
    return {0,impl_->Result(result,"TLS receive")};
}
SATlsIo SASecureSocketLayer::Write(const uint8_t* data,size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);ERR_clear_error();size_t written=0;
    const int result=SSL_write_ex(impl_->connection,data,count,&written);
    if(result==1) return {written,SATlsProgress::Complete};
    return {0,impl_->Result(result,"TLS send")};
}
SATlsProgress SASecureSocketLayer::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!impl_) return SATlsProgress::Complete;
    ERR_clear_error();const int result=SSL_shutdown(impl_->connection);
    if(result>=0) { state_.connected=0;return SATlsProgress::Complete; } // own close_notify has been sent at result0
    return impl_->Result(result,"TLS shutdown");
}
void SASecureSocketLayer::StateGet(sd_tls_state& out) const {
    std::lock_guard<std::mutex> lock(mutex_);out=state_;
}
ULONGLONG SASecureSocketLayer::IncompleteRecordSince() const {
    std::lock_guard<std::mutex> lock(mutex_);return impl_?impl_->incomplete_since:0;
}
}
