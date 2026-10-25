# FM2 online: scope

Bringing Forza Motorsport 2's online features back without Xbox Live: online racing, the auction
house, car gifting, leaderboards and tournaments, run on our own free-tier services. The game's own
online code stays as it is; we answer the system calls it makes (as we already do for everything
else) and stand in for the servers it talked to.

## What FM2 used online

From the game's imports and the strings in its executable (the title update's image):

**1. Xbox Live, for racing.** Peer-to-peer races between consoles.

| Part | How the game does it | Imports |
|---|---|---|
| Lobbies, matchmaking, ranked matches | Xbox Live sessions (create, search, join, arbitration, TrueSkill), sent to the dashboard as XMsg requests | `XMsgStartIORequest(Ex)`, `XMsgInProcessCall`, `XamSessionCreateHandle` |
| Race traffic | Xbox secure networking: UDP/VDP sockets between consoles, addressed by XNADDR and session keys | `NetDll_socket/sendto/recvfrom...`, `XNetConnect`, `XNetXnAddrToInAddr`, `XNetRegisterKey`, `XNetGetTitleXnAddr` |
| Connection quality | QoS probes between players before a match | `XNetQosListen`, `XNetQosLookup`, `XNetQosRelease` |
| Voice chat | Headset voice | `XamVoiceCreate`, `XamVoiceSubmitPacket` |
| Friends, gamercards, invites | Dashboard UI | `XamShowFriendsUI`, `XamUserAreUsersFriends`, `XamShowGamerCardUIForXUID` |
| Stats | Leaderboard reads | `XamUserCreateStatsEnumerator` |

Lobby types in the game: online, online career, ranked, tournament (game, practice, qualify),
system link and split screen (`LobbyType*`).

**2. Turn 10's own servers, for everything else.** Reached through Xbox Live's LSP (the way a
game talked to its publisher's servers: `XNetServerToInAddr`, `XNetLogonGetTitleID`,
`/lsp/enumerate.ashx`). Calls are "XTS" (Xbox Title Services) requests to `.ashx` web services
with a binary format; every request and result type is still named in the executable:

| Service | Calls (`XTS_Forza2...`) |
|---|---|
| Auction house | `AuctionCreate`, `SearchForAuction`, `GetAllOpen`, `GetAll`, `GetDetails`, `GetDetailsAndBids`, `GetBids`, `GetBidOn`, `PlaceBid` (and buyout), `Cancel`, `GetSubmitted`, `RetrieveCarForAuction`, `RetrievePaymentsForAuction`, `RetrievePendingCars`, `RetrievePendingPayments`, `GetCarStatsAndLiveryDetails`, `GetLiverySnapshots`, `GetSystemProperties` |
| Gifting | `SendGift`, `GetGiftsForUser`, `GetDetails`, `RetrieveGift`, `RetrieveForUser` |
| Leaderboards and tournaments | `Lbqt` calls: `ReportLeaderboardResult`, `GetLeaderboardResultsByRank`, `GetLeaderboardResultsNear`, `GetLeaderboardResultForUsers`, `GetAvailableTournaments`; brackets, elimination rounds, qualifying |
| Text vetting | `/msgserver/vetstring2.ashx`, `getstring2`, `logstring` (profanity filter for names and auction text; server messages) |
| Feedback | `/fbserver/submitreview`, `submitcomplaint`, `getaggreview`, `enumrecdreview`, `enumsentreview` |
| Utilities | `UtilsGetServerTime`, server info and config, ForzaTV status |

The auction house also has an anti-cheat ("unicorn") check, and car transfers carry the whole car
and its livery from the save.

## What already exists

- [Xenia Canary netplay](https://github.com/AdrianCassar/xenia-canary) (a fork of Xenia, the
  Xbox 360 emulator) and its Xenia WebServices backend stand in for Xbox Live's peer-to-peer side
  (sessions, XNet, QoS, XStorage), which over 500 games use
  ([overview](https://emulation.gametechwiki.com/index.php/Xbox_Live)). It is the best reference
  for part 1: which XMsg message IDs mean what, how sessions and XNADDRs map onto real addresses.
  It says outright that games needing their own servers (FM2's auction house, gifting,
  tournaments) are out of its scope.
- No revival of FM2's Turn 10 services exists that we could find. The auction house closed years
  before the 360 servers did ([Forza forums](https://forums.forza.net/t/auction-house-and-some-other-questions/21164)).
  Part 2 is new work.

## The plan

Two pieces, both ours:

**In the game (C++, src/kernel and a new src/online):**
- **Xbox Live layer:** the XMsg session calls, XNet addressing (XNADDR to a real IP and port), QoS,
  friends and presence, stats, all answered by our server instead of the dashboard.
- **Sockets:** the game's UDP sockets on real Winsock, with XNet addresses translated; NAT hole
  punching set up by the server, a relay when it fails.
- **LSP layer:** `XNetServerToInAddr` points the game at our service, and its XTS requests go
  over HTTPS to it (the game builds the requests and reads the results itself; we carry them).
- **Identity:** each profile already has its own XUID (signed saves per XUID). A sign-in in the
  launcher ties it to an account so names and cars can't be claimed by someone else.

**On the server:**
- **Accounts and presence:** gamertag, XUID, online status, friends.
- **Matchmaking:** lobbies and sessions (search, join, host migration), ranked TrueSkill.
- **Turn 10 services:** auction house, gifting, leaderboards, tournaments, text vetting, server
  time, rebuilt from the call names above and what the game sends.
- **Relay:** for players whose connection can't be punched through.

## Free services

| Need | Service | Free tier |
|---|---|---|
| API, accounts, auction house, gifting, leaderboards, tournaments | **Cloudflare Workers** with **D1** (SQLite) | 100,000 requests a day; D1 5 GB, about 5 million rows read and 100,000 written a day ([enforced since Sept 2026](https://developers.cloudflare.com/changelog/post/2026-09-01-d1-free-tier-limit-enforcement/)) |
| Lobbies and live sessions (one object per lobby, WebSockets) | **Cloudflare Durable Objects** (SQLite-backed) | 100,000 requests a day, 5 GB ([pricing](https://developers.cloudflare.com/durable-objects/platform/pricing)) |
| UDP relay for blocked connections | **Oracle Cloud Always Free** AMD micro VM (1/8 CPU, 1 GB) | Two of them, unchanged by the June 2026 cut to the Arm allowance ([InfoQ](https://infoq.com/news/2026/07/oracle-cloud-free-tier-limits/)); Oracle may reclaim idle VMs |

Why these: the reports server already runs on Cloudflare (same account, same tooling), Workers
and Durable Objects cover everything that is request and response or a lobby's live state, and
they cost nothing at our scale. Cloudflare has no UDP, so race traffic goes player to player
(the game was built for that), with a small Oracle VM relaying only when hole punching fails.
Fallback if Oracle reclaims the VM: relay through a Durable Object over WebSockets (works, but TCP
adds lag spikes, so only as a last resort).

At FM2 scale (tens to a few hundred players) the free tiers are far from their limits: a race is
a handful of API calls, an auction search one, and race traffic never touches Cloudflare.

## Phases

| Phase | What | Outcome |
|---|---|---|
| 0. Research | Trace the XMsg session calls FM2 makes (with Xenia netplay as the map); capture the XTS binary format (the serializer, `XtsClientAndServer\BinarySerialization`) and each call's fields; read the game's UI flows (`LobbyLaunch.xml`, `QDLobby.xml`) and Lua bindings | A protocol document; no player-facing change |
| 1. LAN | System link between two PCs on one network: XNet over real sockets, no server | Two PCs race each other |
| 2. Online racing | Server matchmaking and lobbies, hole punching, relay | Online races (unranked) |
| 3. Leaderboards and ranked | Stats writes, TrueSkill, leaderboards, time trials | Ranked races and leaderboards |
| 4. Auction house and gifting | The XTS services, car transfer checks, text vetting | Buy, sell and gift cars |
| 5. Tournaments | Brackets, qualifying, scheduling | Tournaments |
| Later | Voice chat, friends and invites UI, ForzaTV, feedback | |

Phases 1 and 2 are the hard core (the game's networking code has to work over real sockets,
and its timing over the internet); 4 is mostly server work once phase 0 has the format.

## Findings (2026-10-25)

**From the Forza Horizon recomp** ([Dyotson/ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined), MIT):
it has online working (a virtual network with hole punching and relay, sessions, friends, invites,
TrueSkill stats, title services) on a Python server, and the message layouts it uses come from the
Xenia netplay fork. Its title services are HTTP (`POST /Services.Horizon/o.xtsw`, `X-ClassName` /
`X-Method` headers, a packed big-endian body described by type descriptors in the executable,
`docs/title_services.md`). Its auction house and gifting server is complete, but FH1 has no screens
for it; FM2 has them.

**FM2's title services are older.** The class and method names match (`Xts.WebServices.Utils, Xts.WebServices`
`GetServerTime`; `Xts.Titles.Forza2.WebServices.Auction` `Cancel`), but there is no HTTP: requests go
through `TSTransportClient` (`Platform\Client\XtsClient`), which asks for a topology (servers by
`ServiceId`, address, port, `VirtualPort`, status, version) after finding the title server the same
way FH1 does (`XTitleServerCreateEnumerator`, `Matches(mask, description)`). Its wire format has to be
captured from the game.

**`FM2_LIVE=1` (trace mode).** The profile shows as signed in to Xbox Live and the network as up; online
requests are logged and still fail. With it, Multiplayer > Xbox LIVE offers Exhibition Race, Career Race,
Tournaments, Auction House, Gift a Car and Forza Motorsport TV. Exhibition Race > Create a Match reaches
the rules screen; Create then sets presence and properties (XGI `0xB0006`, `0xB0007`), creates a key
(`XNetCreateKey`, unimplemented), a session handle (`XamSessionCreateHandle`, `XamSessionRefObjByHandle`)
and reads stats (`0xB0021`), and crashes in `sub_822A01C8` on the session object we don't provide.
Next: sessions (create, search, join) after the Xenia netplay layouts.

## Risks and open questions

- **The XTS format.** Turn 10's binary serialization has to be read from the game's code. The
  type names are there; field layouts are not, so each call needs tracing.
- **Ranked arbitration.** Xbox Live checked race results agreed between players; without it,
  ranked and leaderboards trust the clients. Plan: results reported by every player and compared
  by the server, plus sanity checks (lap times against track records).
- **Cheating and duplication.** Cars are in local saves, which players can edit. The auction house
  and gifting need server-side checks (the game's own "unicorn" check, plus a server record of
  each car that moves) so cars can't be duplicated or invented.
- **Moderation.** Gamertags, auction text and liveries are visible to others: the vetting service
  needs a word filter, and someone has to handle complaints.
- **Identity.** XUIDs are generated locally, so the server needs real accounts (a launcher
  sign-in, for example with Discord or GitHub) to stop impersonation.
- **Free tier changes.** Both providers have cut free tiers before (Oracle in June 2026; D1 now
  enforces its limits). The server should be portable to a cheap paid host.
- **Proton/Linux.** Sockets and hole punching should work under Wine, but need testing.
- **Legal.** We don't touch Microsoft's services or use its code; the servers are our own
  reimplementation, answering what our copy of the game asks. The Xbox Live trademark and the
  Forza name stay out of the service's branding.
