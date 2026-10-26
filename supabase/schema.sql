-- Sales log written by the grinder (src/firmware/SupabaseLog.cpp).
-- Run in the Supabase SQL editor; safe to run again on an existing table.

-- Optional: show created_at in your local time instead of UTC (table editor,
-- SQL editor and API). Timestamps stay absolute, so existing rows don't change.
-- Takes effect for new connections (reload the dashboard).
-- alter database postgres set timezone to 'Area/City';

create table if not exists public.coffee_log (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),  -- when the sale reached Supabase
  dose        text        not null                  -- "Single" / "Double"
);

-- Columns the device sends; added here in case the table was created by hand.
alter table public.coffee_log
  add column if not exists price_cents integer not null default 0 check (price_cents >= 0),  -- EUR cents
  add column if not exists refunded boolean not null default false;  -- milestone shot (#25, #50, ...), paid then refunded

-- The device uses the publishable/anon key, which ships in the firmware.
-- Allow it to insert only: no reading, updating or deleting sales.
alter table public.coffee_log enable row level security;

drop policy if exists "grinder can insert sales" on public.coffee_log;
create policy "grinder can insert sales"
  on public.coffee_log for insert
  to anon
  with check (true);
