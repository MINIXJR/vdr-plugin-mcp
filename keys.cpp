/*
 * keys.cpp: API keys of the HTTP transport, TLS certificate
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "keys.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

cMcpKeys McpKeys;

static std::string Hex(const unsigned char *Data, size_t Length)
{
  static const char *digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < Length; i++) {
      s += digits[Data[i] >> 4];
      s += digits[Data[i] & 15];
      }
  return s;
}

std::string RandomHex(int Bytes)
{
  std::vector<unsigned char> b(Bytes);
  if (RAND_bytes(b.data(), Bytes) != 1)
     return "";
  return Hex(b.data(), b.size());
}

std::string Sha256Hex(const std::string &Data)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  if (EVP_Digest(Data.data(), Data.size(), md, &n, EVP_sha256(), NULL) != 1)
     return "";
  return Hex(md, n);
}

// --- cMcpKeys --------------------------------------------------------------

bool cMcpKeys::Load(void)
{
  std::lock_guard<std::mutex> lock(mutex);
  keys.clear();
  std::ifstream f(fileName);
  if (!f)
     return false;
  std::string line;
  while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
           continue;
        std::istringstream is(line);
        cMcpKey k;
        std::string role;
        if (is >> k.name >> role >> k.created >> k.hash) {
           k.readOnly = role != "rw";
           keys.push_back(k);
           }
        }
  return true;
}

bool cMcpKeys::Save(void)
{
  std::string tmp = fileName + ".tmp";
  FILE *f = fopen(tmp.c_str(), "w");
  if (!f)
     return false;
  fchmod(fileno(f), 0600);
  fprintf(f, "# API keys of vdr-plugin-mcp: name role created sha256(key)\n");
  fprintf(f, "# managed with: svdrpsend PLUG mcp KEYADD <name> ro|rw / KEYLIST / KEYDEL <name>\n");
  for (auto &k : keys)
      fprintf(f, "%s %s %ld %s\n", k.name.c_str(), k.readOnly ? "ro" : "rw", k.created, k.hash.c_str());
  if (fclose(f) != 0)
     return false;
  return rename(tmp.c_str(), fileName.c_str()) == 0;
}

static bool ValidName(const std::string &Name)
{
  if (Name.empty() || Name.size() > 32)
     return false;
  for (char c : Name) {
      if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.')
         return false;
      }
  return true;
}

std::string cMcpKeys::Add(const std::string &Name, bool ReadOnly, std::string &Error)
{
  if (!ValidName(Name)) {
     Error = "invalid name, use 1-32 characters A-Z a-z 0-9 _ - .";
     return "";
     }
  std::lock_guard<std::mutex> lock(mutex);
  for (auto &k : keys) {
      if (k.name == Name) {
         Error = "a key named '" + Name + "' exists already";
         return "";
         }
      }
  std::string key = RandomHex(32);
  if (key.empty()) {
     Error = "no random numbers";
     return "";
     }
  cMcpKey k;
  k.name = Name;
  k.readOnly = ReadOnly;
  k.created = time(NULL);
  k.hash = Sha256Hex(key);
  keys.push_back(k);
  if (!Save()) {
     keys.pop_back();
     Error = "cannot write " + fileName;
     return "";
     }
  return key;
}

bool cMcpKeys::Del(const std::string &Name)
{
  std::lock_guard<std::mutex> lock(mutex);
  for (auto it = keys.begin(); it != keys.end(); ++it) {
      if (it->name == Name) {
         keys.erase(it);
         return Save();
         }
      }
  return false;
}

std::vector<cMcpKey> cMcpKeys::List(void) const
{
  std::lock_guard<std::mutex> lock(mutex);
  return keys;
}

bool cMcpKeys::Check(const std::string &Key, std::string &Name, bool &ReadOnly) const
{
  if (Key.empty())
     return false;
  std::string hash = Sha256Hex(Key);
  std::lock_guard<std::mutex> lock(mutex);
  for (auto &k : keys) {
      if (k.hash.size() == hash.size() && CRYPTO_memcmp(k.hash.data(), hash.data(), hash.size()) == 0) {
         Name = k.name;
         ReadOnly = k.readOnly;
         return true;
         }
      }
  return false;
}

size_t cMcpKeys::Count(void) const
{
  std::lock_guard<std::mutex> lock(mutex);
  return keys.size();
}

// --- Certificate -----------------------------------------------------------

static bool AddExt(X509 *Cert, X509V3_CTX *Ctx, int Nid, const std::string &Value)
{
  X509_EXTENSION *ex = X509V3_EXT_conf_nid(NULL, Ctx, Nid, Value.c_str());
  if (!ex)
     return false;
  bool ok = X509_add_ext(Cert, ex, -1) == 1;
  X509_EXTENSION_free(ex);
  return ok;
}

static std::string SubjectAltNames(std::string &CommonName)
{
  std::vector<std::string> names;
  char host[256] = "";
  gethostname(host, sizeof(host) - 1);
  CommonName = *host ? host : "localhost";
  if (*host)
     names.push_back(std::string("DNS:") + host);
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = AI_CANONNAME;
  addrinfo *ai = NULL;
  if (*host && getaddrinfo(host, NULL, &hints, &ai) == 0) {
     if (ai && ai->ai_canonname && strcmp(ai->ai_canonname, host) != 0)
        names.push_back(std::string("DNS:") + ai->ai_canonname);
     freeaddrinfo(ai);
     }
  names.push_back("DNS:localhost");
  names.push_back("IP:127.0.0.1");
  ifaddrs *ifa = NULL;
  if (getifaddrs(&ifa) == 0) {
     for (ifaddrs *i = ifa; i; i = i->ifa_next) {
         if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
         char ip[INET_ADDRSTRLEN];
         inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
         if (strcmp(ip, "127.0.0.1") != 0)
            names.push_back(std::string("IP:") + ip);
         }
     freeifaddrs(ifa);
     }
  std::string san;
  for (auto &n : names)
      san += (san.empty() ? "" : ",") + n;
  return san;
}

bool EnsureCertificate(const std::string &CertFile, const std::string &KeyFile, std::string &Error)
{
  if (access(CertFile.c_str(), R_OK) == 0 && access(KeyFile.c_str(), R_OK) == 0)
     return true;
  EVP_PKEY *pkey = EVP_EC_gen("P-256");
  X509 *x = X509_new();
  bool ok = pkey && x;
  if (ok) {
     std::string cn;
     std::string san = SubjectAltNames(cn);
     X509_set_version(x, 2);
     ASN1_INTEGER_set(X509_get_serialNumber(x), (long)time(NULL));
     X509_gmtime_adj(X509_getm_notBefore(x), -3600);
     X509_gmtime_adj(X509_getm_notAfter(x), 10L * 365 * 24 * 3600);
     X509_set_pubkey(x, pkey);
     X509_NAME *name = X509_get_subject_name(x);
     X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"vdr-plugin-mcp", -1, -1, 0);
     X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn.c_str(), -1, -1, 0);
     X509_set_issuer_name(x, name);
     X509V3_CTX ctx;
     X509V3_set_ctx_nodb(&ctx);
     X509V3_set_ctx(&ctx, x, x, NULL, NULL, 0);
     ok = AddExt(x, &ctx, NID_basic_constraints, "critical,CA:TRUE") &&
          AddExt(x, &ctx, NID_key_usage, "critical,digitalSignature,keyCertSign") &&
          AddExt(x, &ctx, NID_ext_key_usage, "serverAuth") &&
          AddExt(x, &ctx, NID_subject_key_identifier, "hash") &&
          AddExt(x, &ctx, NID_subject_alt_name, san) &&
          X509_sign(x, pkey, EVP_sha256()) > 0;
     if (!ok)
        Error = "cannot build the certificate";
     }
  else
     Error = "cannot create a key";
  if (ok) {
     FILE *f = fopen(KeyFile.c_str(), "w");
     if (f) {
        fchmod(fileno(f), 0600);
        ok = PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL) == 1;
        ok = (fclose(f) == 0) && ok;
        }
     else
        ok = false;
     f = ok ? fopen(CertFile.c_str(), "w") : NULL;
     if (f) {
        fchmod(fileno(f), 0644);
        ok = PEM_write_X509(f, x) == 1;
        ok = (fclose(f) == 0) && ok;
        }
     else
        ok = false;
     if (!ok)
        Error = "cannot write " + KeyFile + " or " + CertFile;
     }
  X509_free(x);
  EVP_PKEY_free(pkey);
  return ok;
}

std::string CertFingerprint(const std::string &CertFile)
{
  FILE *f = fopen(CertFile.c_str(), "r");
  if (!f)
     return "";
  X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
  fclose(f);
  if (!x)
     return "";
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  std::string s;
  if (X509_digest(x, EVP_sha256(), md, &n) == 1) {
     static const char *digits = "0123456789ABCDEF";
     for (unsigned int i = 0; i < n; i++) {
         if (i)
            s += ':';
         s += digits[md[i] >> 4];
         s += digits[md[i] & 15];
         }
     }
  X509_free(x);
  return s;
}
