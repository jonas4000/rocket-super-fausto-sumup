#pragma once

// Copy to include/secrets.h (gitignored) and fill in; never put real values
// in this file, it is committed. Without secrets.h the
// firmware still builds, but Wi-Fi and Supabase are disabled.

#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"

#define SUMUP_API_KEY "sup_sk_..."
#define SUMUP_MERCHANT_CODE "MXXXXXXX"
#define SUMUP_READER_ID "rdr_..."
// Affiliate key (me.sumup.com > Settings > For Developers > Toolkit > Affiliate
// Keys) and the application identifier it was created for. Required in reader
// checkout requests.
#define SUMUP_AFFILIATE_APP_ID "com.example.grinder"
#define SUMUP_AFFILIATE_KEY "..."

// Supabase project URL (just the base, without /rest/v1) and publishable (or
// legacy anon) key. The key can only
// insert into `coffee_log` thanks to the RLS policy in supabase/schema.sql.
#define SUPABASE_URL "https://your-project.supabase.co"
#define SUPABASE_KEY "sb_publishable_..."

// 4-digit PIN for the Settings page (prices, grind times, Manual run).
#define SETTINGS_PIN "1234"

// Local time zone as a POSIX TZ string (daily shot count, leaderboard dates),
// e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Central Europe. Defaults to UTC.
#define TIME_ZONE "UTC0"
