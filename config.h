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
  // true if the SVDRP command (of the given plugin, empty for VDR itself) is on the deny list
  bool Denied(const std::string &Plugin, const std::string &Command) const;
  };

#endif
