# Halo: Combat Evolved — Raspberry Pi & Fair-Play Port (`halo-ce-pi`)

**A security-hardened, fairness-maximalist fork of the Halo: Combat Evolved
decompilation port — serving as the full-capability reference title for the
[`mmo` Fair-Play Protocol (FPP v1)](https://github.com/tmart234/mmo) and a
Raspberry Pi home-lab multiplayer stack.**

- **Raspberry Pi Home-Lab Target:** run the entire FPP v1 trust plane (`fppd`)
  as one lean `aarch64` binary on a **Raspberry Pi Zero 2 W** (512 MB RAM) while
  hosting verified dedicated Halo matches (targeting a **Raspberry Pi 5**
  dedicated server alongside x86 Linux hosts) for Linux, Windows, and Android
  players.
- **Playable Native Clients:** cross-platform port of Xbox build 2342
  (`cachebeta.exe`, SHA-256
  `4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`) for Linux,
  Windows, and Android — up to 128 players over LAN or cryptographically
  authenticated internet play.

<img width="1289" height="995" alt="The game on Linux" src="https://github.com/user-attachments/assets/0d3ad50f-f8b8-46cf-aef8-e3661da2a7d7" />

Derived from the native port of [bnunu/halo-1](https://github.com/bnunu/halo-1)
(originally forked from [punpckhdq/halo](https://github.com/punpckhdq/halo)).

---

## Fairness & Security Requirements (`mmo` Reference Title)

> **Modification Scope Rule (`FR-00`):** Changes to the Halo: CE engine in this
> fork exist **solely** to harden security, enforce cryptographic authority and
> trust, or maximize competitive fairness (plus cross-platform determinism
> required for audit) — **never** to alter legitimate Halo: CE gameplay, balance,
> or physics.

Every game-engine and networking modification introduced by this fork is
tracked as a verifiable requirement below:

| ID | Requirement | Mitigates / FPP | Implementation | Verification |
| --- | --- | --- | --- | --- |
| **FR-01** | **Host-authoritative movement & vehicles:** host ignores client position reports, simulates units/vehicles strictly from buffered client input ticks, stamps `input_tick`, and clients reconcile prediction error. | `H01` · `AUTH-01`, `AUTH-02` (speed/teleport/wall-step) | `network.authority = "host"` ([`network_distributed.c`](port/linux/game/network_distributed.c), [`network_objects.c`](port/linux/game/network_objects.c), [`player_queues_new.c`](source/game/player_queues_new.c)) | `tools/network_soak.py --scenario movement` |
| **FR-02** | **Host-validated combat & line-of-sight:** host validates shooter ownership, carried weapon, fire rate, lag-compensated target bounds, unobstructed level-geometry raycast, melee reach (`<= 4` wu), and strict damage flags/area/scale/multiplier bounds. | `H02` · `AUTH-03`, `AUTH-04` (wall-shooting, forged damage/kills) | [`network_damage.c`](port/linux/game/network_damage.c) | `tools/network_soak.py` (`wall_hits`, `forged_*`, `soak`) |
| **FR-03** | **Anti-ESP server-side relevance & per-pair LoS filtering:** host withholds enemy player states, occupied vehicles, and relayed inputs outside PVS + per-pair solid-structure BSP line-of-sight (with corner-peek margin) and the 32-wu motion-tracker/audio radius (15-tick hysteresis). | `H06` · `INFO-01` (memory-reader radar/wallhack) | `network.relevance = "true"` ([`network_distributed.c`](port/linux/game/network_distributed.c), [`network_objects.c`](port/linux/game/network_objects.c)) | `tools/network_soak.py --scenario radar` |
| **FR-04** | **Structured server-side cheat & aim-kinematics Signals:** rejected hits, ignored position reports, and impossible single-tick aim snaps (`> 120°` immediately before a hit) emit rate-limited JSONL telemetry (`signals.jsonl`) without altering legitimate gameplay. | `AUTH-04`, `DET-01`, `DET-03` | [`network_signals.c`](port/linux/game/network_signals.c), [`network_distributed.c`](port/linux/game/network_distributed.c), [`network_damage.c`](port/linux/game/network_damage.c) | `tools/network_soak.py` (0 false rejects in `soak`) |
| **FR-05** | **Authenticated per-peer Noise IK transport & zero hand-rolled crypto:** every tunnel pair negotiates forward-secret `Noise_IK_25519_ChaChaPoly_SHA256` with counter replay protection and path-migration challenges; signalling and hashing call the pure-Rust FPP C SDK (`p2p_crypto.c` deleted). | `H03`, `H04`, `H05` · `PROTO-03`, `PROTO-04`, `SEC-01` | [`p2p.c`](port/linux/src/p2p.c), [`p2p_signal.c`](port/linux/src/p2p_signal.c), [`fpp.h`](port/third_party/fpp/include/fpp.h) | `tools/p2p_loopback_test.py` + `mmo` `ffi-c-test-i686` |
| **FR-06** | **Cryptographic match evidence & offline host audit:** clients sign per-epoch `InputCommit`s (hit reports) and hosts sign hash-chained `Checkpoint`s (outcomes) into `.fppb` bundles audited by `fppd audit` to prove host hit-dropping or equivocation. | `H09` · `EVD-01`, `EVD-04`, `EVD-05` (host immunity/omission) | [`p2p_evidence.c`](port/linux/src/p2p_evidence.c) | `tools/p2p_loopback_test.py --evidence`, `tools/network_soak.py --scenario host_immunity` |
| **FR-07** | **Hardware trust tier, measured build allowlist & live Device ID bans:** hosts enforce Verifier-signed Attestation Results (`fpp_ar_verify` / `fpp_admission_admit`), minimum device tier, measured `network.client_builds`, `network.banned_devices`, and live mid-match `fppd enforce` Revocation Feed kicks. | `H07`, `H08` · `ATT-06`, `ATT-10`, `ID-02`, `ENF-01`, `ENF-02` | [`p2p.c`](port/linux/src/p2p.c), [`p2p_verified.c`](port/linux/src/p2p_verified.c) | `tools/p2p_loopback_test.py --trust`, `--verified` |
| **FR-08** | **Verified dedicated playlists with direct `fppd` admission & continuous SAR blessing:** headless dedicated hosts link to `fppd` Server Liveness (`fpp_gs_link_*`), clients attest and match directly via `fpp_client_admit` (`network.trust_authority` / `network.queue`) or `network.ticket_file`, and clients disconnect within one SAR lifetime (10 s) if blessing lapses. | `H09` · `ATT-08`, `ENF-03` (rogue host) | [`network_dedicated.c`](port/linux/game/network_dedicated.c), [`p2p_verified.c`](port/linux/src/p2p_verified.c) | `tools/p2p_loopback_test.py --verified` |
| **FR-09** | **Client-verifiable Transparency Log & live split-view revocation:** verified clients audit host `CheckpointHead`s against `fppd`'s Transparency Log (`fpp_tlog_check_checkpoint`: C2SP log signature + witness cosignature + RFC 9162 Merkle proof) and automatically submit `EquivocationReport`s (`fpp_tlog_report_equivocation`) to revoke split-view hosts in real time. | `H09` · `EVD-02`, `EVD-03` (split-view host) | [`p2p_verified.c`](port/linux/src/p2p_verified.c), [`fpp.h`](port/third_party/fpp/include/fpp.h) | `tools/p2p_loopback_test.py --verified` (`HALO_CHEAT_HOST_EQUIVOCATE=1`) |

Full protocol architecture, config switches, and debug-only red-team test flags
live in [port/linux/NETCODE.md](port/linux/NETCODE.md).

---

## Raspberry Pi Home-Lab Stack

| Node | Role | Status |
| --- | --- | --- |
| **Raspberry Pi Zero 2 W** (`aarch64`) | Runs the entire `mmo` regional trust plane (`fppd`: Server Liveness, Verifier, Broker, Revocation Feed, Evidence Store, Transparency Log) in one static binary under 512 MB RAM on `4444/tcp`. | **Ready** — build with `make pi-cell` and install via [`mmo/deploy/pi/README.md`](https://github.com/tmart234/mmo/blob/main/deploy/pi/README.md). |
| **Raspberry Pi 5** (`aarch64`) | Runs a headless verified dedicated Halo host (`network.dedicated` + `network.liveness`, optional TPM 2.0 HAT) using `aarch64` `libfpp.a` in an LP64 host loader around the ILP32 Halo guest image (same architecture as `port/android/host`). | **SDK & dedicated mode ready; `port/linux-arm64/host` loader next** — `libfpp.a` `aarch64` passes `make ffi-c-test-aarch64` in `mmo`; full design in [`mmo/docs/anticheat/08-reference-title-halo.md`](https://github.com/tmart234/mmo/blob/main/docs/anticheat/08-reference-title-halo.md#8-hardware-deployment-pi-5-game-server-pi-zero-2-w-cell). |
| **PC / Phone Clients** | Linux x86, Windows x86, Android arm64 players connecting directly via `network.trust_authority` or invite link. | **Ready** — built and tested in GitHub Actions on every commit. |

---

## Download

GitHub Actions builds the game for each commit to `main`:

| Platform | Release | Debug |
| --- | --- | --- |
| Linux | [halo-linux-release.zip](https://github.com/tmart234/halo-ce-pi/releases/latest/download/halo-linux-release.zip) | [halo-linux-debug.zip](https://github.com/tmart234/halo-ce-pi/releases/latest/download/halo-linux-debug.zip) |
| Windows | [halo-windows-release.zip](https://github.com/tmart234/halo-ce-pi/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/tmart234/halo-ce-pi/releases/latest/download/halo-windows-debug.zip) |
| Android | [halo-android-release.zip](https://github.com/tmart234/halo-ce-pi/releases/latest/download/halo-android-release.zip) | [halo-android-debug.zip](https://github.com/tmart234/halo-ce-pi/releases/latest/download/halo-android-debug.zip) |

Use the release build to play. The debug build stops at the first failed
assertion, writes it to the log, and enables the red-team test switches
(`debug.cheat_*`). Refer to [Releases](https://github.com/tmart234/halo-ce-pi/releases)
for previous builds.

## Game data

The port does not include Halo: CE game data. Supply your own Xbox disc image
(`.xiso` or `.iso`) of Halo: Combat Evolved. European (PAL) maps are converted
to play identically to North American (NTSC) maps for cross-region play.

1. Start the game.
2. At first launch, select the disc image.
3. The game extracts `maps/` next to the executable (or in the app data folder
   on Android; see [port/android/README.md](port/android/README.md)).

## Platforms

| Platform | Instructions |
| --- | --- |
| Linux (32-bit x86 executable, OpenGL 4.5, SDL3, full `fpp-ffi` + `gs-link`) | [port/linux/README.md](port/linux/README.md) |
| Windows (32-bit x86 executable, OpenGL 4.5, SDL3, `fpp-ffi` Noise IK sessions) | [port/windows/README.md](port/windows/README.md) |
| Android (arm64 app, OpenGL ES 3, SDL3, ILP32 guest + LP64 host) | [port/android/README.md](port/android/README.md) |

## Multiplayer

- Up to 128 players on up to 128 machines across Linux, Windows, and Android.
- **Player-hosted internet play:** Noise IK encrypted sessions via invite links
  plus signed match evidence bundles auditable offline with `fppd audit`.
- **Verified dedicated playlists:** set `network.liveness` on the host and
  `network.trust_authority` + `network.key_bundle` (or `network.ticket_file`) on
  clients for live `fppd` attestation, Broker matchmaking, sub-second
  revocations, and Transparency Log split-view protection. Refer to
  [port/linux/NETCODE.md](port/linux/NETCODE.md).

## Build the game

You do not need the Xbox SDK. The port supplies the SDK declarations that
the game uses. Refer to [port/include/xdk](port/include/xdk/README.md).

To build the game:

1. Install Python and [ninja](https://ninja-build.org/).
2. Install the tools for your platform. Refer to the README for the
   platform.
3. In the root folder of the repository, enter `python configure.py`.
4. Enter `ninja` with the target for the platform:

| Target | Result |
| --- | --- |
| `ninja linux` | `build/linux/halo` |
| `ninja windows` (on Windows) | `build/windows/halo.exe` and `SDL3.dll` |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` |

If you enter `ninja` without a target, ninja builds the game for the
computer that you use.

`tools/ci_build.py` makes the same builds as GitHub Actions. For example,
enter `python tools/ci_build.py linux release`.

### Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--portable` | The Linux and Windows builds operate on all x86-64 processors. Use this option for builds that you give to other persons. |
| `--lto=thin`, `--lto=off` | Less link-time optimization. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--pgo=train` | Records a new optimization profile. Refer to "Optimization profiles". |

Without `--portable`, the Linux and Windows builds use all the instructions
of the processor that builds them (`-march=native`). Such a build does not
always start on a different computer.

### Optimization profiles

The builds use profiles of the game to optimize the code:

- `pgo/halo_linux.profdata` for Linux and Android.
- `pgo/halo_windows.profdata` for Windows.

The profiles need clang 22 or later. With an older clang, the builds do not
use the profiles.

To record a new profile:

1. Delete the profile.
2. Enter `python configure.py --pgo=train`.
3. Enter `ninja linux` or `ninja windows`.

The build then plays the main menu and the first minute of each campaign
level. This procedure continues for approximately 15 minutes. The game
data must be in `assets/`.

### The byte-matching build

The original project also has a byte-matching build. That build compiles
the game with the compiler of the Xbox SDK and compares the result with
`cachebeta.exe`. This project does not generate that build, because the
Xbox SDK is not free to distribute. The sources of that build are not
changed. To use the build again, set `SolutionConfig.matching` in
`tools/project_x86.py`. You must also have the Xbox SDK in `xbox/` and
`cachebeta.exe` in the root folder.
