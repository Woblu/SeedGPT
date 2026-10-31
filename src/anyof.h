// "any" groups: OR, without changing the AND engine.
//
//   {"version":"1.21","conditions":[
//     {"id":"v","structure":"village","within":400},
//     {"id":"g","any":[{"id":"a","structure":"mansion","within":1500},
//                      {"id":"b","structure":"monument","within":1500}]}]}
//
// Pass 1 and pass 2 are built around early returns: the first condition that
// fails ends the seed. Teaching them to continue would touch every path that
// has been checked against the real game. So a group is compiled away instead:
// the query above IS two ordinary queries, village+mansion and village+monument,
// and a seed matches if either does. Each alternative is planned, ordered and
// evaluated exactly as if it had been written alone, and a hit reports the
// alternative that actually held -- which is also what the reader wants to know.
//
// The limits follow from that:
//   - Groups multiply, so at most MAX_ALT combinations.
//   - Nothing may be measured from a group member, since which one exists
//     differs per alternative.
//   - A leaderboard ranks one condition, so it cannot take a group.
#pragma once

#include "query.h"

#define MAX_ALT 16

// Compile a query into the plain queries it stands for. Returns how many (1 when
// it has no groups; *out is malloc'd). On failure *out is NULL and the return is
// 0 for a bad query or -1 for one the planner refused, with err set either way.
// An alternative the planner alone refuses cannot match, so it is dropped with a
// note in the first alternative's warn, unless that leaves none.
int queryCompile(const char *json, Query **out, char *err, size_t errlen);
