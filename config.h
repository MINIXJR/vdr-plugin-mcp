/*
 * config.h: settings of the mcp plugin
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_CONFIG_H
#define __MCP_CONFIG_H

#include <cstddef>
#include <string>
#include <vector>

struct cMcpConfig {
  std::string version;
  std::string bind = "127.0.0.1";
  int port = 6420;
  int svdrpPort = 6419;
  std::vector<std::string> deny = { "DEL*", "HITK" };
  size_t maxReply = 262144;
  std::string logUnit;     // systemd unit for the tool vdr_log, empty = tool off
  // HTTP transport (off while httpPort is 0)
  std::string httpBind;
  int httpPort = 0;
  bool httpTls = true;
  std::string certFile;    // empty = own certificate in the config directory
  std::string keyFile;
  std::string configDir;   // /var/lib/vdr/plugins/mcp, set at start
  // what a read only key may use, besides the deny list
  std::vector<std::string> roAllow = { "HELP", "LSTC", "LSTD", "LSTE", "LSTR", "LSTT",
                                       "NEXT", "PING", "STAT", "vdr_log", "vdr_marks" };
  // true if the SVDRP command (of the given plugin, empty for VDR itself) is on the deny list
  bool Denied(const std::string &Plugin, const std::string &Command) const;
  // true if a client with the given role may use the command or tool
  bool Allowed(const std::string &Plugin, const std::string &Command, bool ReadOnly) const;
  };

#endif
