# Halo: Combat Evolved for Linux, Windows and Android

This project is a port of the Halo: Combat Evolved decompilation to Linux,
Windows and Android. The decompilation is of the Xbox build 2342
(`cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).

<img width="1289" height="995" alt="The game on Linux" src="https://github.com/user-attachments/assets/0d3ad50f-f8b8-46cf-aef8-e3661da2a7d7" />

The port starts from the decompilation of [bnunu/halo-1](https://github.com/bnunu/halo-1).
That project is a fork of [punpckhdq/halo](https://github.com/punpckhdq/halo).

## Download

GitHub Actions builds the game for each commit. These links download the
builds of the latest release:

| Platform | Release | Debug |
| --- | --- | --- |
| Linux | [halo-linux-release.zip](https://github.com/cybersecurity/halo-ce-universal/releases/latest/download/halo-linux-release.zip) | [halo-linux-debug.zip](https://github.com/cybersecurity/halo-ce-universal/releases/latest/download/halo-linux-debug.zip) |
| Windows | [halo-windows-release.zip](https://github.com/cybersecurity/halo-ce-universal/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/cybersecurity/halo-ce-universal/releases/latest/download/halo-windows-debug.zip) |
| Android | [halo-android-release.zip](https://github.com/cybersecurity/halo-ce-universal/releases/latest/download/halo-android-release.zip) | [halo-android-debug.zip](https://github.com/cybersecurity/halo-ce-universal/releases/latest/download/halo-android-debug.zip) |

Use the release build to play. The debug build stops at the first failed
assertion and writes it to the log. Use the debug build to find and report
problems.

The game updates itself. At start-up it looks for a newer release, and asks
if you want to install it. Refer to "Updates" in
[port/linux/README.md](port/linux/README.md#updates).

Each build of the `main` branch that passes on all three platforms is a new
release. The [Releases](https://github.com/cybersecurity/halo-ce-universal/releases)
page keeps the last five releases. If the latest build has a problem, get
an older build from that page.

## Game data

The port does not include the game data. Download an Xbox disc image
(`.xiso` or `.iso`) of Halo: Combat Evolved. All versions of the game
operate. The maps of the European (PAL) version were made for a slower
console. The port changes them to play as the North American (NTSC) maps do,
so players of the two versions can play together.

1. Start the game.
2. At the first start, the game asks for the disc image. Select it.
3. The game extracts the `maps/` folder. Then the game starts.

On Linux and Windows, the game puts `maps/` next to the executable. On
Android, copy the disc image to the phone first. The app puts `maps/` in its
data folder. Refer to [port/android/README.md](port/android/README.md).

## Platforms

Each platform has its own instructions:

| Platform | Instructions |
| --- | --- |
| Linux (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/linux/README.md](port/linux/README.md) |
| Windows (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/windows/README.md](port/windows/README.md) |
| Android (arm64 app, OpenGL ES 3, SDL3) | [port/android/README.md](port/android/README.md) |

The Linux README also gives the controls, the settings and the multiplayer
functions. These are almost the same on all platforms.

## Multiplayer

The game can play system link games on a local network and on the internet:

- A system link game can have up to 128 players on up to 128 machines.
- Linux, Windows and Android machines can play in the same game.
- An invite link lets a machine join a game on the internet. No server of
  this project is necessary.
- The default netcode is new. Each machine moves its own player at once,
  and the host makes the decisions for the game. Refer to
  [port/linux/NETCODE.md](port/linux/NETCODE.md).

## Fairness & Security Requirements (`mmo` Reference Title)

This fork serves as the full-capability reference title for the
[`mmo` Fair-Play Protocol (FPP v1)](https://github.com/tmart234/mmo) framework.

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
| **FR-03** | **Anti-ESP server-side relevance filtering:** host withholds enemy player states, occupied vehicles, and relayed inputs outside each client's potentially visible set and 32-wu audio/motion-tracker radius (with 15-tick hysteresis). | `H06` · `INFO-01` (memory-reader radar/wallhack) | `network.relevance = "true"` ([`network_distributed.c`](port/linux/game/network_distributed.c), [`network_objects.c`](port/linux/game/network_objects.c)) | `tools/network_soak.py --scenario radar` |
| **FR-04** | **Structured server-side cheat Signals:** every rejected hit report and ignored position report emits rate-limited JSONL telemetry (`signals.jsonl`) instead of a silent drop. | `AUTH-04`, `DET-01` | [`network_signals.c`](port/linux/game/network_signals.c) | `tools/network_soak.py` (0 false rejects in `soak`) |
| **FR-05** | **Authenticated per-peer Noise IK transport & zero hand-rolled crypto:** every tunnel pair negotiates forward-secret `Noise_IK_25519_ChaChaPoly_SHA256` with counter replay protection and path-migration challenges; signalling and hashing use the pure-Rust FPP C SDK (`p2p_crypto.c` deleted). | `H03`, `H04`, `H05` · `PROTO-03`, `PROTO-04`, `SEC-01` | [`p2p.c`](port/linux/src/p2p.c), [`p2p_signal.c`](port/linux/src/p2p_signal.c), [`fpp.h`](port/third_party/fpp/include/fpp.h) | `tools/p2p_loopback_test.py` + `mmo` `ffi-c-test-i686` |
| **FR-06** | **Cryptographic match evidence & offline host audit:** clients sign per-epoch `InputCommit`s (hit reports) and hosts sign hash-chained `Checkpoint`s (outcomes) into `.fppb` bundles audited by `fppd audit` to catch cheating hosts dropping hits. | `H09` · `EVD-01`, `EVD-04`, `EVD-05` (host immunity/omission) | [`p2p_evidence.c`](port/linux/src/p2p_evidence.c) | `tools/p2p_loopback_test.py --evidence`, `tools/network_soak.py --scenario host_immunity` |
| **FR-07** | **Hardware trust tier, build allowlist & Device ID bans:** hosts verify Verifier-signed Attestation Results (`fpp_ar_verify` / `fpp_admission_admit`), enforcing minimum device tier, measured `network.client_builds`, and hardware-rooted `network.banned_devices` + live Revocation Feed events. | `H07`, `H08` · `ATT-06`, `ATT-10`, `ID-02`, `ENF-01` | [`p2p.c`](port/linux/src/p2p.c), [`p2p_verified.c`](port/linux/src/p2p_verified.c) | `tools/p2p_loopback_test.py --trust` |
| **FR-08** | **Verified dedicated playlists with continuous SAR blessing:** headless dedicated hosts link to `fppd` Server Liveness (`fpp_gs_link_*`), anchor evidence Checkpoints in the hybrid Transparency Log, admit Broker-ticketed players (`fppd ticket`), and clients disconnect within one SAR lifetime (10 s) if blessing lapses. | `H09` · `ATT-08`, `EVD-02`, `ENF-03` (rogue host) | [`network_dedicated.c`](port/linux/game/network_dedicated.c), [`p2p_verified.c`](port/linux/src/p2p_verified.c) | `tools/p2p_loopback_test.py --verified` |

Full technical details and debug-only red-team test switches live in
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
