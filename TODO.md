# TODO

## Recommendations (owned games only)

- [ ] **Play history**: record last played and total play time on each
      launch (in `gog-manager-state.json`). Show it on the game page, add
      "sort by last played" and a "continue playing" row.
- [ ] **"Recommended for you"**: suggest owned games that are not installed,
      ranked by how much they overlap with the installed games (and, once
      play history exists, weighted towards what is played most and most
      recently). Never games the user doesn't own: no store catalog or GOG
      recommendation endpoint.
      - Signals, all cached offline: GOG genres (categories, already
        fetched), original language, release date, and from `v2/games`
        (see below) series, developer/publisher and features such as co-op
        or controller support.
      - Show it as a row on the library page or a sidebar filter.
      - Give each suggestion its reason as text, e.g. "Shares RPG, Fantasy
        with 3 of your installed games" — not only colour or ranking.
      - Keyboard and screen reader reachable like the rest of the library.
- [ ] Maybe later: "what to play next" among installed games that haven't
      been played (or not for a long time), using the same scoring.

## GOG API

Parts of GOG's API that GOG Manager does not use yet. GOG has no official
documentation for its store and library web API: the endpoints below come
from the unofficial, incomplete community docs at
<https://gogapidocs.readthedocs.io/> and can change without notice. GOG's
official docs (<https://docs.gog.com/sdk/>, the Galaxy SDK for game
developers, and the Galaxy Integrations API for Galaxy 2.0 plugins) don't
cover it.

GOG Manager already gets its data in three ways:

- **lgogdownloader** (3.19) for the login, `--status`, `--list j`,
  `--download` and `--check-orphans`, saving the product JSON, icon, logo and
  serials. Patches are excluded.
- **The product JSON** that lgogdownloader saves: id, slug, title,
  release_date, game_type, `description.full`, `content_system_compatibility`,
  related_products, and the installer and bonus file lists.
- **`api.gog.com/v2/games/<id>`**, only `_embedded.tags` (categories).

Where lgogdownloader already does something, use it rather than calling the
API ourselves: it holds the login and handles GOG's quirks.

### Through lgogdownloader

- [ ] **Changelogs with updates**: the game page shows the changelog from
      the product JSON; show it in the download window too, next to a
      game's new version.
- [ ] **Updated / new flags**: `--updated` and `--new` to show which games
      changed before a full `--status`; `--clear-update-flags` afterwards.
- [ ] **Notifications**: `--notifications`.
- [ ] **Repair**: `--repair` to check downloaded files against GOG's hashes
      (the update check uses `--size-only` today).
- [ ] **Your GOG tags**: `--list tags` and `--tag` as an extra filter in the
      sidebar.
- [ ] **Hidden games**: `--include-hidden-products`, or hide them in our list
      too.
- [ ] **Game details JSON**: `--save-game-details-json` (the
      `account/gameDetails/<id>.json` data: changelog, messages, CD keys).
- [x] **Free space**: checked by GOG Manager itself before downloading
      (lgogdownloader's `--check-free-space` counts files already on disk
      too) and before installing.
- [ ] **Download speed limit**: `--limit-rate`.
- [ ] **Patches and language packs**: offer them, instead of always
      excluding patches.
- [ ] **Galaxy cloud saves**: `--galaxy-show-cloud-saves`,
      `--galaxy-download-cloud-saves`, `--galaxy-upload-cloud-saves`
      (with `--cloud-whitelist` and `--wine-prefix` for our Wine/Proton
      prefixes), so saves are shared with other machines and Windows.
- [ ] **Galaxy builds**: `--galaxy-show-builds` and `--galaxy-install`
      for a build history, per-file updates and rolling back to an older
      version.

### api.gog.com (no login needed)

- [x] **More of `v2/games/<id>`**, which we already fetch for categories:
      features, tags, audio and text languages, system requirements per OS,
      age ratings, editions, technologies (DOSBox), copyright and "in
      development"; cached with the categories, with Feature and Tag
      filters. Not used: developers, publisher, series, screenshots and
      videos (the games database has them), and prices (games are owned).
  - [ ] A filter by audio or text language.
- [x] **`products/<id>?expand=screenshots,videos,changelog`**: not needed,
      the product JSON lgogdownloader saves has them (changelog,
      screenshots, videos, links), and the game page now uses them.

### Other GOG services

- [x] **`gamesdb.gog.com`**: vertical covers on the cards; background,
      summary, developer, publisher, series, themes and game modes on the
      game page.
  - [x] Filters and sorting by theme, series, developer or game mode
        (also genre and publisher; sorting by rating).
  - [x] Screenshots and videos (`game.screenshots`, `game.artworks`,
        `game.videos`) on the game page; genres, critics' rating and the
        first release in its details.
  - [x] Refresh the game page when details arrive while it is open.
  - Not used: `game.releases` (the other stores; gamesdb links games to
    stores that don't sell them), `available_languages`,
    `supported_operating_systems`, `dlcs`, `logo` and `icon` (the product
    JSON and lgogdownloader have them), `mods_ids` and the popularity
    fields (always empty or 0 when tried).
- [ ] **Free on GOG now** (`catalog.gog.com/v1/catalog`, no login needed):
      now and then (e.g. once a day, and on demand), look for games at
      100% off and show "Free on GOG now: X (until …)" with a link to the
      store page. Leave out games already in the library.
      - Query: `price=between:0,0&discounted=eq:true&productType=in:game,pack`
        plus `countryCode`, `currencyCode` and `locale`. Deals differ by
        country, and the user may be travelling or living somewhere other
        than their system language suggests, so ask GOG where they are:
        `https://www.gog.com/userData.json` (no login needed) answers from
        the request's location with `country` ("NO") and
        `selectedCurrency.code` ("NOK"). Ask on each check, not once, as
        the user may have moved; fall back to the system locale when it
        fails, and allow a country setting to override it. Without
        `countryCode` the catalog quietly uses US prices in USD.
        (`price=between:0,0` alone gives the ~440 always-free products,
        mostly demos.) Tested 2026-10-20: the filters work; no 100% deal
        was running that day.
      - GOG's front-page giveaways ("claim" while logged in) don't always
        appear as a 100% discount in the catalog, and there is no
        documented way to list or claim them; lgogdownloader can't either.
      - Opt-in, and not only shown by colour; announce it to screen readers.
- [ ] **`reviews.gog.com`**: user ratings and reviews.
- [ ] **Achievements and stats** (`gameplay.gog.com`, needs the Galaxy
      login): show them read-only. Games only report them through Galaxy,
      so they can't be earned through our launcher.
