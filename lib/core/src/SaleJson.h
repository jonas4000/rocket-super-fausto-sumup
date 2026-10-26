#pragma once

#include <cstddef>

#include "Ports.h"

namespace grinder {

// Builds the JSON row for the Supabase `coffee_log` table (see supabase/schema.sql),
// e.g. {"dose":"Single","price_cents":50,"refunded":false}. The timestamp is the table's
// created_at default. Dose names are inserted unescaped, so they must not
// contain quotes or backslashes. Returns false if the buffer is too small.
bool formatSaleJson(char* buf, size_t len, const Dose& dose, bool refunded);

}  // namespace grinder
