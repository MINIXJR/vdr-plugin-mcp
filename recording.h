/*
 * recording.h: tools on recordings that SVDRP does not offer
 *              (editing marks, index regeneration)
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_RECORDING_H
#define __MCP_RECORDING_H

#include <string>
#include <vector>

// Lists the editing marks of the recording with the given SVDRP id.
std::string ReadMarks(int Id, bool &IsError);

// Replaces the editing marks (times as h:mm:ss[.ff]), aligned to I-frames.
std::string WriteMarks(int Id, const std::vector<std::string> &Marks, bool &IsError);

// Starts the regeneration of the index file in the background; it also
// counts the errors of the recording. Only one at a time.
std::string StartGenIndex(int Id, bool &IsError);

// Waits for a running index regeneration (plugin stop).
void StopGenIndex(void);

#endif
