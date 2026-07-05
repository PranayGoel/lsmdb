#pragma once

#include "lsmdb/db.h"

#include <string>

namespace lsmdb::server {

// Parses and executes exactly one protocol line (already stripped of its
// trailing \r\n) against db, returning the full response, \r\n included.
// Kept as a pure function with no dependency on any socket or Asio type,
// specifically so the wire protocol's actual behavior -- PUT/GET/DELETE/PING
// semantics and malformed-input handling -- can be unit tested directly
// against a Db, without spinning up a real TCP connection for every case
// (tests/server/server_test.cpp covers the real-socket path separately).
//
// Grammar (Redis-inspired, not RESP-compatible -- a genuine text protocol
// simple enough to drive by hand from `nc`/`telnet`/PowerShell's
// Test-NetConnection, which is the whole point of choosing this shape over a
// binary or fully RESP-compliant one):
//   PUT <key> <value>   -> +OK          value is everything after the first
//                                       space following key, so it may
//                                       itself contain spaces -- just not
//                                       \r or \n, which terminate the line.
//   GET <key>           -> +<value>     if present
//                       -> $-1          if absent (Redis's classic nil
//                                       sentinel; instantly recognizable to
//                                       anyone who's used redis-cli)
//   DELETE <key>        -> +OK
//   PING                -> +PONG
//   anything else        -> -ERR <message>
//
// A key itself may not contain a space in this protocol -- a documented,
// accepted scope limit for a demo protocol, not a general-purpose one.
//
// `read_only`, when true, rejects PUT and DELETE with an error instead of
// applying them -- how a Tier 3 replica server refuses ordinary client
// writes (all of its data must come from its primary via replication, never
// from a client that happened to connect to the wrong server). GET and PING
// are always allowed regardless. Defaults to false so every existing
// read-write call site is unaffected; a replica's Session passes true.
std::string dispatch(Db& db, const std::string& line, bool read_only = false);

// True if `line`'s command mutates the database (PUT or DELETE). Used by
// Session to decide whether a just-applied command should be forwarded to
// any subscribed replication followers.
bool is_mutating_command(const std::string& line);

}  // namespace lsmdb::server
