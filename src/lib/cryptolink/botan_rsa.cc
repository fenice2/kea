// Copyright (C) 2014, 2015  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <cryptolink.h>
#include <cryptolink/crypto_asym.h>

#include <boost/scoped_ptr.hpp>

#include <botan/version.h>
#include <botan/botan.h>
#include <botan/hash.h>
#include <botan/oids.h>
#include <botan/pubkey.h>
#include <botan/rsa.h>
#include <botan/x509stor.h>

#include <util/encode/base64.h>
#include <cryptolink/botan_common.h>
#include <cryptolink/botan_rsa.h>

#include <cstring>
#include <fstream>

namespace isc {
namespace cryptolink {

/// @brief Constructor from a key, asym and hash algorithm,
///        key kind and key binary format
RsaAsymImpl::RsaAsymImpl(const void* key, size_t key_len,
                         const HashAlgorithm hash_algorithm,
                         const AsymKeyKind key_kind,
                         const AsymFormat key_format) {
    algo_ = RSA_;
    hash_ = hash_algorithm;
    kind_ = key_kind;
    std::string hash = btn::getHashAlgorithmName(hash_);
    if (hash.compare("Unknown") == 0) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown hash algorithm: " << static_cast<int>(hash_));
    }
    std::string emsa = "EMSA3(" + hash + ")";
    if (key_len == 0) {
        isc_throw(BadKey, "Bad RSA " <<
                  (kind_ != CERT ? "key" : "cert") << " length: 0");
    }

    if ((kind_ == PRIVATE) && (key_format == BASIC)) {
        // PKCS#1 Private Key
        const Botan::byte* keyin = reinterpret_cast<const Botan::byte*>(key);
        const Botan::MemoryVector<Botan::byte> ber(keyin, key_len);
        // rsaEncription OID and NULL parameters
        const Botan::AlgorithmIdentifier alg_id =
            Botan::AlgorithmIdentifier("1.2.840.113549.1.1.1",
                Botan::AlgorithmIdentifier::USE_NULL_PARAM);
        Botan::AutoSeeded_RNG rng;
        try {
            priv_.reset(new Botan::RSA_PrivateKey(alg_id, ber, rng));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "RSA_PrivateKey: " << exc.what());
        }
    } else if (kind_ == PRIVATE) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Private Key format: " <<
                  static_cast<int>(key_format));
    } else if ((kind_ == PUBLIC) && (key_format == BASIC)) {
        // PKCS#1 Public Key
        const Botan::byte* keyin = reinterpret_cast<const Botan::byte*>(key);
        const Botan::MemoryVector<Botan::byte> ber(keyin, key_len);
        // rsaEncription OID and NULL parameters
        const Botan::AlgorithmIdentifier alg_id =
            Botan::AlgorithmIdentifier("1.2.840.113549.1.1.1",
                Botan::AlgorithmIdentifier::USE_NULL_PARAM);
        try {
            pub_.reset(new Botan::RSA_PublicKey(alg_id, ber));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "RSA_PublicKey: " << exc.what());
        }
    } else if ((kind_ == PUBLIC) && (key_format == ASN1)) {
        // SubjectPublicKeyInfo
        const Botan::byte* keyin = reinterpret_cast<const Botan::byte*>(key);
        Botan::DataSource_Memory source(keyin, key_len);
        Botan::Public_Key* key;
        try {
            key = Botan::X509::load_key(source);
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "X509::load_key: " << exc.what());
        }
        if (key->algo_name().compare("RSA") != 0) {
            delete key;
            isc_throw(BadKey, "not a RSA Public Key");
        }
        pub_.reset(dynamic_cast<Botan::RSA_PublicKey*>(key));
        if (!pub_) {
            delete key;
            isc_throw(LibraryError, "dynamic_cast");
        }
    } else if ((kind_ == PUBLIC) && (key_format == DNS)) {
        // RFC 3110 DNS wire format
        // key_len == 0 was already checked
        const uint8_t* p = reinterpret_cast<const uint8_t*>(key);
        size_t e_bytes = *p++;
        --key_len;
        if (e_bytes == 0) {
            if (key_len < 2) {
                isc_throw(BadKey, "Bad RSA Public Key: short exponent length");
            }
            e_bytes = (*p++) << 8;
            e_bytes += *p++;
            key_len -= 2;
        }
        if (key_len < e_bytes) {
            isc_throw(BadKey, "Bad RSA Public Key: short exponent");
        }
        if ((key_len - e_bytes) < 64) {
            isc_throw(BadKey, "Bad RSA Public Key: too short: " <<
                      (key_len - e_bytes) * 8);
        }
        if ((key_len - e_bytes) > 512) {
            isc_throw(BadKey, "Bad RSA Public Key: too large: " <<
                      (key_len - e_bytes) * 8);
        }
        Botan::BigInt e(p, e_bytes);
        p += e_bytes;
        key_len -= e_bytes;
        Botan::BigInt n(p, key_len);
        try {
            pub_.reset(new Botan::RSA_PublicKey(n, e));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "RSA_PublicKey: " << exc.what());
        }
    } else if (kind_ == PUBLIC) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Public Key format: " <<
                  static_cast<int>(key_format));
    } else if ((kind_ == CERT) && (key_format == ASN1)) {
        // X.509 Public Key Certificate
        const Botan::byte* keyin = reinterpret_cast<const Botan::byte*>(key);
        Botan::DataSource_Memory source(keyin, key_len);
        try {
            x509_.reset(new Botan::X509_Certificate(source));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "X509_Certificate: " << exc.what());
        }
        const Botan::AlgorithmIdentifier
            sig_algo(x509_->signature_algorithm());
        if (hash_ == MD5) {
            const Botan::AlgorithmIdentifier
                rsa_md5("1.2.840.113549.1.1.4",
                        Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_md5) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA MD5 certificate");
            }
        } else if (hash_ == SHA1) {
            const Botan::AlgorithmIdentifier
                rsa_sha1("1.2.840.113549.1.1.5",
                         Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha1) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA1 certificate");
            }
        } else if (hash_ == SHA224) {
            const Botan::AlgorithmIdentifier
                rsa_sha224("1.2.840.113549.1.1.14",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha224) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA224 certificate");
            }
        } else if (hash_ == SHA256) {
            const Botan::AlgorithmIdentifier
                rsa_sha256("1.2.840.113549.1.1.11",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha256) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA256 certificate");
            }
        } else if (hash_ == SHA384) {
            const Botan::AlgorithmIdentifier
                rsa_sha384("1.2.840.113549.1.1.12",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha384) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA384 certificate");
            }
        } else if (hash_ == SHA512) {
            const Botan::AlgorithmIdentifier
                rsa_sha512("1.2.840.113549.1.1.13",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha512) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA512 certificate");
            }
        } else {
            x509_.reset();
            isc_throw(UnsupportedAlgorithm,
                      "Bad hash algorithm for certificate: " <<
                      static_cast<int>(hash_));
        }
        Botan::Public_Key* key = x509_->subject_public_key();
        if (key->algo_name().compare("RSA") != 0) {
            delete key;
            x509_.reset();
            isc_throw(BadKey, "not a RSA Certificate");
        }
        pub_.reset(dynamic_cast<Botan::RSA_PublicKey*>(key));
        if (!pub_) {
            delete key;
            x509_.reset();
            isc_throw(LibraryError, "dynamic_cast");
        }
    } else if (kind_ == CERT) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown Certificate format: " <<
                  static_cast<int>(key_format));
    } else {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Key kind: " << static_cast<int>(kind_));
    }

    if (kind_ == PRIVATE) {
        try {
            if (!pub_) {
                pub_.reset(new Botan::RSA_PublicKey(priv_->get_n(),
                                                    priv_->get_e()));
            }
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "priv to pub: " << exc.what());
        }
        try {
            signer_.reset(new Botan::PK_Signer(*priv_, emsa));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PK_Signer: " << exc.what());
        }
    } else {
        try {
            verifier_.reset(new Botan::PK_Verifier(*pub_, emsa));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PK_Verifier: " << exc.what());
        }
    }
}

/// @brief Constructor from a key file with password,
///        asym and hash algorithm, key kind and key binary format
RsaAsymImpl::RsaAsymImpl(const std::string& filename,
                         const std::string& password,
                         const HashAlgorithm hash_algorithm,
                         const AsymKeyKind key_kind,
                         const AsymFormat key_format) {
    algo_ = RSA_;
    hash_ = hash_algorithm;
    kind_ = key_kind;
    std::string hash = btn::getHashAlgorithmName(hash_);
    if (hash.compare("Unknown") == 0) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown hash algorithm: " << static_cast<int>(hash_));
    }
    std::string emsa = "EMSA3(" + hash + ")";

    if ((kind_ == PRIVATE) && (key_format == ASN1)) {
        // PKCS#8 Private Key PEM file
        Botan::Private_Key* key;
        Botan::AutoSeeded_RNG rng;
        try {
            key = Botan::PKCS8::load_key(filename, rng, password);
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PKCS8::load_key: " << exc.what());
        }
        if (key->algo_name().compare("RSA") != 0) {
            delete key;
            isc_throw(BadKey, "not a RSA Private Key");
        }
        priv_.reset(dynamic_cast<Botan::RSA_PrivateKey*>(key));
        if (!priv_) {
            delete key;
            isc_throw(LibraryError, "dynamic_cast");
        }
    } else if ((kind_ == PRIVATE) && (key_format == DNS)) {
        // bind9 .private file
        // warn when password not empty
        if ((hash_ != MD5) && (hash_ != SHA1) &&
            (hash_ != SHA256) && (hash_ != SHA512)) {
            isc_throw(UnsupportedAlgorithm,
                      "Not compatible hash algorithm: " <<
                      static_cast<int>(hash_));
        }
        std::ifstream fp(filename.c_str(), std::ios::in);
        if (!fp.is_open()) {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
        bool got_algorithm = false;
        bool got_modulus = false;
        bool got_pub_exponent = false;
        bool got_priv_exponent = false;
        bool got_prime1 = false;
        bool got_prime2 = false;
        Botan::BigInt n;
        Botan::BigInt e;
        Botan::BigInt d;
        Botan::BigInt p;
        Botan::BigInt q;
        while (fp.good()) {
            std::string line;
            getline(fp, line);
            if (line.find("Algorithm:") == 0) {
                if (got_algorithm) {
                    fp.close();
                    isc_throw(BadKey, "Two Algorithm entries");
                }
                got_algorithm = true;
                std::string value = line.substr(strlen("Algorithm:") + 1,
                                                std::string::npos);
                int alg = std::atoi(value.c_str());
                if (alg == 1) {
                    // RSAMD5
                    if (hash_ != MD5) {
                        fp.close();
                        isc_throw(BadKey, "Require a RSA MD5 key");
                    }
                } else if ((alg == 5) || (alg == 7)) {
                    // RSASHA1 or RSASHA1-NSEC3-SHA1
                    if (hash_ != SHA1) {
                        fp.close();
                        isc_throw(BadKey, "Require a RSA SHA1 key");
                    }
                } else if (alg == 8) {
                    // RSASHA256
                    if (hash_ != SHA256) {
                        fp.close();
                        isc_throw(BadKey, "Require a RSA SHA256 key");
                    }
                } else if (alg == 10) {
                    // RSASHA512
                    if (hash_ != SHA512) {
                        fp.close();
                        isc_throw(BadKey, "Require a RSA SHA512 key");
                    }
                } else {
                    fp.close();
                    isc_throw(BadKey, "Bad Algorithm: " << alg);
                }
            } else if (line.find("Modulus:") == 0) {
                if (got_modulus) {
                    fp.close();
                    isc_throw(BadKey, "Two Modulus entries");
                }
                got_modulus = true;
                std::string value = line.substr(strlen("Modulus:") + 1,
                                                std::string::npos);
                std::vector<uint8_t> bin;
                try {
                    isc::util::encode::decodeBase64(value, bin);
                } catch (const BadValue& exc) {
                    fp.close();
                    isc_throw(BadKey, "Modulus: " << exc.what());
                }
                n = Botan::BigInt(&bin[0], bin.size());
          } else if (line.find("PublicExponent:") == 0) {
                if (got_pub_exponent) {
                    fp.close();
                    isc_throw(BadKey, "Two PublicExponent entries");
                }
                got_pub_exponent = true;
                std::string value =
                    line.substr(strlen("PublicExponent:") + 1,
                                std::string::npos);
                std::vector<uint8_t> bin;
                try {
                    isc::util::encode::decodeBase64(value, bin);
                } catch (const BadValue& exc) {
                    fp.close();
                    isc_throw(BadKey, "PublicExponent: " << exc.what());
                }
                e = Botan::BigInt(&bin[0], bin.size());
            } else if (line.find("PrivateExponent:") == 0) {
                if (got_priv_exponent) {
                    fp.close();
                    isc_throw(BadKey, "Two PrivateExponent entries");
                }
                got_priv_exponent = true;
                std::string value =
                    line.substr(strlen("PrivateExponent:") + 1,
                                std::string::npos);
                std::vector<uint8_t> bin;
                try {
                    isc::util::encode::decodeBase64(value, bin);
                } catch (const BadValue& exc) {
                    fp.close();
                    isc_throw(BadKey, "PrivateExponent: " << exc.what());
                }
                d = Botan::BigInt(&bin[0], bin.size());
            } else if (line.find("Prime1:") == 0) {
                if (got_prime1) {
                    fp.close();
                    isc_throw(BadKey, "Two Prime1 entries");
                }
                got_prime1 = true;
                std::string value = line.substr(strlen("Prime1:") + 1,
                                                std::string::npos);
                std::vector<uint8_t> bin;
                try {
                    isc::util::encode::decodeBase64(value, bin);
                } catch (const BadValue& exc) {
                    fp.close();
                    isc_throw(BadKey, "Prime1: " << exc.what());
                }
                p = Botan::BigInt(&bin[0], bin.size());
            } else if (line.find("Prime2:") == 0) {
                if (got_prime2) {
                    fp.close();
                    isc_throw(BadKey, "Two Prime2 entries");
                }
                got_prime2 = true;
                std::string value = line.substr(strlen("Prime2:") + 1,
                                                std::string::npos);
                std::vector<uint8_t> bin;
                try {
                    isc::util::encode::decodeBase64(value, bin);
                } catch (const BadValue& exc) {
                    fp.close();
                    isc_throw(BadKey, "Prime2: " << exc.what());
                }
                q = Botan::BigInt(&bin[0], bin.size());
            }
        }
        fp.close();
        if (!got_algorithm) {
            isc_throw(BadKey, "Missing Algorithm entry");
        }
        if (!got_modulus) {
            isc_throw(BadKey, "Missing Modulus entry");
        }
        if (!got_pub_exponent) {
            isc_throw(BadKey, "Missing PublicExponent entry");
        }
        if (!got_priv_exponent) {
            isc_throw(BadKey, "Missing PrivateExponent entry");
        }
        if (!got_prime1) {
            isc_throw(BadKey, "Missing Prime1 entry");
        }
        if (!got_prime2) {
            isc_throw(BadKey, "Missing Prime2 entry");
        }
        Botan::AutoSeeded_RNG rng;
        try {
            priv_.reset(new Botan::RSA_PrivateKey(rng, p, q, e, d, n));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "RSA_PrivateKey" << exc.what());
        }
    } else if (kind_ == PRIVATE) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Private Key format: " <<
                  static_cast<int>(key_format));
    } else if ((kind_ == PUBLIC) && (key_format == ASN1)) {
        // SubjectPublicKeyInfo PEM file
        // warn when password not empty
        Botan::Public_Key* key;
        try {
            key = Botan::X509::load_key(filename);
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "X509::load_key: " << exc.what());
        }
        if (key->algo_name().compare("RSA") != 0) {
            delete key;
            isc_throw(BadKey, "not a RSA Public Key");
        }
        pub_.reset(dynamic_cast<Botan::RSA_PublicKey*>(key));
        if (!pub_) {
            delete key;
            isc_throw(LibraryError, "dynamic_cast");
        }
    } else if ((kind_ == PUBLIC) && (key_format == DNS)) {
        // bind9 .key file (RDATA)
        // warn when password not empty
        if ((hash_ != MD5) && (hash_ != SHA1) &&
            (hash_ != SHA256) && (hash_ != SHA512)) {
            isc_throw(UnsupportedAlgorithm,
                      "Not compatible hash algorithm: " <<
                      static_cast<int>(hash_));
        }
        std::ifstream fp(filename.c_str(), std::ios::in);
        if (!fp.is_open()) {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
        std::string line;
        bool found = false;
        while (fp.good()) {
            getline(fp, line);
            if (line.empty() || (line[0] == ';')) {
                continue;
            }
            if (line.find("DNSKEY") == std::string::npos) {
                continue;
            }
            found = true;
            if (line[line.size() - 1] == '\n') {
                line.erase(line.size() - 1);
            }
            break;
        }
        fp.close();
        if (!found) {
            isc_throw(BadKey, "Can't find a DNSKEY");
        }
        const std::string b64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"
            "ghijklmnopqrstuvwxyz0123456789+/=";
        const std::string value = line.substr(line.find_last_not_of(b64));
        std::vector<uint8_t> bin;
        try {
            util::encode::decodeBase64(value, bin);
        } catch (const BadValue& exc) {
           isc_throw(BadKey, "Can't decode base64: " << exc.what());
        }
        size_t key_len = bin.size();
        const uint8_t* p = &bin[0];
        size_t e_bytes = *p++;
        --key_len;
        if (e_bytes == 0) {
            if (key_len < 2) {
                isc_throw(BadKey,
                          "Bad RSA Public Key: short exponent length");
            }
            e_bytes = (*p++) << 8;
            e_bytes += *p++;
            key_len -= 2;
        }
        if (key_len < e_bytes) {
            isc_throw(BadKey, "Bad RSA Public Key: short exponent");
        }
        if ((key_len - e_bytes) < 64) {
            isc_throw(BadKey, "Bad RSA Public Key: too short: " <<
                      (key_len - e_bytes) * 8);
        }
        if ((key_len - e_bytes) > 512) {
            isc_throw(BadKey, "Bad RSA Public Key: too large: " <<
                      (key_len - e_bytes) * 8);
        }
        Botan::BigInt e(p, e_bytes);
        p += e_bytes;
        key_len -= e_bytes;
        Botan::BigInt n(p, key_len);
        try {
            pub_.reset(new Botan::RSA_PublicKey(n, e));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "RSA_PublicKey: " << exc.what());
        }
    } else if (kind_ == PUBLIC) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Public Key format: " <<
                  static_cast<int>(key_format));
    } else if ((kind_ == CERT) && (key_format == ASN1)) {
        // X.509 Public Key Certificate PEM file
        // warn when password not empty
        try {
            x509_.reset(new Botan::X509_Certificate(filename));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "X509_Certificate: " << exc.what());
        }
        const Botan::AlgorithmIdentifier
            sig_algo(x509_->signature_algorithm());
        if (hash_ == MD5) {
            const Botan::AlgorithmIdentifier
                rsa_md5("1.2.840.113549.1.1.4",
                        Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_md5) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA MD5 certificate");
            }
        } else if (hash_ == SHA1) {
            const Botan::AlgorithmIdentifier
                rsa_sha1("1.2.840.113549.1.1.5",
                         Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha1) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA1 certificate");
            }
        } else if (hash_ == SHA224) {
            const Botan::AlgorithmIdentifier
                rsa_sha224("1.2.840.113549.1.1.14",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha224) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA224 certificate");
            }
        } else if (hash_ == SHA256) {
            const Botan::AlgorithmIdentifier
                rsa_sha256("1.2.840.113549.1.1.11",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha256) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA256 certificate");
            }
        } else if (hash_ == SHA384) {
            const Botan::AlgorithmIdentifier
                rsa_sha384("1.2.840.113549.1.1.12",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha384) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA384 certificate");
            }
        } else if (hash_ == SHA512) {
            const Botan::AlgorithmIdentifier
                rsa_sha512("1.2.840.113549.1.1.13",
                           Botan::AlgorithmIdentifier::USE_NULL_PARAM);
            if (sig_algo != rsa_sha512) {
                x509_.reset();
                isc_throw(BadKey, "Require a RSA SHA512 certificate");
            }
        } else {
            x509_.reset();
            isc_throw(UnsupportedAlgorithm,
                      "Bad hash algorithm for certificate: " <<
                      static_cast<int>(hash_));
        }
        Botan::Public_Key* key;
        try {
            key = x509_->subject_public_key();
        } catch (const std::exception& exc) {
            x509_.reset();
            isc_throw(BadKey, "subject_public_key: " << exc.what());
        }
        if (key->algo_name().compare("RSA") != 0) {
            delete key;
            x509_.reset();
            isc_throw(BadKey, "not a RSA Public Key");
        }
        pub_.reset(dynamic_cast<Botan::RSA_PublicKey*>(key));
        if (!pub_) {
            delete key;
            x509_.reset();
            isc_throw(LibraryError, "dynamic_cast");
        }
    } else if (kind_ == CERT) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown Public Key Certificate format: " <<
                  static_cast<int>(key_format));
    } else {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Key kind: " << static_cast<int>(kind_));
    }

    if (kind_ == PRIVATE) {
        try {
            if (!pub_) {
                pub_.reset(new Botan::RSA_PublicKey(priv_->get_n(),
                                                    priv_->get_e()));
            }
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "priv to pub: " << exc.what());
        }
        try {
            signer_.reset(new Botan::PK_Signer(*priv_, emsa));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PK_Signer: " << exc.what());
        }
    } else {
        try {
            verifier_.reset(new Botan::PK_Verifier(*pub_, emsa));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PK_Verifier: " << exc.what());
        }
    }
}

/// @brief Destructor
RsaAsymImpl::~RsaAsymImpl() { }

/// @brief Returns the AsymAlgorithm of the object
AsymAlgorithm RsaAsymImpl::getAsymAlgorithm() const {
    return (algo_);
}

/// @brief Returns the HashAlgorithm of the object
HashAlgorithm RsaAsymImpl::getHashAlgorithm() const {
    return (hash_);
}

/// @brief Returns the AsymKeyKind of the object
AsymKeyKind RsaAsymImpl::getAsymKeyKind() const {
    return (kind_);
}

/// @brief Returns the key size in bits
size_t RsaAsymImpl::getKeySize() const {
    if (kind_ == PRIVATE) {
        return (priv_->get_n().bits());
    } else {
        return (pub_->get_n().bits());
    }
}

/// @brief Returns the output size of the signature
size_t RsaAsymImpl::getSignatureLength(const AsymFormat sig_format) const {
    switch (sig_format) {
    case BASIC:
    case ASN1:
    case DNS:
        // In all cases a big integer of the size of n
        if (kind_ == PRIVATE) {
            return (priv_->get_n().bytes());
        } else {
            return (pub_->get_n().bytes());
        }
    default:
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Signature format: " <<
                  static_cast<int>(sig_format));
    }
}           

/// @brief Add data to digest
void RsaAsymImpl::update(const void* data, const size_t len) {
    try {
        if (kind_ == PRIVATE) {
            signer_->update(reinterpret_cast<const Botan::byte*>(data), len);
        } else {
            verifier_->update(reinterpret_cast<const Botan::byte*>(data), len);
        }            
    } catch (const std::exception& exc) {
        isc_throw(LibraryError, "update: " << exc.what());
    }
}

/// @brief Calculate the final signature
void RsaAsymImpl::sign(isc::util::OutputBuffer& result, size_t len,
          const AsymFormat) {
    try {
        Botan::SecureVector<Botan::byte> b_result;
        Botan::AutoSeeded_RNG rng;
        b_result = signer_->signature(rng);
        if (len > b_result.size()) {
            len = b_result.size();
        }
        result.writeData(b_result.begin(), len);
    } catch (const std::exception& exc) {
        isc_throw(LibraryError, "signature: " << exc.what());
    }
}

/// @brief Calculate the final signature
void RsaAsymImpl::sign(void* result, size_t len, const AsymFormat sig_format) {
    try {
        Botan::SecureVector<Botan::byte> b_result;
        Botan::AutoSeeded_RNG rng;
        b_result = signer_->signature(rng);
        size_t output_size = getSignatureLength(sig_format);
        if (output_size > len) {
            output_size = len;
        }
        std::memcpy(result, b_result.begin(), output_size);
    } catch (const std::exception& exc) {
        isc_throw(LibraryError, "signature: " << exc.what());
    }
}

/// @brief Calculate the final signature
std::vector<uint8_t> RsaAsymImpl::sign(size_t len, const AsymFormat) {
    try {
        Botan::SecureVector<Botan::byte> b_result;
        Botan::AutoSeeded_RNG rng;
        b_result = signer_->signature(rng);
        if (len > b_result.size()) {
            return (std::vector<uint8_t>(b_result.begin(), b_result.end()));
        } else {
            return (std::vector<uint8_t>(b_result.begin(), &b_result[len]));
        }
    } catch (const std::exception& exc) {
        isc_throw(LibraryError, "signature: " << exc.what());
    }
}

/// @brief Verify an existing signature
bool RsaAsymImpl::verify(const void* sig, size_t len,
                         const AsymFormat sig_format) {
    size_t size = getSignatureLength(sig_format);
    if (len != size) {
        return false;
    }
    const Botan::byte* sigbuf = reinterpret_cast<const Botan::byte*>(sig);
    try {
        return verifier_->check_signature(sigbuf, len);
    } catch (const std::exception& exc) {
        isc_throw(LibraryError, "check_signature: " << exc.what());
    }
}

/// \brief Clear the crypto state and go back to the initial state
void RsaAsymImpl::clear() {
    std::string hash = btn::getHashAlgorithmName(hash_);
    if (hash.compare("Unknown") == 0) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown hash algorithm: " << static_cast<int>(hash_));
    }
    std::string emsa = "EMSA3(" + hash + ")";
    if (kind_ == PRIVATE) {
        try {
            signer_.reset(new Botan::PK_Signer(*priv_, emsa));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PK_Signer: " << exc.what());
        }
    } else {
        try {
            verifier_.reset(new Botan::PK_Verifier(*pub_, emsa));
        } catch (const std::exception& exc) {
            isc_throw(BadKey, "PK_Verifier: " << exc.what());
        }
    }
}

/// @brief Export the key value (binary)
std::vector<uint8_t>
    RsaAsymImpl::exportkey(const AsymKeyKind key_kind,
                           const AsymFormat key_format) const {
    if ((key_kind == PRIVATE) && (key_format == BASIC)) {
        // PKCS#1 Private Key
        if (kind_ != PRIVATE) {
            isc_throw(UnsupportedAlgorithm, "Have no RSA Private Key");
        }
        Botan::MemoryVector<Botan::byte> der;
        try {
            der = priv_->pkcs8_private_key();
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "pkcs8_private_key: " << exc.what());
        }
        return std::vector<uint8_t>(der.begin(), der.end());
    } else if (key_kind == PRIVATE) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Private Key format: " <<
                  static_cast<int>(key_format));
    } else if ((key_kind == PUBLIC) && (key_format == BASIC)) {
        // PKCS#1 Public Key
        Botan::MemoryVector<Botan::byte> der;
        try {
            der = pub_->x509_subject_public_key();
        } catch (const std::exception& exc) {
            isc_throw(LibraryError,
                      "x509_subject_public_key: " << exc.what());
        }
        return std::vector<uint8_t>(der.begin(), der.end());
    } else if ((key_kind == PUBLIC) && (key_format == ASN1)) {
        // SubjectPublicKeyInfo
        Botan::MemoryVector<Botan::byte> ber;
        try {
            ber = Botan::X509::BER_encode(*pub_);
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "X509::BER_encode: " << exc.what());
        }
        return std::vector<uint8_t>(ber.begin(), ber.end());
    } else if ((key_kind == PUBLIC) && (key_format == DNS)) {
        // RFC 3110 DNS wire format
        size_t e_bytes = pub_->get_e().bytes();
        size_t mod_bytes = pub_->get_n().bytes();
        size_t x_bytes = 1;
        if (e_bytes >= 256) {
            x_bytes += 2;
        }
        std::vector<uint8_t> rdata(x_bytes + e_bytes + mod_bytes);
        if (e_bytes < 256) {
            rdata[0] = e_bytes;
        } else {
            rdata[0] = 0;
            rdata[1] = (e_bytes >> 8) & 0xff;
            rdata[2] = e_bytes & 0xff;
        }
        pub_->get_e().binary_encode(&rdata[x_bytes]);
        pub_->get_n().binary_encode(&rdata[x_bytes + e_bytes]);
        return rdata;
    } else if (key_kind == PUBLIC) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Public Key format: " <<
                  static_cast<int>(key_format));
    } else if ((key_kind == CERT) && (key_format == ASN1)) {
        // X.509 Public Key Certificate
        if (kind_ != CERT) {
            isc_throw(UnsupportedAlgorithm, "Have no Certificate");
        }
        Botan::MemoryVector<Botan::byte> ber;
        try {
            ber = x509_->BER_encode();
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "BER_encode" << exc.what());
        }
        return std::vector<uint8_t>(ber.begin(), ber.end());
    } else if (key_kind == CERT) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown Certificate format: " <<
                  static_cast<int>(key_format));
    } else {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Key kind: " << static_cast<int>(key_kind));
    }
}

/// @brief Export the key value (file)
void RsaAsymImpl::exportkey(const std::string& filename,
                            const std::string& password,
                            const AsymKeyKind key_kind,
                            const AsymFormat key_format) const {
    if ((key_kind == PRIVATE) && (key_format == ASN1)) {
        // PKCS#8 Private Key PEM file
        std::string pem;
        Botan::AutoSeeded_RNG rng;
        try {
            pem = Botan::PKCS8::PEM_encode(*priv_, rng,
                                           password, "AES-128/CBC");
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "PKCS8::PEM_encode: " << exc.what());
        }
        std::ofstream fp(filename.c_str(),
                         std::ofstream::out | std::ofstream::trunc);
        if (fp.is_open()) {
            fp.write(pem.c_str(), pem.size());
            fp.close();
        } else {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
    } else if ((key_kind == PRIVATE) && (key_format == DNS)) {
        //  bind9 .private file
        if (kind_ != PRIVATE) {
            isc_throw(UnsupportedAlgorithm, "Have no RSA Private Key");
        }
        if ((hash_ != MD5) && (hash_ != SHA1) &&
            (hash_ != SHA256) && (hash_ != SHA512)) {
            isc_throw(UnsupportedAlgorithm,
                      "Not compatible hash algorithm: " <<
                      static_cast<int>(hash_));
        }
        std::ofstream fp(filename.c_str(),
                         std::ofstream::out | std::ofstream::trunc);
        if (!fp.is_open()) {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
        fp << "Private-key-format: v1.2\n";
        if (hash_ == MD5) {
            fp << "Algorithm: 1 (RSA)\n";
        } else if (hash_ == SHA1) {
            fp << "Algorithm: 5 (RSASHA1)\n";
        } else if (hash_ == SHA256) {
            fp << "Algorithm: 8 (RSASHA256)\n";
        } else if (hash_ == SHA512) {
            fp << "Algorithm: 10 (RSASHA512)\n";
        }
        std::vector<uint8_t> bin;
        bin.resize(priv_->get_n().bytes());
        priv_->get_n().binary_encode(&bin[0]);
        fp << "Modulus: " << util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_e().bytes());
        priv_->get_e().binary_encode(&bin[0]);
        fp << "PublicExponent: " <<
            util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_d().bytes());
        priv_->get_d().binary_encode(&bin[0]);
        fp << "PrivateExponent: " <<
            util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_p().bytes());
        priv_->get_p().binary_encode(&bin[0]);
        fp << "Prime1: " << util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_q().bytes());
        priv_->get_q().binary_encode(&bin[0]);
        fp << "Prime2: " << util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_d1().bytes());
        priv_->get_d1().binary_encode(&bin[0]);
        fp << "Exponent1: " << util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_d2().bytes());
        priv_->get_d2().binary_encode(&bin[0]);
        fp << "Exponent2: " << util::encode::encodeBase64(bin) << '\n';
        bin.resize(priv_->get_c().bytes());
        priv_->get_c().binary_encode(&bin[0]);
        fp << "Coefficient: " << util::encode::encodeBase64(bin) << '\n';
        fp.close();
    } else if (key_kind == PRIVATE) {
        if (kind_ != PRIVATE) {
            isc_throw(UnsupportedAlgorithm, "Have no RSA Private Key");
        }
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Private Key format: " <<
                  static_cast<int>(key_format));
    } else if ((key_kind == PUBLIC) && (key_format == ASN1)) {
        // SubjectPublicKeyInfo PEM file
        // warn when password not empty
        std::string pem;
        try {
            pem = Botan::X509::PEM_encode(*pub_);
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "X509::PEM_encode: " << exc.what());
        }
        std::ofstream fp(filename.c_str(),
                         std::ofstream::out | std::ofstream::trunc);
        if (fp.is_open()) {
            fp.write(pem.c_str(), pem.size());
            fp.close();
        } else {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
    }  else if ((key_kind == PUBLIC) && (key_format == DNS)) {
        // bind9 .key file (RDATA)
        // warn when password not empty
        std::vector<uint8_t> bin = exportkey(key_kind, key_format);
        std::ofstream fp(filename.c_str(),
                         std::ofstream::out | std::ofstream::trunc);
        if (!fp.is_open()) {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
        fp << "; DNSKEY RDATA: " << util::encode::encodeBase64(bin) << '\n';
        fp.close();
    } else if (key_kind == PUBLIC) {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Public Key format: " <<
                  static_cast<int>(key_format));
    } else if ((key_kind == CERT) && (key_format == ASN1)) {
        // Public Key Certificate PEM file
        // warn when password not empty
        if (!x509_) {
            isc_throw(UnsupportedAlgorithm, "Have no Certificate");
        }
        std::string pem;
        try {
            pem = x509_->PEM_encode();
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "PEM_encode: " << exc.what());
        }
        std::ofstream fp(filename.c_str(),
                         std::ofstream::out | std::ofstream::trunc);
        if (fp.is_open()) {
            fp.write(pem.c_str(), pem.size());
            fp.close();
        } else {
            isc_throw(BadKey, "Can't open file: " << filename);
        }
    } else if (key_kind == CERT) {
        if (!x509_) {
            isc_throw(UnsupportedAlgorithm, "Have no Certificate");
        }
        isc_throw(UnsupportedAlgorithm,
                  "Unknown Certificate format: " <<
                  static_cast<int>(key_format));
    } else {
        isc_throw(UnsupportedAlgorithm,
                  "Unknown RSA Key kind: " << static_cast<int>(key_kind));
    }
}

/// @brief Check the validity
bool RsaAsymImpl::validate() const {
    Botan::AutoSeeded_RNG rng;
    Botan::X509_Store store;
    Botan::X509_Code status;
    switch (kind_) {
    case PUBLIC:
        // what to do?
        try {
            return pub_->check_key(rng, true);
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "check_key: " << exc.what());
        }
    case PRIVATE:
        try {
            return priv_->check_key(rng, true);
        } catch (const std::exception& exc) {
            isc_throw(LibraryError, "check_key: " << exc.what());
        }
    case CERT:
        store.add_cert(*x509_, true);
        status = store.validate_cert(*x509_);
        if (status == Botan::VERIFIED) {
            return true;
        }
        return false;
    default:
        return false;
    }
}

/// @brief Compare two keys
bool RsaAsymImpl::compare(const RsaAsymImpl* other,
                          const AsymKeyKind key_kind) const {
    if (!other || (other->algo_ != RSA_)) {
        return false;
    }
    Botan::BigInt e, n;
    switch (key_kind) {
    case CERT:
        // Special case for cert - cert
        if ((kind_ == CERT) && (other->kind_ == CERT)) {
            return (*x509_ == *other->x509_);
        }
        // At least one should be a cert
        if ((kind_ != CERT) && (other->kind_ != CERT)) {
            return false;
        }
        // For all other cases just compare public keys
        // Falls into
    case PUBLIC:
        if (kind_ == PRIVATE) {
            e = priv_->get_e();
            n = priv_->get_n();;
        } else if ((kind_ == PUBLIC) || (kind_ == CERT)) {
            e = pub_->get_e();
            n = pub_->get_n();
        } else {
            return false;
        }
        if (other->kind_ == PRIVATE) {
            return ((e == other->priv_->get_e()) &&
                    (n == other->priv_->get_n()));
        } else if ((other->kind_ == PUBLIC) || (other->kind_ == CERT)) {
            return ((e == other->pub_->get_e()) &&
                    (n == other->pub_->get_n()));
        } else {
            return false;
        }
    case PRIVATE:
        if ((kind_ != PRIVATE) || (other->kind_ != PRIVATE)) {
            return false;
        }
        // If public keys match so private too
        return ((priv_->get_e() == other->priv_->get_e()) &&
                (priv_->get_n() == other->priv_->get_n()));
    default:
        return false;
    }
}

} // namespace cryptolink
} // namespace isc
