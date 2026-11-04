/*
 * journal.h: read the systemd journal of VDR for the tool vdr_log
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_JOURNAL_H
#define __MCP_JOURNAL_H

#include <string>

// Returns the last Lines journal entries of the systemd unit Unit, oldest first,
// optionally only those whose message contains Filter (case insensitive).
std::string ReadJournal(const std::string &Unit, int Lines, const std::string &Filter, bool &IsError);

#endif
