# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) Azeroth Core Module

## mod-auctionator

[![core-build](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/core-build.yml/badge.svg)](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/core-build.yml)
[![codestyle](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/codestyle.yml/badge.svg)](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/codestyle.yml)

> The badge URLs above point at this fork. If you push the module to another
> repository, replace `bluegene-ai/mod-auctionator` with your own `owner/repo`.

## Compatibility

This branch targets **[azerothcore/azerothcore-wotlk](https://github.com/azerothcore/azerothcore-wotlk) `master` only**.

* CI checks out upstream `master`, drops this module into `modules/` and builds it
  with the official AzerothCore reusable workflow
  (`azerothcore/reusable-workflows`) — NOPCH, clang, `-Wall -Wextra -Werror`,
  plus cppcheck over `modules/`. Green CI means "compiles and passes cppcheck
  against upstream master".
* Nothing else is supported: no core forks, no NPCBots or other patches, no
  release-branch backports. If upstream master changes an API the module uses,
  the build breaks on purpose and gets fixed here.
* Because CI builds **without precompiled headers**, every source file includes
  what it uses. Keep it that way when adding code.

<p align="left">
  <img src="https://github.com/araxiaonline/docs/blob/main/docs/media/logo-sm.png?raw=true" alt="Araxia Online" width="70" style="vertical-align: middle;"/>
  <span style="font-size: 20px; vertical-align: middle;" >Originally developed by Araxia Online</span>
</p>

## Description

This mod is meant to keep a healthy auction house stocked on a low-pop server. It's in it's early phases of building/testing/configuration but keeps a LOT of stuff in the AH. Code is also a little rough right now but I haven't done c++ in 20+ years so I am getting there.

## Installation and Setup

1. Download the code into your source code `modules/` folder.
2. Build the core with the module in place (`-DMODULES=static`), then deploy the new
   `worldserver` binaries.
3. Make sure the **server's `SourceDirectory` points at the source tree that contains
   `modules/mod-auctionator/data/sql/`** (leave it empty to use the compile-time source
   path). The AzerothCore updater applies the module's SQL from
   `<SourceDirectory>/modules/<module>/data/sql/`; if that path is not reachable the
   module's tables are never created, the seller finds no candidate items and the market
   import fails.
   If you prefer to do it by hand, run these against the matching database:
   * world database: `data/sql/db-world/base/2023-09-18.sql`,
     `data/sql/db-world/base/mod_auctionator_gm_list.sql` and
     `data/sql/db-world/base/mod_auctionator_quality_config.sql`
   * characters database, in this order:
     `data/sql/db-characters/updates/2023_11_12_00_marketprice.sql`,
     `data/sql/db-characters/updates/2026_09_20_00_market_price_history.sql`,
     `data/sql/db-characters/updates/2026_01_29_00_improve_performance.sql`,
     `data/sql/db-characters/updates/2026_09_27_00_sale_log.sql`
   Verify with `SHOW TABLES LIKE 'mod_auctionator%';` in both databases.
4. **Create the module configuration.** The build ships
   `modules/mod-auctionator/conf/mod_auctionator.conf.dist`; the server only reads
   `configs/modules/mod_auctionator.conf` (no `.dist`), and Windows builds do not copy it
   for you:
   ```
   copy modules\mod-auctionator\conf\mod_auctionator.conf.dist configs\modules\mod_auctionator.conf
   ```
   Without that file every option falls back to its code default, and
   `Auctionator.Enabled` defaults to **0**, so the module stays silent.
5. Optionally add the module's logging lines to `worldserver.conf` (they do **not** work
   in the module config, which is loaded after the log system is initialised):
   ```
   Appender.AuctionatorLog=2,5,0,auctionator.log,w
   Appender.AuctionatorConsole=1,4,0,"1 9 3 6 5 8"
   Logger.auctionator=4,AuctionatorLog AuctionatorConsole
   ```
6. If you want a fresh start, clear the auction house with these SQL commands. Do this
   ONLY when your server is stopped. The AH is cached in memory and doing this while the
   server is running will not end well. Run this against your character database.

> The module itself never hardcodes a schema name: the seller and market queries
> resolve the world and characters databases at runtime from `worldserver.conf`
> (`WorldDatabaseInfo` / `CharacterDatabaseInfo`), so non-default names such as
> `acore_world80` / `acore_characters80` work out of the box.
>
> Because those queries span both schemas from one connection, the **world** database
> user needs `SELECT` on the characters schema (the default AzerothCore setup uses the
> same user for every database and already satisfies this). Without the grant the
> seller's item query fails with `Table ... doesn't exist` in the SQL log and the
> seller logs "seller item query produced no rows".

### Several realms on one machine (sharing one auth database)

The module has no realm-wide singleton: a machine that runs several realms runs one
`worldserver` per realm, each with its own worldserver directory, its own
`configs/modules/mod_auctionator.conf`, its own `mod_auctionator*` tables (they live in
that realm's world/characters databases) and its own bot character
(`Auctionator.CharacterId` / `CharacterGuid` are per realm, and each realm needs its own
dedicated, never-played character). Nothing in the module is keyed by realm id, so no
extra configuration is required — but every realm needs the SQL from step 3 applied to
*its* databases and the conf copied into *its* directory.

Start and stop them independently:

* per realm, at runtime: `.auctionator stop` / `.auctionator start` on that realm's
  console (or through the AGMP panel's auction page, which writes that realm's
  `Auctionator.Enabled` and sends the command in one step);
* persistently: `Auctionator.Enabled` in that realm's
  `configs/modules/mod_auctionator.conf`;
* per house/role: `.auctionator enable|disable <target>`.

Each realm keeps its own event schedule, so stopping one realm's bot never touches
another's; auctions already listed in a stopped realm stay in its auction house.


```
DELETE FROM `item_instance` WHERE `guid` IN (SELECT `itemguid` FROM `auctionhouse`);
DELETE FROM `auctionhouse`;
```

## GM commands

`.auctionator` on its own (or `.auctionator help`) prints the command list, and an
unknown subcommand answers with an explicit error instead of doing nothing.

### auctionator add <house> <item[,item...]> <price> [stack] [hours] [owner]

### auctionator add <house> <item[,item...]> mode=<buyout|bid> [bid=<copper>] [buyout=<copper>] [stack=<n>] [hours=<n>] [owner=<bot|me|guid>]

ItemID can be looked up in the database table `item_template`. The two forms are
**never mixed** inside one command: a number in the price position selects the positional
form, a `key=value` token there selects the option form.

**Every price is per single item**: the listing price is the unit price times the `stack`,
capped at the maximum money amount. That holds for both forms.

Positional form (the original one, unchanged):

* `house` - `2` = alliance, `6` = horde, `7` = neutral
* `item` - one id, or several separated by commas (max 200 per command)
* `price` - **unit** price in copper; the buyout becomes `price * stack`
* `stack` - optional, default 1, capped by the item's max stack
* `hours` - optional, default 48, range 1..720
* `owner` - optional: `bot` (default), `me`, or a character guid

Option form (states this one listing's mode and prices explicitly, **overriding**
`Auctionator.Seller.BidOnly` for this command only):

* `mode=buyout` - **one fixed price**: only `buyout` is needed, and the start bid is
  pinned to the buyout so nobody can win the stack with a low bid.
* `mode=bid` - **auction**: `bid` (the unit start bid) is required and `buyout` is
  optional; when given it may not be below the start bid.
* `mode` may be left out and is then inferred from `bid=` / `buyout=`.
* `stack=` / `hours=` / `owner=` mean the same as in the positional form.

Neither form accepts a start bid of 0, for the reason below.

**Start bid.** A listing with a start bid of 0 can be taken for 1 copper: the core only
rejects a bid below `auction->startbid`, and 1 copper is never below 0. The positional
form therefore derives the start bid from the buyout with the same rule as the automatic
seller - `buyout * (1 - Auctionator.Seller.BidStartModifier)`, never below 1 - so with the
shipped `0.3` a 10000 copper listing starts bidding at 7000. The option form puts the
start bid in the GM's hands instead: `mode=bid` uses `bid` as given, and `mode=buyout`
pins it to the buyout, which is what setting `BidStartModifier = 0` used to achieve.

**Bid-only listings.** With `Auctionator.Seller.BidOnly = 1` the module creates no buyout at
all (the automatic seller and both GM commands): the price above becomes the *start bid*
(`<price> * stack` for `.auctionator add`) and the entry can only be won by bidding. The
core treats buyout 0 as "no buyout", which is exactly what a player listing with an empty
buyout sends, so no core change is involved. `BidStartModifier` is ignored in this mode.
A bid-only entry that nobody bids on still expires and its item is recycled like any other
unsold listing. `BidOnly` now only governs the **automatic seller** and the **positional**
`.auctionator add` form: the option form and any `mod_auctionator_gm_list` row whose `mode`
is not `legacy` override it, so listing a single item as an auction no longer means changing
a realm-wide setting.

**Gold handling.** With the default owner (`bot`, i.e. the configured
`Auctionator.CharacterGuid`) the sale money is mailed to the auctionator
character and recycled by the mail script: the gold leaves the economy. The
listing's *item* is recycled the same way if nobody buys it, so an unsold
listing does not leave a dead mail behind either. Pass `me` or a character guid
if you want the money to reach a real character instead.

The item is created fresh from `item_template`, so any item can be listed,
including bind-on-pickup, quest and unique items. The command warns you when an
item is quest/unique, because those checks are bypassed.

Add an Iridescent Pearl to the Neutral AH for 1 gold buyout:

```
.auctionator add 7 5500 10000
```

List 20 Copper Bolts in the Horde AH at 5 silver each, for 12 hours, and let the
gold be recycled:

```
.auctionator add 6 4359 500 20 12
```

List a bind-on-pickup epic for yourself, 3 day duration, money to your character:

```
.auctionator add 7 19019 500000 1 72 me
```

List the pearl at an explicit fixed 1 gold price that cannot be underbid, gold recycled:

```
.auctionator add 7 5500 mode=buyout buyout=10000
```

Auction 20 Copper Bolts with a 5 silver start bid and a 1 gold buyout, for 12 hours:

```
.auctionator add 6 4359 mode=bid bid=500 buyout=10000 stack=20 hours=12 owner=bot
```

A pure auction (no buyout at all) starting at 5 silver:

```
.auctionator add 7 4359 mode=bid bid=500
```

### auctionator addlist [house] [owner]

Lists every enabled row of `mod_auctionator_gm_list` (world database) so a
curated set of special items can be restocked with one command. The optional
`house` and `owner` arguments override the per-row columns.

How a row is priced comes from its own `mode` column:

* `legacy` - the **default**, follows the realm-wide `Auctionator.Seller.BidOnly` /
  `BidStartModifier` pair, i.e. the behaviour every row had before the column existed.
  Upgrading therefore never changes what an existing row means.
* `buyout` - one fixed price: `price` is the unit buyout and the start bid is pinned to it.
* `bid` - auction: `bid` is the unit start bid (required) and `price` is the optional unit
  buyout (0 = none), which may not be below the start bid.

`price` and `bid` are both **unit** prices in copper (per single item), so the listing
price is the unit price times `stack`. See
`data/sql/db-world/base/mod_auctionator_gm_list.sql` for example rows.

```
.auctionator addlist          # use each row's own house/owner
.auctionator addlist 7        # force the neutral house
.auctionator addlist 7 me     # neutral house, money goes to you
```

### auctionator auctionspercycle <value>

Set the number of auctions added per cycle to each auction house (shared by all
houses). The seller walks the whole candidate set and lists this many of them, so
there is no longer a separate query limit to keep in sync.

This has a direct impact on how hard each cycle hits your worldserver and your
mysql server, so on smaller hardware tune this down.

```
.auctionator auctionspercycle 45
```

### auctionator bidonown <value>

Allows the bidder to bid on the auctionator's own auctions. Only useful for
testing the buyer. Note that the bidder in general is a gold *faucet* (it pays
sellers without paying itself), see "Economy and safety guarantees" below.

Valid values are `1` to enable and `0` to disable.

```
.auctionator bidonown 1
```

### auctionator buyout <0|1>

The quick switch for the shape of every listing the module creates, and the command behind
the management panel's buyout toggle:

* `1` - listings carry a buyout again (`Auctionator.Seller.BidOnly = 0`), and the starting
  bid is derived from `Auctionator.Seller.BidStartModifier` as usual.
* `0` - **no buyout at all** (`Auctionator.Seller.BidOnly = 1`): the price the seller
  computed becomes the *start bid* and the entry can only be won by bidding. An entry
  nobody bids on still expires and is recycled.

It applies to the whole realm at once - the automatic seller, the positional
`.auctionator add` form and every `mod_auctionator_gm_list` row that does not name a mode -
and takes effect on the next seller run: the flag is read for every listing it creates, so
no restart is involved. Only the running configuration changes; the panel's switch also
writes `Auctionator.Seller.BidOnly` into the option file so the choice survives a restart.

```
.auctionator buyout 0    # bidding only
.auctionator buyout 1    # buyout mode back on
```

### auctionator maxitemlevel <value|off>

The item level limit of the **automatic seller**, and the command behind the management
panel's item filter control. `off` (or `0`) removes the limit.

A **recipe** (item class 9) is judged by the level of the item it *makes*, never by its own
`item_template.ItemLevel`, which is a placeholder on recipes. The module follows the recipe's
taught spell (`item_template.spells` with `spelltrigger = 6`) into Spell.dbc and compares the
level of the item that spell creates (`SPELL_EFFECT_CREATE_ITEM`): `Plans: Saronite
Swordbreakers` (47623, ItemLevel 85) is refused once the limit is below 245, the level of the
bracers it teaches (47570).

A recipe whose crafted item cannot be resolved - enchanting formulas, inert tomes and books -
is never listed, whatever the limit is. GM listings are unaffected. The value is read with the
candidate pool, so it applies on the next seller run without a restart.

```
.auctionator maxitemlevel 200    # nothing above item level 200 is auto-listed
.auctionator maxitemlevel off    # no limit again
```

### auctionator disable <target>

Disable the seller or bidder for a particular faction. `<target>` is one of
`hordeseller`, `allianceseller`, `neutralseller`, `hordebidder`,
`alliancebidder`, `neutralbidder` or `all`.

The change is applied to the running schedule immediately: no restart is needed
(only the master switch `Auctionator.Enabled` requires one — or use
`start` / `stop` below).

```
.auctionator disable hordeseller
```

### auctionator enable <target>

Enable the seller or bidder for a particular faction (same targets as
`disable`). The first run of a newly enabled event happens about a minute later,
then the configured cycle time takes over.

```
.auctionator enable hordeseller
```

### auctionator start | on

Turn the module's master switch **on at runtime**. `Auctionator.Enabled` is read
once, while the worldserver builds the Auctionator singleton, so without this a
realm whose option file says `Auctionator.Enabled = 0` could only be started by
restarting its worldserver. `start` flips the switch in place and re-arms the
event schedule from the current per-house flags (first run about a minute later).

This is a **runtime** switch: it is not written back to
`configs/modules/mod_auctionator.conf`. Set `Auctionator.Enabled = 1` there to
keep it across restarts — the AGMP panel's "start this realm's bot" button does
both in one step.

```
.auctionator start
```

### auctionator stop | off

Turn the master switch **off at runtime**: `Update()` stops feeding the event
schedule and the pending timers are dropped, so a long stop cannot end in a burst
of overdue runs. Auctions already listed are untouched, and the per-house flags
are preserved, so `start` resumes exactly what was enabled before.

```
.auctionator stop
```

### auctionator expireall <house> [all]

Expire the auctionator's own auctions for the specified house on the next tick.
Player auctions are left alone unless you pass `all` explicitly (the core then
returns their items and refunds their bidders, nothing is destroyed). Listings that were
created with an explicit owner (`.auctionator add ... me` or a character guid) count as
somebody else's and need `all` as well.

```
.auctionator expireall 7
.auctionator expireall 7 all
```

### auctionator multiplier <typetext> <qualitytext> <multiplier>

Typetext is either `seller` or `bidder` depending on which you want to set.

Set the sell and buy multiplier for a quality. Qualities are:

* `poor`
* `normal`
* `uncommon`
* `rare`
* `epic`
* `legendary`

Multiplier is a decimal value. Defaults are set in the config file.

For the **seller** multiplier, note that it scales the *fallback* price only
(`item_template.BuyPrice`, or `Auctionator.Seller.DefaultPrice` when the item has
no vendor price either). An imported market price is never multiplied, otherwise
market driven pricing would not be market driven.

A **bidder** multiplier below `1.0` is honoured as configured and lowers what the bidder
is willing to pay (it is not silently raised to 1.0); `0.0` means the bidder buys nothing
and the only hard limit is a floor of 1 copper. The same applies to the seller multiplier
on the fallback price.

```
.auctionator multiplier bidder epic 10
```

### auctionator status

Shows the status of the in memory configs as well as the current auction
houses. Useful for checking config values that may have changed or checking
on auction counts. Example output below.

```
.auctionator status
```

Output:
```
[Auctionator] Status:

 Enabled: 1

 CharacterGuid: 2
 Horde:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 9319
    Bidder Enabled: 1
        Cycle Time: 2
        Per Cycle: 20
 Alliance:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 8959
    Bidder Enabled: 1
        Cycle Time: 1
        Per Cycle: 20
 Neutral:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 9379
    Bidder Enabled: 1
        Cycle Time: 3
        Per Cycle: 30
 Seller Multipliers:
    Poor: 1.000000
    Normal: 1.000000
    Uncommon: 1.500000
    Rare: 2.000000
    Epic: 6.000000
    Legendary: 10.000000
 Bidder Multipliers:
    Poor: 1.000000
    Normal: 1.000000
    Uncommon: 1.500000
    Rare: 2.000000
    Epic: 6.000000
    Legendary: 10.000000
 Seller settings:
    Auctions per run: 100
    Default Price (no vendor/market price): 1000000
    Randomize Stack Size: 1
    Bid Start Modifier: 0.300000
    Market data max age (days, seller+bidder, 0 = never): 14
    Prefer market items: 1
    Exclude VerifiedBuild = 1 items: 0
    Min price modifier (x SellPrice): 1.000000
    Max price modifier (x market avg): 2.000000
```

## Shared faction houses (AllowTwoSide.Interaction.Auction = 1)

With this core option the three auction houses are one object, the core stores every auction
with `houseId = Neutral` (`WorldSession::HandleAuctionSellItem` forces it) and clients only
ever search the neutral house (`AuctionHouseSearcher` picks its partition from
`AuctionEntry::GetFactionId()`, i.e. from `houseId`). An Alliance or Horde entry would
therefore be an auction that **no client can see or bid on**, while still occupying that
house's quota and inflating `.auctionator status`.

The module refuses to create such an entry at every level:

* `.auctionator add 2 ...` / `add 6 ...` answers with an error, and `addlist` skips rows
  whose `house` column (or the `house` override) is 2 or 6;
* `Auctionator::CreateAuction()` rejects it as a second line of defence, so no caller can
  create an invisible auction;
* the `AllianceSeller`/`HordeSeller` and `AllianceBidder`/`HordeBidder` events log one error
  per cycle and do nothing (every *player* auction lives in the neutral house, so a
  non-neutral bidder would not find any of them anyway).

On such a realm enable `Auctionator.NeutralSeller` and `Auctionator.NeutralBidder` only.
`.auctionator status` prints a reminder, and `.auctionator expireall 2|6` still works, which
is what you want for rows left over from before the option was switched on.

## Market data import

The seller prices from `mod_auctionator_market_price` (characters database). The
table now keeps history: the primary key is `(entry, scan_datetime)`, so imports
upsert per item and scan, a partial export never touches the items it does not
mention, and you can prune old scans when you want. A `source` column records
which feed a row came from.

### Option A - the server imports a CSV on a timer (no external tools)

```
Auctionator.MarketData.ImportFile = "C:/acore/marketprice.csv"
Auctionator.MarketData.ImportSource = tsm
Auctionator.MarketData.ImportIntervalMinutes = 360
```

The worldserver reads that file about one minute after startup and then every
interval. An unchanged file (same size and mtime) is skipped, so the interval can
be short. Columns, with an optional header line:

```
scan_datetime,item_entry,avg_price,minimum_buyout,minimum_bid,item_count
```

### Option B - the helper script (needs Node 18+ and the mysql client)

```
cd apps/marketprice

# only need to do this the first time
npm install

node index.js mypricedata.csv --source=tsm | mysql -u <dbuser> -p <character_database_name>
```

The generated SQL is idempotent (batched `INSERT ... ON DUPLICATE KEY UPDATE`
inside a transaction), so re-importing the same export updates instead of failing.
Progress goes to stderr, the SQL to stdout.

`minimum_buyout` and `minimum_bid` are not used; `item_count` (volume) is used by
the seller's market weighting.

### Option C - price the realm's own auction house (no export at all)

```
.auctionator marketscan
```

or, to have it run by itself:

```
Auctionator.MarketData.ScanIntervalMinutes = 60
Auctionator.MarketData.ScanExcludeSelf = 1
```

This aggregates the live listings of *this* realm's auction house
(`characters.auctionhouse` + `item_instance`) into the market table with a single SQL
statement - no CSV, no external tool, no cron. For each item it writes the
volume-weighted **per-unit** price (the house stores whole-stack prices, so they are
divided by the stack size), plus the cheapest unit buyout/start bid and the number of
listings it sampled, as `source = 'ah-scan'`.

* A listing's **buyout** is preferred. Only items whose listings carry no buyout at all
  fall back to their **start bid**, so a realm running bid-only listings still gets
  prices while items that do have buyouts are not dragged down by reserves.
* `ScanExcludeSelf = 1` (the default) leaves the **auctionator character's own listings**
  out of the sample. Otherwise the "market" price would just mirror the bot's own asking
  price, and because a market-priced listing ignores the quality multipliers, your own
  stock would freeze your last price in for `MaxAgeDays`. On a realm whose house holds
  nothing but the bot's stock the scan therefore writes 0 rows - set it to `0` to sample
  those too, and the command tells you when the owner split is the reason.
* The command reports rows written, listings sampled and how many belonged to the
  auctionator, so an empty result is explained instead of mysterious.
* `ScanIntervalMinutes = 0` (the default) means "only when asked": the command and the
  management panel's button still work, the timer simply never fires. The first timed
  scan runs about a minute after startup.

Because every run adds one row per item, prune the history periodically
(`.auctionator marketprune 30`) when the interval is short.

### Operating it

```
.auctionator market              # rows, distinct items, how many items have a usable scan
.auctionator marketimport        # import Auctionator.MarketData.ImportFile now
.auctionator marketimport force  # import even if the file did not change
.auctionator marketscan          # price this realm's own auction house now
.auctionator marketprune 30      # drop scans older than 30 days
```

`marketimport` reports "nothing was imported" and logs an error when every line of the file
was rejected (wrong date format, non-numeric or zero item entry/price): a broken export must
not look like an empty one. An empty file is still a successful no-op.

A scan older than `Auctionator.MarketData.MaxAgeDays` is ignored by both the
seller (falls back to `item_template.BuyPrice`) and the bidder, so a broken import
degrades to vendor pricing instead of listing nonsense. Run `marketprune`
regularly (or from your own cron) to keep the history table small.

`scan_datetime` is interpreted in the **database server's time zone**: the age of a
scan is computed by the database itself (`TIMESTAMPDIFF(..., NOW())`), so the importer
and the readers only agree when the CSV carries local time of the database host. An
export in UTC on a `+08:00` host looks eight hours older than it is.

Note: the timed import only runs while the module is enabled
(`Auctionator.Enabled = 1`). Use `.auctionator marketimport` if you need to import
while it is disabled.

## Sale log

The core keeps no record of a **finished** auction: `characters.auctionhouse` holds the
live listings only (the row is deleted in the same transaction that pays the seller) and
the core's own `log_money` row is written for sales of 500 gold and up only. A listing the
module created therefore left no trace at all once a player won it - and that matters
here, because the sale gold is swallowed by the mail script (gold sink) and the sale mail
itself is deleted instead of sent. "Who bought which of the bot's items, and for how
much" was unanswerable.

`mod_auctionator_sale` (characters database, `2026_09_27_00_sale_log.sql`) is that record.
The module writes **one row per sold listing it created** - the automatic seller and the
GM `add` / `addlist` commands - from its "auction successful" hook, which covers both ways
a listing can sell (a buyout, and a winning bid when it expires):

| Column | Meaning |
|---|---|
| `auction_id` | the `auctionhouse.id` the sale came from (that row is gone by now) |
| `item_entry`, `item_count` | `item_template.entry` and how many were in the stack |
| `house_id` | `2` alliance, `6` horde, `7` neutral |
| `seller_guid` | the listing's owner |
| `seller_is_bot` | `1` when the owner is `Auctionator.CharacterGuid`, i.e. the sale gold left the economy; `0` for a listing with an explicit `owner=` |
| `buyer_guid` | the winner the core settled with |
| `price` | what the winner paid for the whole stack, in copper |
| `startbid`, `buyout`, `deposit` | the listing's prices at the time of the sale |
| `cut` | the auction house cut the core took out of the price |
| `is_buyout` | `1` = bought out, `0` = won by bidding (`price` is the winning bid) |
| `sold_at` | server local time, the same clock as `auctionhouse.time` |

Notes:

* **Player-to-player auctions are not recorded.** Only listings the module created
  (`deposit = 0` - it never charges one, see the economy section) and listings owned by
  the configured auctionator character are written.
* **A listing that expires unsold is not recorded**: it is not a sale. The item is simply
  recycled with its mail.
* The table is append-only; the module never updates or deletes rows. Prune it from your
  own cron when it grows, e.g.
  `DELETE FROM mod_auctionator_sale WHERE sold_at < NOW() - INTERVAL 180 DAY;`
* A realm that has not applied the SQL update gets one error line in the worldserver log
  ("`mod_auctionator_sale` is missing") and keeps working; it just records nothing.
* The management panel (Acore GM Panel) reads this table for its "sale log" card when that
  panel version is deployed, so a fresh record shows up there without any further setup.

## Pricing in one picture

For every listed item the seller picks one of three sources, in this order:

| Source | Used when | Scaled by |
|---|---|---|
| `mod_auctionator_market_price.average_price` | a scan exists and is newer than `MarketData.MaxAgeDays` | **nothing** (imported as-is) |
| `item_template.BuyPrice` | no usable market data | the per quality `Multipliers.Seller.*` |
| `Auctionator.Seller.DefaultPrice` (default 100 gold) | no market data **and** no vendor price | the per quality `Multipliers.Seller.*` |

The result is then clamped into `[SellPrice x MinPriceModifier, marketPrice x
MaxPriceModifier]` (the ceiling only applies when the price came from the market) and
multiplied by the stack size, capped at `MAX_MONEY_AMOUNT`. The ceiling is applied
**before** the floor, so the floor always wins: even a market average far below the vendor
sell price cannot make the bot list below what a vendor pays (which a player could
otherwise buy up and vendor at a profit). The starting bid is a random value between
`(1 - BidStartModifier) x buyout` and the buyout, and is never 0 - a start bid of 0 would
let a player take the auction for 1 copper.

With `Auctionator.Seller.BidOnly = 1` the buyout is dropped entirely instead: the price
computed above becomes the start bid and the listing can only be won by bidding (see the
"Bid-only listings" note under the GM commands).

## Item selection

Which items may be listed, and in what shape, is data driven (world database):

| Table | Columns | Meaning |
|---|---|---|
| `mod_auctionator_itemclass_config` | `class`, `subclass`, `bonding`, `max_count`, `stack_count` | one row per item class/subclass |
| `mod_auctionator_disabled_items` | `item` | flat blacklist of `item_template.entry` |
| `mod_auctionator_quality_config` | `quality`, `enabled` | one row per `item_template.quality` (0 poor .. 7 heirloom) |

* `bonding` - minimum `item_template.bonding` for the row to match; `0` means "no
  additional constraint". Items with `bonding = 1` (bind on pickup) are always excluded.
  The shipped rows only use 0 and 1, so the column currently acts as a flag for
  "unbound items are allowed / not allowed" in that class.
* `max_count` - quota per item entry: the seller skips an item while it already has
  `max_count` auctions in that house. **`max_count = 0` means "never list this
  class/subclass"** (it is a quota, not "unlimited").
* `stack_count` - how many items go into one listing (weapons/armour `1`, trade
  goods and potions `20`, elixirs `10`, ...). It is clamped by the item's own max
  stack; a row with `0` falls back to that max stack (the column is `NOT NULL DEFAULT 1`,
  so `NULL` cannot occur). With
  `Auctionator.Seller.RandomizeStackSize = 1` the listing uses a random value
  between 1 and `stack_count` instead of the full value.
* `enabled` - the per-quality gate. `0` means that quality is **never** auto-listed;
  `1`, or **no row at all**, means it may be. The "no row is allowed" rule keeps an
  empty table (and a world database that has not run the update yet) listing exactly
  what it used to, so the gate only ever removes qualities the GM turned off. It is
  independent of the class whitelist: an item must match a class row *and* not sit on a
  disabled quality.

`mod_auctionator_itemclass_config` is a **whitelist**, not a blacklist: the seller's
candidate query inner-joins it, so a class/subclass pair without a row is never listed.
That is why a type is added by creating its rows (the panel's "apply to whole class" does
this from `mod_auctionator_item_class`) and removed by setting `max_count = 0`.

`item_template.VerifiedBuild = 1` rows are **not** filtered out by default: in most
world databases that flag means "not verified by the content team", and skipping those
rows removes a large part of the eligible catalogue (~30% on a stock WotLK world
database). Set `Auctionator.Seller.ExcludeUnverifiedItems = 1` if your data means
something else by it.

`Auctionator.Seller.MaxItemLevel` caps the level the automatic seller may list (`0` = no
limit); see `.auctionator maxitemlevel` above for how a recipe is judged.

All three tables are read on every seller cycle, so edits apply on the next run without a
restart.

## Economy and safety guarantees

These are invariants of the module, not side effects:

1. **Gold from bot and GM listings is recycled by the system.** When a bot/GM
   listing sells, the core mails the proceeds to the auctionator character, and
   the mail script deletes that mail (and any items attached to it) before it is
   sent. The gold leaves the economy. **Unsold listings are recycled too**: the
   core's `SendAuctionExpiredMail()` attaches the unsold item to the expiry mail,
   and because the character is never logged in - and the core only ever clears a
   mailbox when its owner logs in - the script drops that item as well. Without
   this every unsold listing leaked one dead mail plus its `item_instance` row for
   good, and once the 100 mail cap was reached the core started silently dropping
   the next expiry. Passing an explicit `owner` to `.auctionator add` is the only
   way to send the money to a character instead; the command reply labels such a
   listing `GOLD SINK BYPASSED`.
2. **No deposit is ever paid out.** The module never charges a deposit, so
   mod-created auctions are created with `deposit = 0`; otherwise the core's
   "bid + deposit - cut" payout would mint gold.
3. **Player auctions are never damaged.**
   * `.auctionator expireall <house>` only expires auctions owned by the
     auctionator character; player auctions are untouched unless you pass `all`.
   * The item that the bot buys out is sunk, but the seller is paid in the same
     transaction, and the previous bidder is refunded through the core's
     `SendAuctionOutbiddedMail` before the bid is taken over, so no escrowed
     player gold is ever destroyed.
   * The bidder never places a *bid* on an auction that already has one (it skips it),
     so a player's bid is never overwritten. It can, however, **buy out** an auction a
     player has already bid on: the previous bidder is refunded through the core's
     `SendAuctionOutbiddedMail` first (see 6 for what that refund means), and the
     seller is paid in the same transaction.
4. **Mail recycling stops automatically if a human is playing the configured
   auctionator character.** The mail hook checks whether the receiver is online
   and, if so, leaves the mail alone (with a warning in the log). A dedicated,
   never-logged-in auctionator character keeps full recycling.
5. **The bidder is disabled by default** (`Auctionator.*Bidder.Enabled = 0`)
   because it buys player auctions without paying for them: the core pays the
   seller with newly created gold, which inflates the economy. Enable it only if
   that is what you want.
6. **Every bot purchase is unfunded, and the refund path depends on mail
   recycling.** The module holds no money by design, so its bids and buyouts are
   recorded without any escrow while `SendAuctionSuccessfulMail()` still pays the
   seller: each purchase grows the money supply (and the item the bot wins is sunk).
   When a *player* outbids the bot, the core refunds the bot's bid - money that was
   never escrowed - and that refund stays harmless only because the mail recycling
   destroys it (see 4: the hook deliberately steps aside while the auctionator
   character is online, at which point such a refund becomes real minted gold).
   Every purchase is logged with an `UNFUNDED` prefix, and a warning is printed once
   at startup whenever a bidder is enabled, so the faucet is never silent.

## Current limitations

1. ~~Stacks are acting weird, especially things like enchanter rods.~~ Fixed. The
   listed stack comes from `mod_auctionator_itemclass_config.stack_count` (clamped by
   the item's own max stack); with `Auctionator.Seller.RandomizeStackSize = 1` it is a
   random value between 1 and that size.
2. ~~No control on stack size, it is hard coded to 20.~~ Stack size is data driven per
   class/subclass (`stack_count`, see "Item selection"), and GM listings take an
   explicit `stack` argument.
3. Item selection is driven by `mod_auctionator_itemclass_config` (the class whitelist),
   `mod_auctionator_quality_config` (the per-quality gate) and
   `mod_auctionator_disabled_items` (the flat blacklist); there is still no in-game editor for
   them, but the management panel edits all three and they apply on the next seller run.
   (edit the tables and wait for the next seller cycle).
4. The import is synchronous, so a very large CSV stalls the world thread while it
   is written. Keep `Auctionator.MarketData.ImportMaxRows` sane and prefer
   incremental exports.
5. Recycled mail is destroyed, not returned: an unsold listing's item is deleted
   together with its expiry mail (see "Economy and safety guarantees" 1), so the
   module is an item sink as well as a gold sink. The bot never reclaims its own
   stock - if you want unsold items back, use a real `owner` on `.auctionator add`
   (that mail is not recycled).
