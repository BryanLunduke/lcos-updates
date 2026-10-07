/* Startup-notification timestamps for a second launch.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LCOS_UPDATES_STARTUP_ID_HPP
#define LCOS_UPDATES_STARTUP_ID_HPP

#include <cerrno>
#include <cstdlib>
#include <string>

/* Startup-notification ids end in _TIME<x11 timestamp>. A parsed value of 0
 * is GDK_CURRENT_TIME, not a real event time, and is rejected. */
inline bool startup_timestamp_from_id(const std::string& startup_id, unsigned long& timestamp)
{
  const std::string::size_type pos = startup_id.rfind("_TIME");
  if (pos == std::string::npos)
    return false;
  const char* timestr = startup_id.c_str() + pos + 5;
  if (*timestr == '\0')
    return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long value = std::strtoul(timestr, &end, 0);
  if (end == timestr || *end != '\0' || errno != 0)
    return false;
  if (value == 0 || value > 0xFFFFFFFFUL)
    return false;
  timestamp = value;
  return true;
}

#endif
