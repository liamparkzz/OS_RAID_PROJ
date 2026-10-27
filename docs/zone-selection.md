# Zone selection

Selection used by both empty-destination migration and reserve exchange:

- `SWANS_Zone_Popularity_Mode=HALF_DECAY` (default): at each window boundary,
  `P_new = 0.5 * P_old + host_write_sectors_in_window`.
- `LAST_EPOCH`: `P_new = host_write_sectors_in_window`.
- `SWANS_Zone_Popularity_Period` is a fixed duration in nanoseconds; zero uses
  `SWANS_Epoch_Default`. It does not change with NORMAL/REDIRECT/MIGRATION state.
- Selection uses the latest completed window. Updates happen lazily before host
  write accounting and policy evaluation. Idle windows still age popularity.
- Host overwrites count again for popularity; migration copies do not.
- Source: maximum popularity on the oldest SSD. Equal-popularity ties prefer
  fewer copy sectors, then lowest logical zone ID. A cold-zone selector uses the
  minimum popularity, with the same cost tie break.
- Logical valid ratio counts distinct observed sectors divided by zone size.
  This is logical data coverage, not physical NAND valid-page ratio. Read-initialized
  ranges are recorded without contributing to write popularity. With reserve mode,
  supported preconditioning data is registered from the actual FTL mappings before
  host I/O; mappings outside the exported capacity are discarded. Legacy mode
  still tracks host-observed data only.
- Copy cost counts touched migration units per stream. Reserve mode uses flash
  pages, so reading a copy never initializes a wholly unwritten page. Legacy mode
  retains its erase-block copy units. Partially valid pages are copied as full pages;
  the sector-level ratio is not an estimate of physical NAND valid-page ratio.

This implements the requested halving/last-window alternatives, not a verified
reproduction of a particular EDM paper. No weighted cost-versus-popularity score
is assumed: hottest/coldest ordering remains primary.

The youngest SSD's empty user zone is preferred. If none exists, select its coldest
used zone and the oldest SSD's hidden spare. Only one exchange can use that spare
at a time. See [reserve-exchange.md](reserve-exchange.md) for the two phases and
pause semantics. Selection is popularity-first; copy cost only breaks popularity
ties. A weighted popularity/cost objective has not been assumed.
