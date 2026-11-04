/*
 * keys.h: API keys of the HTTP transport, TLS certificate
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_KEYS_H
#define __MCP_KEYS_H

#include <mutex>
#include <string>
#include <vector>

struct cMcpKey {
  std::string name;
  bool readOnly = true;
  long created = 0;
  std::string hash;        // SHA-256 of the key, hex
  };

class cMcpKeys {
private:
  std::string fileName;
  std::vector<cMcpKey> keys;
  mutable std::mutex mutex;
  bool Save(void);         // caller holds the mutex
public:
  void SetFileName(const std::string &FileName) { fileName = FileName; }
  bool Load(void);
  // Creates a key and returns it (it is shown only once), empty on error.
  std::string Add(const std::string &Name, bool ReadOnly, std::string &Error);
  bool Del(const std::string &Name);
  std::vector<cMcpKey> List(void) const;
  // Checks a key a client presented.
  bool Check(const std::string &Key, std::string &Name, bool &ReadOnly) const;
  size_t Count(void) const;
  };

extern cMcpKeys McpKeys;

std::string RandomHex(int Bytes);
std::string Sha256Hex(const std::string &Data);

// Creates a self-signed certificate (EC P-256, 10 years) for the host names
// and IPv4 addresses of this machine, unless CertFile already exists.
bool EnsureCertificate(const std::string &CertFile, const std::string &KeyFile, std::string &Error);

// SHA-256 fingerprint of a PEM certificate, "AB:CD:..."
std::string CertFingerprint(const std::string &CertFile);

#endif
