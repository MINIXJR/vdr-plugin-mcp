/*
 * mcp.cpp: a plugin for the Video Disk Recorder
 *
 * Offers the SVDRP commands of VDR and of its plugins as tools of a
 * Model Context Protocol (MCP) server.
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "config.h"
#include "http.h"
#include "keys.h"
#include "recording.h"
#include "server.h"
#include <vdr/plugin.h>
#include <getopt.h>
#include <strings.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

static const char *VERSION     = "0.4.1";
static const char *DESCRIPTION = trNOOP("MCP server for the SVDRP commands");

static std::vector<std::string> SplitList(const char *s)
{
  std::vector<std::string> v;
  std::string cur;
  for (const char *p = s; ; p++) {
      if (*p == ',' || *p == 0) {
         size_t b = cur.find_first_not_of(" \t");
         size_t e = cur.find_last_not_of(" \t");
         if (b != std::string::npos)
            v.push_back(cur.substr(b, e - b + 1));
         cur.clear();
         if (*p == 0)
            break;
         }
      else
         cur += *p;
      }
  return v;
}

// How a client reaches the HTTP server with the given key: the ready line for
// Claude Code, the URL and header for others, the certificate to trust.
static std::string ClientHint(const cMcpConfig &Config, const std::string &Key)
{
  if (Config.httpPort <= 0)
     return "MCP over HTTP(S) is off, switch it on with --http=ADDR:PORT in mcp.conf.";
  char name[256] = "localhost";
  if (gethostname(name, sizeof(name)) != 0)
     strcpy(name, "localhost");
  name[sizeof(name) - 1] = 0;
  std::string host = name;
  std::string server = host.substr(0, host.find('.'));
  const std::string &b = Config.httpBind;
  if (b == "127.0.0.1" || b == "::1" || b == "localhost")
     host = "localhost";
  else if (!b.empty() && b != "0.0.0.0" && b != "::")
     host = b;
  std::string url = std::string(Config.httpTls ? "https" : "http") + "://" + host + ":" +
                    std::to_string(Config.httpPort) + "/mcp";
  std::string header = "Authorization: Bearer " + Key;
  std::string s = "Claude Code:\n"
                  "claude mcp add --transport http " + server + " " + url + " --header \"" + header + "\"\n"
                  "Other MCP clients: URL " + url + ", HTTP header \"" + header + "\".";
  if (Config.httpTls)
     s += "\nThe client must trust the certificate " +
          (Config.certFile.empty() ? Config.configDir + "/cert.pem" : Config.certFile) +
          " (Node.js: NODE_EXTRA_CA_CERTS).";
  return s;
}

class cPluginMcp : public cPlugin {
private:
  cMcpConfig config;
  cMcpServer *server;
  cMcpHttpServer *httpServer;
public:
  cPluginMcp(void) : server(NULL), httpServer(NULL) {}
  virtual ~cPluginMcp() { delete httpServer; delete server; }
  virtual const char *Version(void) { return VERSION; }
  virtual const char *Description(void) { return tr(DESCRIPTION); }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual const char **SVDRPHelpPages(void);
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);
  };

const char *cPluginMcp::CommandLineHelp(void)
{
  return
    "  -b ADDR,   --bind=ADDR        address of the local MCP server (default: 127.0.0.1),\n"
    "                                other addresses are reachable WITHOUT authentication\n"
    "  -p PORT,   --port=PORT        TCP port of the local MCP server (default: 6420)\n"
    "  -s PORT,   --svdrp-port=PORT  SVDRP port of this VDR (default: 6419)\n"
    "  -d LIST,   --deny=LIST        SVDRP commands that are not offered, comma separated;\n"
    "                                a trailing '*' matches any rest, PLUGIN.CMD a single\n"
    "                                plugin command (default: DEL*,HITK), --deny= offers all\n"
    "  -m BYTES,  --max-reply=BYTES  cut longer SVDRP replies (default: 262144)\n"
    "  -l,        --log[=UNIT]       offer the tool vdr_log with the journal of the\n"
    "                                systemd unit UNIT (default: vdr.service); the user\n"
    "                                vdr must be in the group systemd-journal\n"
    "  -w ADDR:PORT, --http=ADDR:PORT  MCP over HTTPS with API keys, e.g. 0.0.0.0:6421\n"
    "                                (default: off), endpoint /mcp\n"
    "  -c FILE,   --cert=FILE        TLS certificate (default: own self-signed one in the\n"
    "                                plugin's config directory, created at the first start)\n"
    "  -k FILE,   --key=FILE         TLS private key belonging to --cert\n"
    "  -n,        --http-plain       HTTP without TLS (only for tests, keys travel unencrypted)\n"
    "  -r LIST,   --ro-allow=LIST    commands and tools read only keys may use, in addition to\n"
    "                                HELP,LSTC,LSTD,LSTE,LSTR,LSTT,NEXT,PING,STAT,vdr_log,vdr_marks\n";
}

bool cPluginMcp::ProcessArgs(int argc, char *argv[])
{
  static const struct option long_options[] = {
    { "bind",       required_argument, NULL, 'b' },
    { "port",       required_argument, NULL, 'p' },
    { "svdrp-port", required_argument, NULL, 's' },
    { "deny",       required_argument, NULL, 'd' },
    { "max-reply",  required_argument, NULL, 'm' },
    { "log",        optional_argument, NULL, 'l' },
    { "http",       required_argument, NULL, 'w' },
    { "cert",       required_argument, NULL, 'c' },
    { "key",        required_argument, NULL, 'k' },
    { "http-plain", no_argument,       NULL, 'n' },
    { "ro-allow",   required_argument, NULL, 'r' },
    { NULL,         0,                 NULL,  0  }
    };
  int c;
  while ((c = getopt_long(argc, argv, "b:p:s:d:m:l::w:c:k:nr:", long_options, NULL)) != -1) {
        switch (c) {
          case 'b': config.bind = optarg; break;
          case 'p': config.port = atoi(optarg); break;
          case 's': config.svdrpPort = atoi(optarg); break;
          case 'd': config.deny = SplitList(optarg); break;
          case 'm': config.maxReply = strtoul(optarg, NULL, 10); break;
          case 'l': config.logUnit = (optarg && *optarg) ? optarg : "vdr.service"; break;
          case 'w': {
                    std::string a = optarg;
                    size_t colon = a.rfind(':');
                    if (colon == std::string::npos) {
                       esyslog("mcp: --http needs ADDR:PORT, e.g. 0.0.0.0:6421");
                       return false;
                       }
                    config.httpBind = a.substr(0, colon);
                    config.httpPort = atoi(a.substr(colon + 1).c_str());
                    }
                    break;
          case 'c': config.certFile = optarg; break;
          case 'k': config.keyFile = optarg; break;
          case 'n': config.httpTls = false; break;
          case 'r': for (auto &s : SplitList(optarg)) config.roAllow.push_back(s); break;
          default:  return false;
          }
        }
  if (config.port <= 0 || config.port > 65535 || config.svdrpPort <= 0 || config.svdrpPort > 65535 ||
      config.httpPort < 0 || config.httpPort > 65535) {
     esyslog("mcp: invalid port");
     return false;
     }
  if (config.certFile.empty() != config.keyFile.empty()) {
     esyslog("mcp: --cert and --key belong together");
     return false;
     }
  if (config.maxReply < 1024)
     config.maxReply = 1024;
  return true;
}

bool cPluginMcp::Start(void)
{
  config.version = VERSION;
  config.configDir = ConfigDirectory(Name());
  McpKeys.SetFileName(config.configDir + "/keys");
  McpKeys.Load();
  if (!config.logUnit.empty())
     isyslog("mcp: tool vdr_log reads the journal of %s", config.logUnit.c_str());
  server = new cMcpServer(config);
  if (!server->Open())
     esyslog("mcp: local server not started");
  if (config.httpPort > 0) {
     httpServer = new cMcpHttpServer(config);
     if (!httpServer->Open())
        esyslog("mcp: HTTP server not started");
     }
  return true; // VDR runs on without the MCP servers
}

void cPluginMcp::Stop(void)
{
  if (httpServer)
     httpServer->Shutdown();
  if (server)
     server->Shutdown();
  StopGenIndex();
}

const char **cPluginMcp::SVDRPHelpPages(void)
{
  static const char *HelpPages[] = {
    "KEYADD <name> ro|rw\n"
    "    Creates an API key for MCP over HTTP(S). ro keys may only list and\n"
    "    show, rw keys may use every command that is not denied. The key is\n"
    "    shown only now, together with the command line for Claude Code;\n"
    "    the plugin stores just its SHA-256 hash.",
    "KEYLIST\n"
    "    Lists the API keys: name, role and creation date.",
    "KEYDEL <name>\n"
    "    Deletes an API key; clients using it are refused at once.",
    NULL
    };
  return HelpPages;
}

cString cPluginMcp::SVDRPCommand(const char *Command, const char *Option, int &ReplyCode)
{
  std::string option = Option ? Option : "";
  std::istringstream is(option);
  std::string name, role;
  is >> name >> role;
  if (strcasecmp(Command, "KEYADD") == 0) {
     if (name.empty() || (role != "ro" && role != "rw")) {
        ReplyCode = 501;
        return "usage: KEYADD <name> ro|rw";
        }
     std::string error;
     std::string key = McpKeys.Add(name, role == "ro", error);
     if (key.empty()) {
        ReplyCode = 550;
        return error.c_str();
        }
     isyslog("mcp: API key '%s' (%s) created", name.c_str(), role.c_str());
     return cString::sprintf("API key '%s' (%s), shown only this once:\n%s\n%s", name.c_str(), role.c_str(),
                             key.c_str(), ClientHint(config, key).c_str());
     }
  if (strcasecmp(Command, "KEYLIST") == 0) {
     auto keys = McpKeys.List();
     if (keys.empty()) {
        ReplyCode = 550;
        return "no API keys, create one with KEYADD <name> ro|rw";
        }
     std::string s;
     for (auto &k : keys) {
         char t[32];
         time_t c = k.created;
         struct tm tm;
         localtime_r(&c, &tm);
         strftime(t, sizeof(t), "%Y-%m-%d %H:%M", &tm);
         s += (s.empty() ? "" : "\n") + k.name + " " + (k.readOnly ? "ro" : "rw") + " " + t;
         }
     return s.c_str();
     }
  if (strcasecmp(Command, "KEYDEL") == 0) {
     if (name.empty()) {
        ReplyCode = 501;
        return "usage: KEYDEL <name>";
        }
     if (!McpKeys.Del(name)) {
        ReplyCode = 550;
        return cString::sprintf("no API key named '%s'", name.c_str());
        }
     isyslog("mcp: API key '%s' deleted", name.c_str());
     return cString::sprintf("API key '%s' deleted", name.c_str());
     }
  return NULL;
}

VDRPLUGINCREATOR(cPluginMcp); // Don't touch this!
