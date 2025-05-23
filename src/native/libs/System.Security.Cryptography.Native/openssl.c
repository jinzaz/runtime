// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "pal_err.h"
#include "pal_types.h"
#include "pal_utilities.h"
#include "pal_safecrt.h"
#include "pal_x509.h"
#include "pal_ssl.h"
#include "memory_debug.h"
#include "openssl.h"

#ifdef FEATURE_DISTRO_AGNOSTIC_SSL
#include "opensslshim.h"
#endif

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if OPENSSL_VERSION_NUMBER >= OPENSSL_VERSION_1_1_0_RTM
c_static_assert(CRYPTO_EX_INDEX_X509 == 3);
#else
c_static_assert(CRYPTO_EX_INDEX_X509 == 10);
#endif

// See X509NameType.SimpleName
#define NAME_TYPE_SIMPLE 0
// See X509NameType.EmailName
#define NAME_TYPE_EMAIL 1
// See X509NameType.UpnName
#define NAME_TYPE_UPN 2
// See X509NameType.DnsName
#define NAME_TYPE_DNS 3
// See X509NameType.DnsFromAlternateName
#define NAME_TYPE_DNSALT 4
// See X509NameType.UrlName
#define NAME_TYPE_URL 5

/*
Function:
MakeTimeT

Used to convert the constituent elements of a struct tm into a time_t. As time_t does not have
a guaranteed blitting size, this function is static and cannot be p/invoked. It is here merely
as a utility.

Return values:
A time_t representation of the input date. See also man mktime(3).
*/
static time_t
MakeTimeT(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second, int32_t isDst)
{
    struct tm currentTm;
    currentTm.tm_year = year - 1900;
    currentTm.tm_mon = month - 1;
    currentTm.tm_mday = day;
    currentTm.tm_hour = hour;
    currentTm.tm_min = minute;
    currentTm.tm_sec = second;
    currentTm.tm_isdst = isDst;
    return mktime(&currentTm);
}

/*
Function:
GetX509Thumbprint

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to copy the SHA1
digest of the certificate (the thumbprint) into a managed buffer.

Return values:
0: Invalid X509 pointer
1: Data was copied
Any negative value: The input buffer size was reported as insufficient. A buffer of size ABS(return) is required.
*/
int32_t CryptoNative_GetX509Thumbprint(X509* x509, uint8_t* pBuf, int32_t cBuf)
{
    if (!x509)
    {
        return 0;
    }

    if (cBuf < SHA_DIGEST_LENGTH)
    {
        return -SHA_DIGEST_LENGTH;
    }

    ERR_clear_error();

    if (!X509_digest(x509, EVP_sha1(), pBuf, NULL))
    {
        return 0;
    }

    return 1;
}

/*
Function:
GetX509NotBefore

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to identify the
beginning of the validity period of the certificate in question.

Return values:
NULL if the validity cannot be determined, a pointer to the ASN1_TIME structure for the NotBefore value
otherwise.
*/
const ASN1_TIME* CryptoNative_GetX509NotBefore(X509* x509)
{
    // No error queue impact.

    if (x509)
    {
        return X509_get0_notBefore(x509);
    }

    return NULL;
}

/*
Function:
GetX509NotAfter

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to identify the
end of the validity period of the certificate in question.

Return values:
NULL if the validity cannot be determined, a pointer to the ASN1_TIME structure for the NotAfter value
otherwise.
*/
const ASN1_TIME* CryptoNative_GetX509NotAfter(X509* x509)
{
    // No error queue impact.

    if (x509)
    {
        return X509_get0_notAfter(x509);
    }

    return NULL;
}

/*
Function:
GetX509CrlNextUpdate

Used by System.Security.Cryptography.X509Certificates' CrlCache to identify the
end of the validity period of the certificate revocation list in question.

Return values:
NULL if the validity cannot be determined, a pointer to the ASN1_TIME structure for the NextUpdate value
otherwise.
*/
const ASN1_TIME* CryptoNative_GetX509CrlNextUpdate(X509_CRL* crl)
{
    // No error queue impact.

    if (crl)
    {
        return X509_CRL_get0_nextUpdate(crl);
    }

    return NULL;
}

/*
Function:
GetX509Version

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to identify the
X509 data format version for this certificate.

Return values:
-1 if the value cannot be determined
The encoded value of the version, otherwise:
  0: X509v1
  1: X509v2
  2: X509v3
*/
int32_t CryptoNative_GetX509Version(X509* x509)
{
    // No errors are expected to be written to the queue on this call,
    // and the managed caller doesn't check for one.

    if (x509)
    {
        return (int32_t)X509_get_version(x509);
    }

    return -1;
}

/*
Function:
GetX509PublicKeyAlgorithm

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to identify the
algorithm the public key is associated with.

Return values:
NULL if the algorithm cannot be determined, otherwise a pointer to the OpenSSL ASN1_OBJECT structure
describing the object type.
*/
ASN1_OBJECT* CryptoNative_GetX509PublicKeyAlgorithm(X509* x509)
{
    // No error queue impact, all of the called routines are just field accessors.

    if (x509)
    {
        X509_PUBKEY* pubkey = X509_get_X509_PUBKEY(x509);
        ASN1_OBJECT* algOid;

        if (pubkey && X509_PUBKEY_get0_param(&algOid, NULL, NULL, NULL, pubkey))
        {
            return algOid;
        }
    }

    return NULL;
}

/*
Function:
GetX509SignatureAlgorithm

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to identify the
algorithm used by the Certificate Authority for signing the certificate.

Return values:
NULL if the algorithm cannot be determined, otherwise a pointer to the OpenSSL ASN1_OBJECT structure
describing the object type.
*/
ASN1_OBJECT* CryptoNative_GetX509SignatureAlgorithm(X509* x509)
{
    // No error queue impact.

    if (x509)
    {
        const X509_ALGOR* sigAlg = X509_get0_tbs_sigalg(x509);

        if (sigAlg)
        {
            return sigAlg->algorithm;
        }
    }

    return NULL;
}

/*
Function:
GetX509PublicKeyParameterBytes

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to copy out the
parameters to the algorithm used by the certificate public key

Return values:
0: Invalid X509 pointer
1: Data was copied
2: No data exists.
Any negative value: The input buffer size was reported as insufficient. A buffer of size ABS(return) is required.
*/
int32_t CryptoNative_GetX509PublicKeyParameterBytes(X509* x509, uint8_t* pBuf, int32_t cBuf)
{
    ERR_clear_error();

    if (!x509)
    {
        return 0;
    }

    X509_PUBKEY* pubkey = X509_get_X509_PUBKEY(x509);

    if (!pubkey)
    {
        return 0;
    }

    X509_ALGOR* alg;

    if (!X509_PUBKEY_get0_param(NULL, NULL, NULL, &alg, pubkey) || !alg)
    {
        return 0;
    }

    ASN1_TYPE* parameter = alg->parameter;

    if (!parameter)
    {
        // Callers should not attempt to get the value if it was originally reported there is missing data.
        assert(pBuf == NULL);
        return 2;
    }

    int len = i2d_ASN1_TYPE(parameter, NULL);

    if (cBuf < len)
    {
        return -len;
    }

    unsigned char* pBuf2 = pBuf;
    len = i2d_ASN1_TYPE(parameter, &pBuf2);

    if (len > 0)
    {
        return 1;
    }

    return 0;
}

/*
Function:
GetX509PublicKeyBytes

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to obtain the
raw bytes of the public key.

Return values:
NULL if the public key cannot be determined, a pointer to the ASN1_BIT_STRING structure representing
the public key.
*/
ASN1_BIT_STRING* CryptoNative_GetX509PublicKeyBytes(X509* x509)
{
    // No error queue impact.

    if (x509)
    {
        return X509_get0_pubkey_bitstr(x509);
    }

    return NULL;
}

/*
Function:
GetAsn1StringBytes

Used by the NativeCrypto shim type to extract byte[] data from OpenSSL ASN1_* types whenever a byte[] is called
for in managed code.

Return values:
0: Invalid X509 pointer
1: Data was copied
Any negative value: The input buffer size was reported as insufficient. A buffer of size ABS(return) is required.

Remarks:
 Many ASN1 types are actually the same type in OpenSSL:
   STRING
   INTEGER
   ENUMERATED
   BIT_STRING
   OCTET_STRING
   PRINTABLESTRING
   T61STRING
   IA5STRING
   GENERALSTRING
   UNIVERSALSTRING
   BMPSTRING
   UTCTIME
   TIME
   GENERALIZEDTIME
   VISIBLEStRING
   UTF8STRING

 So this function will really work on all of them.
*/
int32_t CryptoNative_GetAsn1StringBytes(ASN1_STRING* asn1, uint8_t* pBuf, int32_t cBuf)
{
    // No error queue impact.

    if (!asn1 || cBuf < 0)
    {
        return 0;
    }

    int length = asn1->length;
    assert(length >= 0);
    if (length < 0)
    {
        return 0;
    }

    if (!pBuf || cBuf < length)
    {
        return -length;
    }

    memcpy_s(pBuf, Int32ToSizeT(cBuf), asn1->data, Int32ToSizeT(length));
    return 1;
}

/*
Function:
GetX509NameRawBytes

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader to obtain the
DER encoded value of an X500DistinguishedName.

Return values:
0: Invalid X509 pointer
1: Data was copied
Any negative value: The input buffer size was reported as insufficient. A buffer of size ABS(return) is required.
*/
int32_t CryptoNative_GetX509NameRawBytes(X509_NAME* x509Name, uint8_t* pBuf, int32_t cBuf)
{
    ERR_clear_error();

    const uint8_t* nameBuf;
    size_t nameBufLen;

    if (!x509Name || cBuf < 0 || !X509_NAME_get0_der(x509Name, &nameBuf, &nameBufLen))
    {
        return 0;
    }

    /*
     * length is size_t on some platforms and int on others, so the comparisons
     * are not tautological everywhere. We can let the compiler optimize away
     * any part of the check that is. We split the size checks into two checks
     * so we can get around the warnings on Linux where the Length is unsigned
     * whereas Length is signed on OS X. The first check makes sure the variable
     * value is less than INT_MAX in it's native format; once we know it is not
     * too large, we can safely cast to an int to make sure it is not negative
     */
    if (nameBufLen > INT_MAX)
    {
        assert(0 && "Huge length X509_NAME");
        return 0;
    }

    int length = (int)(nameBufLen);

    if (length < 0)
    {
        assert(0 && "Negative length X509_NAME");
        return 0;
    }

    if (!pBuf || cBuf < length)
    {
        return -length;
    }

    memcpy_s(pBuf, Int32ToSizeT(cBuf), nameBuf, Int32ToSizeT(length));
    return 1;
}

/*
Function:
GetX509NameInfo

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader as the entire
implementation of X509Certificate2.GetNameInfo.

Return values:
NULL if the certificate is invalid or no name information could be found, otherwise a pointer to a
memory-backed BIO structure which contains the answer to the GetNameInfo query
*/
BIO* CryptoNative_GetX509NameInfo(X509* x509, int32_t nameType, int32_t forIssuer)
{
    static const char szOidUpn[] = "1.3.6.1.4.1.311.20.2.3";

    ERR_clear_error();

    if (!x509 || nameType < NAME_TYPE_SIMPLE || nameType > NAME_TYPE_URL)
    {
        return NULL;
    }

    // Algorithm behaviors (pseudocode).  When forIssuer is true, replace "Subject" with "Issuer" and
    // SAN (Subject Alternative Names) with IAN (Issuer Alternative Names).
    //
    // SimpleName: Subject[CN] ?? Subject[OU] ?? Subject[O] ?? Subject[E] ?? Subject.Rdns.FirstOrDefault() ??
    // SAN.Entries.FirstOrDefault(type == GEN_EMAIL);
    // EmailName: SAN.Entries.FirstOrDefault(type == GEN_EMAIL) ?? Subject[E];
    // UpnName: SAN.Entries.FirsOrDefaultt(type == GEN_OTHER && entry.AsOther().OID == szOidUpn).AsOther().Value;
    // DnsName: SAN.Entries.FirstOrDefault(type == GEN_DNS) ?? Subject[CN];
    // DnsFromAlternativeName: SAN.Entries.FirstOrDefault(type == GEN_DNS);
    // UrlName: SAN.Entries.FirstOrDefault(type == GEN_URI);
    if (nameType == NAME_TYPE_SIMPLE)
    {
        X509_NAME* name = forIssuer ? X509_get_issuer_name(x509) : X509_get_subject_name(x509);

        if (name)
        {
            ASN1_STRING* cn = NULL;
            ASN1_STRING* ou = NULL;
            ASN1_STRING* o = NULL;
            ASN1_STRING* e = NULL;
            ASN1_STRING* firstRdn = NULL;

            // Walk the list backwards because it is stored in stack order
            for (int i = X509_NAME_entry_count(name) - 1; i >= 0; --i)
            {
                X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);

                if (!entry)
                {
                    continue;
                }

                ASN1_OBJECT* oid = X509_NAME_ENTRY_get_object(entry);
                ASN1_STRING* str = X509_NAME_ENTRY_get_data(entry);

                if (!oid || !str)
                {
                    continue;
                }

                int nid = OBJ_obj2nid(oid);

                if (nid == NID_commonName)
                {
                    // CN wins, so no need to keep looking.
                    cn = str;
                    break;
                }
                else if (nid == NID_organizationalUnitName)
                {
                    ou = str;
                }
                else if (nid == NID_organizationName)
                {
                    o = str;
                }
                else if (nid == NID_pkcs9_emailAddress)
                {
                    e = str;
                }
                else if (!firstRdn)
                {
                    firstRdn = str;
                }
            }

            ASN1_STRING* answer = cn;

            // If there was no CN, but there was something, then perform fallbacks.
            if (!answer && firstRdn)
            {
                answer = ou;

                if (!answer)
                {
                    answer = o;
                }

                if (!answer)
                {
                    answer = e;
                }

                if (!answer)
                {
                    answer = firstRdn;
                }
            }

            if (answer)
            {
                BIO* b = BIO_new(BIO_s_mem());
                ASN1_STRING_print_ex(b, answer, ASN1_STRFLGS_UTF8_CONVERT);
                return b;
            }
        }
    }

    if (nameType == NAME_TYPE_SIMPLE || nameType == NAME_TYPE_DNS || nameType == NAME_TYPE_DNSALT ||
        nameType == NAME_TYPE_EMAIL || nameType == NAME_TYPE_UPN || nameType == NAME_TYPE_URL)
    {
        int expectedType = -1;

        switch (nameType)
        {
            case NAME_TYPE_DNS:
            case NAME_TYPE_DNSALT:
                expectedType = GEN_DNS;
                break;
            case NAME_TYPE_SIMPLE:
            case NAME_TYPE_EMAIL:
                expectedType = GEN_EMAIL;
                break;
            case NAME_TYPE_UPN:
                expectedType = GEN_OTHERNAME;
                break;
            case NAME_TYPE_URL:
                expectedType = GEN_URI;
                break;
        }

        GENERAL_NAMES* altNames = (GENERAL_NAMES*)
            X509_get_ext_d2i(x509, forIssuer ? NID_issuer_alt_name : NID_subject_alt_name, NULL, NULL);

        if (altNames)
        {
            int i;

            for (i = 0; i < sk_GENERAL_NAME_num(altNames); ++i)
            {
                GENERAL_NAME* altName = sk_GENERAL_NAME_value(altNames, i);

                if (altName && altName->type == expectedType)
                {
                    ASN1_STRING* str = NULL;

                    switch (nameType)
                    {
                        case NAME_TYPE_DNS:
                        case NAME_TYPE_DNSALT:
                            str = altName->d.dNSName;
                            break;
                        case NAME_TYPE_SIMPLE:
                        case NAME_TYPE_EMAIL:
                            str = altName->d.rfc822Name;
                            break;
                        case NAME_TYPE_URL:
                            str = altName->d.uniformResourceIdentifier;
                            break;
                        case NAME_TYPE_UPN:
                        {
                            OTHERNAME* value = altName->d.otherName;

                            if (value)
                            {
                                // Enough more padding than szOidUpn that a \0 won't accidentally align
                                char localOid[sizeof(szOidUpn) + 3];
                                int cchLocalOid = 1 + OBJ_obj2txt(localOid, sizeof(localOid), value->type_id, 1);

                                if (sizeof(szOidUpn) == cchLocalOid &&
                                    0 == strncmp(localOid, szOidUpn, sizeof(szOidUpn)))
                                {
                                    if (value->value)
                                    {
                                        // OTHERNAME->ASN1_TYPE->union.field
                                        str = value->value->value.asn1_string;
                                    }
                                }
                            }

                            break;
                        }
                    }

                    if (str)
                    {
                        BIO* b = BIO_new(BIO_s_mem());
                        ASN1_STRING_print_ex(b, str, ASN1_STRFLGS_UTF8_CONVERT);
                        GENERAL_NAMES_free(altNames);
                        return b;
                    }
                }
            }

            GENERAL_NAMES_free(altNames);
        }
    }

    if (nameType == NAME_TYPE_EMAIL || nameType == NAME_TYPE_DNS)
    {
        X509_NAME* name = forIssuer ? X509_get_issuer_name(x509) : X509_get_subject_name(x509);
        int expectedNid = NID_undef;

        switch (nameType)
        {
            case NAME_TYPE_EMAIL:
                expectedNid = NID_pkcs9_emailAddress;
                break;
            case NAME_TYPE_DNS:
                expectedNid = NID_commonName;
                break;
        }

        if (name)
        {
            // Walk the list backwards because it is stored in stack order
            for (int i = X509_NAME_entry_count(name) - 1; i >= 0; --i)
            {
                X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);

                if (!entry)
                {
                    continue;
                }

                ASN1_OBJECT* oid = X509_NAME_ENTRY_get_object(entry);
                ASN1_STRING* str = X509_NAME_ENTRY_get_data(entry);

                if (!oid || !str)
                {
                    continue;
                }

                int nid = OBJ_obj2nid(oid);

                if (nid == expectedNid)
                {
                    BIO* b = BIO_new(BIO_s_mem());
                    ASN1_STRING_print_ex(b, str, 0);
                    return b;
                }
            }
        }
    }

    return NULL;
}

/*
Function:
CheckX509Hostname

Used by System.Net.Security's Unix CertModule to identify if the certificate presented by
the server is applicable to the hostname requested.

Return values:
1 if the hostname is a match
0 if the hostname is not a match
Any negative number indicates an error in the arguments.
*/
int32_t CryptoNative_CheckX509Hostname(X509* x509, const char* hostname, int32_t cchHostname)
{
    // Input errors.  OpenSSL might return -1 or -2, so skip those.
    if (!x509)
        return -3;
    if (cchHostname > 0 && !hostname)
        return -4;
    if (cchHostname < 0)
        return -5;

    ERR_clear_error();

    // OpenSSL will treat a target hostname starting with '.' as special.
    // We don't expect target hostnames to start with '.', but if one gets in here, the fallback
    // and the mainline won't be the same... so just make it report false.
    if (cchHostname > 0 && hostname[0] == '.')
    {
        return 0;
    }

    return X509_check_host(
        x509,
        hostname,
        (size_t)cchHostname,
        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS,
        NULL);
}

/*
Function:
CheckX509IpAddress

Used by System.Net.Security's Unix CertModule to identify if the certificate presented by
the server is applicable to the hostname (an IP address) requested.

Return values:
1 if the hostname is a match
0 if the hostname is not a match
Any negative number indicates an error in the arguments.
*/
int32_t CryptoNative_CheckX509IpAddress(
    X509* x509, const uint8_t* addressBytes, int32_t addressBytesLen, const char* hostname, int32_t cchHostname)
{
    if (!x509)
        return -2;
    if (cchHostname > 0 && !hostname)
        return -3;
    if (cchHostname < 0)
        return -4;
    if (addressBytesLen < 0)
        return -5;
    if (!addressBytes)
        return -6;

    ERR_clear_error();

    int subjectNid = NID_commonName;
    int sanGenType = GEN_IPADD;
    GENERAL_NAMES* san = (GENERAL_NAMES*)(X509_get_ext_d2i(x509, NID_subject_alt_name, NULL, NULL));
    int success = 0;

    if (san)
    {
        int i;
        int count = sk_GENERAL_NAME_num(san);

        for (i = 0; i < count; ++i)
        {
            GENERAL_NAME* sanEntry = sk_GENERAL_NAME_value(san, i);
            ASN1_OCTET_STRING* ipAddr;

            if (sanEntry->type != sanGenType)
            {
                continue;
            }

            ipAddr = sanEntry->d.iPAddress;

            if (!ipAddr || !ipAddr->data || ipAddr->length != addressBytesLen)
            {
                continue;
            }

            if (!memcmp(addressBytes, ipAddr->data, (size_t)addressBytesLen))
            {
                success = 1;
                break;
            }
        }

        GENERAL_NAMES_free(san);
    }

    if (!success)
    {
        // This is a shared/interor pointer, do not free!
        X509_NAME* subject = X509_get_subject_name(x509);

        if (subject)
        {
            int i = -1;

            while ((i = X509_NAME_get_index_by_NID(subject, subjectNid, i)) >= 0)
            {
                // Shared/interior pointers, do not free!
                X509_NAME_ENTRY* nameEnt = X509_NAME_get_entry(subject, i);
                ASN1_STRING* cn = X509_NAME_ENTRY_get_data(nameEnt);

                if (cn->length == cchHostname &&
                    !strncasecmp((const char*)cn->data, hostname, (size_t)cchHostname))
                {
                    success = 1;
                    break;
                }
            }
        }
    }

    return success;
}
/*
Function:
GetX509StackFieldCount

Used by System.Security.Cryptography.X509Certificates' OpenSslX509ChainProcessor to identify the
number of certificates returned in the built chain.

Return values:
0 if the field count cannot be determined, or the count of certificates in STACK_OF(X509)
Note that 0 does not always indicate an error, merely that GetX509StackField should not be called.
*/
int32_t CryptoNative_GetX509StackFieldCount(STACK_OF(X509) * stack)
{
    // No error queue impact.
    return sk_X509_num(stack);
}

/*
Function:
GetX509StackField

Used by System.Security.Cryptography.X509Certificates' OpenSslX509ChainProcessor to get a pointer to
the indexed member of a chain.

Return values:
NULL if stack is NULL or loc is out of bounds, otherwise a pointer to the X509 structure encoding
that particular element.
*/
X509* CryptoNative_GetX509StackField(STACK_OF(X509) * stack, int loc)
{
    // No error queue impact.
    return sk_X509_value(stack, loc);
}

/*
Function:
RecursiveFreeX509Stack

Used by System.Security.Cryptography.X509Certificates' OpenSslX509ChainProcessor to free a stack
when done with it.
*/
void CryptoNative_RecursiveFreeX509Stack(STACK_OF(X509) * stack)
{
    // No error queue impact.
    sk_X509_pop_free(stack, X509_free);
}

/*
Function:
SetX509StoreVerifyTime

Used by System.Security.Cryptography.X509Certificates' OpenSslX509ChainProcessor to assign the
verification time to the chain building.  The input is in LOCAL time, not UTC.

Return values:
0 if ctx is NULL, if ctx has no X509_VERIFY_PARAM, or the date inputs don't produce a valid time_t;
1 on success.
*/
int32_t CryptoNative_X509StoreSetVerifyTime(X509_STORE* ctx,
                                            int32_t year,
                                            int32_t month,
                                            int32_t day,
                                            int32_t hour,
                                            int32_t minute,
                                            int32_t second,
                                            int32_t isDst)
{
    ERR_clear_error();

    if (!ctx)
    {
        return 0;
    }

    time_t verifyTime = MakeTimeT(year, month, day, hour, minute, second, isDst);

    if (verifyTime == (time_t)-1)
    {
        return 0;
    }

    X509_VERIFY_PARAM* verifyParams = X509_STORE_get0_param(ctx);

    if (!verifyParams)
    {
        return 0;
    }

#if defined(FEATURE_DISTRO_AGNOSTIC_SSL) && defined(TARGET_ARM) && defined(TARGET_LINUX)
    if (g_libSslUses32BitTime)
    {
        if (verifyTime > INT_MAX || verifyTime < INT_MIN)
        {
            return 0;
        }

        // Cast to a signature that takes a 32-bit value for the time.
        ((void (*)(X509_VERIFY_PARAM*, int32_t))(void*)(X509_VERIFY_PARAM_set_time))(verifyParams, (int32_t)verifyTime);
        return 1;
    }
#endif

    X509_VERIFY_PARAM_set_time(verifyParams, verifyTime);
    return 1;
}

/*
Function:
ReadX509AsDerFromBio

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader when attempting
to turn the contents of a file into an ICertificatePal object.

Return values:
If bio contains a valid DER-encoded X509 object, a pointer to that X509 structure that was deserialized,
otherwise NULL.
*/
X509* CryptoNative_ReadX509AsDerFromBio(BIO* bio)
{
    ERR_clear_error();
    return d2i_X509_bio(bio, NULL);
}

/*
Function:
BioTell

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader when attempting
to turn the contents of a file into an ICertificatePal object to allow seeking back to the start point
in the event of a deserialization failure.

Return values:
The current seek position of the BIO if it is a file-related BIO, -1 on NULL inputs, and has unspecified
behavior on non-file, non-null BIO objects.

See also:
OpenSSL's BIO_tell
*/
int32_t CryptoNative_BioTell(BIO* bio)
{
    // No error queue impact.

    if (!bio)
    {
        return -1;
    }

    return BIO_tell(bio);
}

/*
Function:
BioTell

Used by System.Security.Cryptography.X509Certificates' OpenSslX509CertificateReader when attempting
to turn the contents of a file into an ICertificatePal object to seek back to the start point
in the event of a deserialization failure.

Return values:
-1 if bio is NULL
-1 if bio is a file-related BIO and seek fails
0 if bio is a file-related BIO and seek succeeds
otherwise unspecified

See also:
OpenSSL's BIO_seek
*/
int32_t CryptoNative_BioSeek(BIO* bio, int32_t ofs)
{
    // No error queue impact.

    if (!bio)
    {
        return -1;
    }

    return BIO_seek(bio, ofs);
}

/*
Function:
NewX509Stack

Used by System.Security.Cryptography.X509Certificates when needing to pass a collection
of X509* to OpenSSL.

Return values:
A STACK_OF(X509*) with no comparator.
*/
STACK_OF(X509) * CryptoNative_NewX509Stack(void)
{
    ERR_clear_error();
    return sk_X509_new_null();
}

/*
Function:
PushX509StackField

Used by System.Security.Cryptography.X509Certificates when needing to pass a collection
of X509* to OpenSSL.

Return values:
1 on success
0 on a NULL stack, or an error within sk_X509_push
*/
int32_t CryptoNative_PushX509StackField(STACK_OF(X509) * stack, X509* x509)
{
    ERR_clear_error();

    if (!stack)
    {
        return 0;
    }

    return sk_X509_push(stack, x509);
}

/*
Function:
GetRandomBytes

Puts num cryptographically strong pseudo-random bytes into buf.

Return values:
Returns a bool to managed code.
1 for success
0 for failure
*/
int32_t CryptoNative_GetRandomBytes(uint8_t* buf, int32_t num)
{
    ERR_clear_error();
    int ret = RAND_bytes(buf, num);

    return ret == 1;
}

/*
Function:
LookupFriendlyNameByOid

Looks up the FriendlyName value for a given OID in string representation.
For example, "1.3.14.3.2.26" => "sha1".

Return values:
1 indicates that *friendlyName contains a pointer to the friendly name value
0 indicates that the OID was not found, or no friendly name exists for that OID
-1 indicates OpenSSL signalled an error, CryptographicException should be raised.
-2 indicates an error in the input arguments
*/
int32_t CryptoNative_LookupFriendlyNameByOid(const char* oidValue, const char** friendlyName)
{
    ASN1_OBJECT* oid;
    int nid;
    const char* ln;

    ERR_clear_error();

    if (!oidValue || !friendlyName)
    {
        return -2;
    }

    // First, check if oidValue parses as a dotted decimal OID. If not, we'll
    // return not-found and let the system cache that.
    int asnRet = a2d_ASN1_OBJECT(NULL, 0, oidValue, -1);

    if (asnRet <= 0)
    {
        return 0;
    }

    // Do a lookup with no_name set. The purpose of this function is to map only the
    // dotted decimal to the friendly name. "sha1" in should not result in "sha1" out.
    oid = OBJ_txt2obj(oidValue, 1);

    if (oid == NULL)
    {
        // We know that the OID parsed (unless it underwent concurrent modification,
        // which is unsupported), so any error in this stage should be an exception.
        return -1;
    }

    // Look in the predefined, and late-registered, OIDs list to get the lookup table
    // identifier for this OID.  The OBJ_txt2obj object will not have ln set.
    nid = OBJ_obj2nid(oid);
    ASN1_OBJECT_free(oid);

    if (nid == NID_undef)
    {
        return 0;
    }

    // Get back a shared pointer to the long name from the registration table.
    ln = OBJ_nid2ln(nid);

    if (ln)
    {
        *friendlyName = ln;
        return 1;
    }

    return 0;
}

/*
Function:
SSLeay (OpenSSL_version_num for OpenSSL 1.1+)

Gets the version of openssl library.

Return values:
Version number as MNNFFRBB (major minor fix final beta/patch)
*/
int64_t CryptoNative_OpenSslVersionNumber(void)
{
    // No error queue impact.
    return (int64_t)OpenSSL_version_num();
}

static void ExDataFreeOcspResponse(
    void* parent,
    void* ptr,
    CRYPTO_EX_DATA* ad,
    int idx,
    long argl,
    void* argp)
{
    (void)parent;
    (void)ad;
    (void)idx;
    (void)argl;
    (void)argp;

    if (ptr != NULL)
    {
        if (idx == g_x509_ocsp_index)
        {
            OCSP_RESPONSE_free((OCSP_RESPONSE*)ptr);
        }
    }
}

// In the OpenSSL 1.0.2 headers, the `from` argument is not const (became const in 1.1.0)
// In the OpenSSL 3 headers, `from_d` changed from (void*) to (void**).
static int ExDataDupOcspResponse(
    CRYPTO_EX_DATA* to,
#if OPENSSL_VERSION_NUMBER >= OPENSSL_VERSION_1_1_0_RTM
    const CRYPTO_EX_DATA* from,
#else
    CRYPTO_EX_DATA* from,
#endif
#if OPENSSL_VERSION_NUMBER >= OPENSSL_VERSION_3_0_RTM
    void** from_d,
#else
    void* from_d,
#endif
    int idx,
    long argl,
    void* argp)
{
    (void)to;
    (void)from;
    (void)idx;
    (void)argl;
    (void)argp;

    // From the docs (https://www.openssl.org/docs/man1.1.1/man3/CRYPTO_get_ex_new_index.html):
    // "The from_d parameter needs to be cast to a void **pptr as the API has currently the wrong signature ..."
    void** pptr = (void**)from_d;

    if (pptr != NULL)
    {
        if (idx == g_x509_ocsp_index)
        {
            *pptr = NULL;
        }
    }

    // If the dup_func() returns 0 the whole CRYPTO_dup_ex_data() will fail.
    // So, return 1 unless we returned 0 already.
    return 1;
}

static void ExDataFreeNoOp(
    void* parent,
    void* ptr,
    CRYPTO_EX_DATA* ad,
    int idx,
    long argl,
    void* argp)
{
    (void)parent;
    (void)ptr;
    (void)ad;
    (void)idx;
    (void)argl;
    (void)argp;

    // do nothing.
}

static int ExDataDupNoOp(
    CRYPTO_EX_DATA* to,
#if OPENSSL_VERSION_NUMBER >= OPENSSL_VERSION_1_1_0_RTM
    const CRYPTO_EX_DATA* from,
#else
    CRYPTO_EX_DATA* from,
#endif
#if OPENSSL_VERSION_NUMBER >= OPENSSL_VERSION_3_0_RTM
    void** from_d,
#else
    void* from_d,
#endif
    int idx,
    long argl,
    void* argp)
{
    (void)to;
    (void)from;
    (void)from_d;
    (void)idx;
    (void)argl;
    (void)argp;

    // do nothing, this should lead to copy of the pointer being stored in the
    // destination, we treat the ptr as an opaque blob.
    return 1;
}

void CryptoNative_RegisterLegacyAlgorithms(void)
{
#ifdef NEED_OPENSSL_3_0
    if (API_EXISTS(OSSL_PROVIDER_try_load))
    {
        OSSL_PROVIDER_try_load(NULL, "legacy", 1);

        // Doesn't matter if it succeeded or failed.
        ERR_clear_error();
    }
#endif
}

int32_t CryptoNative_IsSignatureAlgorithmAvailable(const char* algorithm)
{
    int32_t ret = 0;

#if defined(NEED_OPENSSL_3_0) && HAVE_OPENSSL_EVP_PKEY_SIGN_MESSAGE_INIT
    if (!API_EXISTS(EVP_PKEY_sign_message_init) ||
        !API_EXISTS(EVP_PKEY_verify_message_init))
    {
        return 0;
    }

    EVP_SIGNATURE* sigAlg = NULL;

    sigAlg = EVP_SIGNATURE_fetch(NULL, algorithm, NULL);
    if (sigAlg)
    {
        ret = 1;
        EVP_SIGNATURE_free(sigAlg);
    }
#endif

    (void)algorithm;
    return ret;
}

#ifdef NEED_OPENSSL_1_0
// Lock used to make sure EnsureopenSslInitialized itself is thread safe
static pthread_mutex_t g_initLock = PTHREAD_MUTEX_INITIALIZER;

// Set of locks initialized for OpenSSL
static pthread_mutex_t* g_locks = NULL;

/*
Function:
LockingCallback

Called back by OpenSSL to lock or unlock.
*/
static void LockingCallback(int mode, int n, const char* file, int line)
{
    (void)file, (void)line; // deliberately unused parameters

// Clang complains about releasing locks that are not held.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"

#ifndef CRYPTO_LOCK
#define CRYPTO_LOCK 1
#endif

    int result;
    if (mode & CRYPTO_LOCK)
    {
        result = pthread_mutex_lock(&g_locks[n]);
    }
    else
    {
        result = pthread_mutex_unlock(&g_locks[n]);
    }

    if (result != 0)
    {
        assert(0 && "LockingCallback failed.");
    }
#pragma clang diagnostic pop
}

/*
Function:
EnsureOpenSslInitialized

Initializes OpenSSL with a locking callback to ensure thread safety.

Return values:
0 on success
non-zero on failure
*/
static int32_t EnsureOpenSsl10Initialized(void)
{
    int ret = 0;
    int numLocks = 0;
    int locksInitialized = 0;
    int randPollResult = 0;

    pthread_mutex_lock(&g_initLock);

    if (g_locks != NULL)
    {
        // Already initialized; nothing more to do.
        goto done;
    }

    // Determine how many locks are needed
    numLocks = CRYPTO_num_locks();
    if (numLocks <= 0)
    {
        assert(0 && "CRYPTO_num_locks returned invalid value.");
        ret = 1;
        goto done;
    }

    // Create the locks array
    size_t allocationSize = 0;
    if (!multiply_s(sizeof(pthread_mutex_t), (size_t)numLocks, &allocationSize))
    {
        ret = 2;
        goto done;
    }

    g_locks = (pthread_mutex_t*)malloc(allocationSize);
    if (g_locks == NULL)
    {
        ret = 2;
        goto done;
    }

    // Initialize each of the locks
    for (locksInitialized = 0; locksInitialized < numLocks; locksInitialized++)
    {
        if (pthread_mutex_init(&g_locks[locksInitialized], NULL) != 0)
        {
            ret = 3;
            goto done;
        }
    }

    // Initialize the callback
    CRYPTO_set_locking_callback(LockingCallback);

    // Initialize the random number generator seed
    randPollResult = RAND_poll();
    if (randPollResult < 1)
    {
        ret = 4;
        goto done;
    }

    // Load the SHA-2 hash algorithms, and anything else not in the default
    // support set.
    OPENSSL_add_all_algorithms_conf();

    // Ensure that the error message table is loaded.
    ERR_load_crypto_strings();

    // In OpenSSL 1.0.2-, CRYPTO_EX_INDEX_X509 is 10.
    g_x509_ocsp_index = CRYPTO_get_ex_new_index(10, 0, NULL, NULL, ExDataDupOcspResponse, ExDataFreeOcspResponse);
    // In OpenSSL 1.0.2-, CRYPTO_EX_INDEX_SSL_SESSION is 3.
    g_ssl_sess_cert_index = CRYPTO_get_ex_new_index(3, 0, NULL, NULL, ExDataDupNoOp, ExDataFreeNoOp);

done:
    if (ret != 0)
    {
        // Cleanup on failure
        if (g_locks != NULL)
        {
            for (int i = locksInitialized - 1; i >= 0; i--)
            {
                pthread_mutex_destroy(&g_locks[i]); // ignore failures
            }
            free(g_locks);
            g_locks = NULL;
        }
    }

    pthread_mutex_unlock(&g_initLock);
    return ret;
}
#endif // NEED_OPENSSL_1_0 */

#if defined NEED_OPENSSL_1_1 || defined NEED_OPENSSL_3_0

// Only defined in OpenSSL 1.1.1+, has no effect on 1.1.0.
#ifndef OPENSSL_INIT_NO_ATEXIT
    #define OPENSSL_INIT_NO_ATEXIT 0x00080000L
#endif

pthread_mutex_t g_err_mutex = PTHREAD_MUTEX_INITIALIZER;
int volatile g_err_unloaded = 0;

static void HandleShutdown(void)
{
    // Generally, a mutex to set a boolean is overkill, but this lock
    // ensures that there are no callers already inside the string table
    // when the unload (possibly) executes.
    int result = pthread_mutex_lock(&g_err_mutex);
    assert(!result && "Acquiring the error string table mutex failed.");

    g_err_unloaded = 1;

    result = pthread_mutex_unlock(&g_err_mutex);
    assert(!result && "Releasing the error string table mutex failed.");
}

static int32_t EnsureOpenSsl11Initialized(void)
{
    // In OpenSSL 1.0 we call OPENSSL_add_all_algorithms_conf() and ERR_load_crypto_strings(),
    // so do the same for 1.1
    OPENSSL_init_ssl(
        // OPENSSL_add_all_algorithms_conf
            OPENSSL_INIT_ADD_ALL_CIPHERS |
            OPENSSL_INIT_ADD_ALL_DIGESTS |
            OPENSSL_INIT_LOAD_CONFIG |
        // Do not unload on process exit, as the CLR may still have threads running
            OPENSSL_INIT_NO_ATEXIT |
        // ERR_load_crypto_strings
            OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
            OPENSSL_INIT_LOAD_SSL_STRINGS,
        NULL);

    // As a fallback for when the NO_ATEXIT isn't respected, register a later
    // atexit handler, so we will indicate that we're in the shutdown state
    // and stop asking problematic questions from other threads.
    atexit(HandleShutdown);

    // In OpenSSL 1.1.0+, CRYPTO_EX_INDEX_X509 is 3.
    g_x509_ocsp_index = CRYPTO_get_ex_new_index(3, 0, NULL, NULL, ExDataDupOcspResponse, ExDataFreeOcspResponse);
    // In OpenSSL 1.1.0+, CRYPTO_EX_INDEX_SSL_SESSION is 2.
    g_ssl_sess_cert_index = CRYPTO_get_ex_new_index(2, 0, NULL, NULL, ExDataDupNoOp, ExDataFreeNoOp);
    return 0;
}

#endif

int32_t CryptoNative_OpenSslAvailable(void)
{
#ifdef FEATURE_DISTRO_AGNOSTIC_SSL
    // OpenLibrary will attempt to open libssl. DlOpen will handle
    // the case of it already being open and dlclose the duplicate
    return OpenLibrary();
#else
    return 1;
#endif
}

static int32_t g_initStatus = 1;
int g_x509_ocsp_index = -1;
int g_ssl_sess_cert_index = -1;

static int32_t EnsureOpenSslInitializedCore(void)
{
    int ret = 0;

    // If portable then decide which OpenSSL we are, and call the right one.
    // If 1.0, call the 1.0 one.
    // Otherwise call the 1.1 one.
#ifdef FEATURE_DISTRO_AGNOSTIC_SSL
    InitializeOpenSSLShim();
#endif
    // This needs to be done before any allocation is done e.g. EnsureOpenSsl* is called.
    // And it also needs to be after the pointers are loaded for DISTRO_AGNOSTIC_SSL
    InitializeMemoryDebug();

#ifdef FEATURE_DISTRO_AGNOSTIC_SSL
    if (API_EXISTS(SSL_state))
    {
        ret = EnsureOpenSsl10Initialized();
    }
    else
    {
        ret = EnsureOpenSsl11Initialized();
    }
#elif OPENSSL_VERSION_NUMBER < OPENSSL_VERSION_1_1_0_RTM
    ret = EnsureOpenSsl10Initialized();
#else
    ret = EnsureOpenSsl11Initialized();
#endif

    if (ret == 0)
    {
        // On OpenSSL 1.0.2 our expected index is 0.
        // On OpenSSL 1.1.0+ 0 is a reserved value and we expect 1.
        assert(g_x509_ocsp_index != -1);
        assert(g_ssl_sess_cert_index != -1);
    }

    return ret;
}

static void EnsureOpenSslInitializedOnce(void)
{
    g_initStatus = EnsureOpenSslInitializedCore();
}

static pthread_once_t g_initializeShim = PTHREAD_ONCE_INIT;

int32_t CryptoNative_EnsureOpenSslInitialized(void)
{
    pthread_once(&g_initializeShim, EnsureOpenSslInitializedOnce);
    return g_initStatus;
}
