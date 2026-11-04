/*
 * mcp.cpp: a plugin for the Video Disk Recorder
 *
 * Offers the SVDRP commands of VDR and of its plugins as tools of a
 * Model Context Protocol (MCP) server.
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "config.h"
#include "server.h"
#include <vdr/plugin.h>
#include <getopt.h>
#include <cstdlib>

static const char *VERSION     = "0.1.0";
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

class cPluginMcp : public cPlugin {
private:
  cMcpConfig config;
  cMcpServer *server;
public:
  cPluginMcp(void) : server(NULL) {}
  virtual ~cPluginMcp() { delete server; }
  virtual const char *Version(void) { return VERSION; }
  virtual const char *Description(void) { return tr(DESCRIPTION); }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Start(void);
  virtual void Stop(void);
  };

const char *cPluginMcp::CommandLineHelp(void)
{
  return
    "  -b ADDR,   --bind=ADDR        address to listen on (default: 127.0.0.1),\n"
    "                                other addresses are reachable WITHOUT authentication\n"
    "  -p PORT,   --port=PORT        TCP port of the MCP server (default: 6420)\n"
    "  -s PORT,   --svdrp-port=PORT  SVDRP port of this VDR (default: 6419)\n"
    "  -d LIST,   --deny=LIST        SVDRP commands that are not offered, comma separated;\n"
    "                                a trailing '*' matches any rest, PLUGIN.CMD a single\n"
    "                                plugin command (default: DEL*), --deny= offers all\n"
    "  -m BYTES,  --max-reply=BYTES  cut longer SVDRP replies (default: 262144)\n";
}

bool cPluginMcp::ProcessArgs(int argc, char *argv[])
{
  static const struct option long_options[] = {
    { "bind",       required_argument, NULL, 'b' },
    { "port",       required_argument, NULL, 'p' },
    { "svdrp-port", required_argument, NULL, 's' },
    { "deny",       required_argument, NULL, 'd' },
    { "max-reply",  required_argument, NULL, 'm' },
    { NULL,         0,                 NULL,  0  }
    };
  int c;
  while ((c = getopt_long(argc, argv, "b:p:s:d:m:", long_options, NULL)) != -1) {
        switch (c) {
          case 'b': config.bind = optarg; break;
          case 'p': config.port = atoi(optarg); break;
          case 's': config.svdrpPort = atoi(optarg); break;
          case 'd': config.deny = SplitList(optarg); break;
          case 'm': config.maxReply = strtoul(optarg, NULL, 10); break;
          default:  return false;
          }
        }
  if (config.port <= 0 || config.port > 65535 || config.svdrpPort <= 0 || config.svdrpPort > 65535) {
     esyslog("mcp: invalid port");
     return false;
     }
  if (config.maxReply < 1024)
     config.maxReply = 1024;
  return true;
}

bool cPluginMcp::Start(void)
{
  config.version = VERSION;
  server = new cMcpServer(config);
  if (!server->Open())
     esyslog("mcp: server not started");
  return true; // VDR runs on without the MCP server
}

void cPluginMcp::Stop(void)
{
  if (server)
     server->Shutdown();
}

VDRPLUGINCREATOR(cPluginMcp); // Don't touch this!
