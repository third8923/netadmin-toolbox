# Dev Plan · Rogue Software Purger (Purge Center)

**Project**: NetAdmin Toolbox — the "Purge Center" group
**Doc version**: v1.1 (2026-10-26 · M1 shipped)
**Nature**: research + incremental development planning
**Status**: ✅ M1 shipped with v2.13 (four decisions confirmed: ① integrate as a new
toolbox group ② M1 ships `purg.scan` first ③ signature DB = SoftCnKiller + own
additions ④ **online updates pulled forward to M1** — originally M4; dual-source
updates required: SoftCnKiller data repo + this project's own GitHub rule repo)

---

## 1. Reference-tool capability matrix (from an 8-tool review)

| Tool | Core positioning | Borrowable | Not borrowable / weak spots |
|---|---|---|---|
| Ashampoo UnInstaller 14 (commercial) | **Install-monitor snapshots**: pre/post-install diff → zero-residue uninstall | Snapshot-diff concept, browser extension detection, batch uninstall | Closed source, paid — cannot copy implementation |
| Geek Uninstaller | 6 MB portable single file, native uninstall + residue scan | Single-file portable positioning (matches our philosophy) | Narrow feature set |
| HiBit Uninstaller | Portable, broadest feature set (store apps / extensions / registry / shell menu / tasks) | Its feature list makes a great acceptance checklist | Closed source |
| Uninstall Tool | Hidden install entries, forced uninstall, HTML reports | Hidden-entry enumeration, report export | Commercial |
| **SoftCnKiller (primary benchmark)** | **Signature-DB scanner**: reads signatures & directory names of autoruns/services/drivers/tasks/shell menus/IMEs/file associations, matches sign.txt/folder.txt blacklists, one-click action; popup-based attribution; online rule updates | The whole scan-act model, tiered handling, DB format, popup attribution UX | MFC closed source; ineffective against driver-protected 360/2345-class software; doesn't remove program bodies |
| Revo Uninstaller | Deep registry/file residue scanning | Deep-scan algorithm concepts | Commercial |
| Wise Force Deleter | Handle unlocking + forced delete | Force-delete UX | Single-purpose |
| Wise Care 365 | Real-time registry protection, privacy erase, process monitoring | Real-time protection layer (Run-key watch) | Bundleware style, mismatch |

**Conclusion**: the 8 tools cover three capability tiers — **detect** (full
enumeration of installs/autoruns), **identify** (signature/directory blacklists +
heuristics), **act** (native uninstall → silent uninstall → forced delete →
residue cleanup → revival protection). No open-source tool covers all three
tiers; our opportunity = SoftCnKiller's CN-signature DB + BCUninstaller's batch
framework + own attribution/revival monitoring, all inside the single-file
toolbox form factor.

---

## 2. Open-source projects & rule repos (each verified)

### 2.1 Tools

| Project | License | Language | Value |
|---|---|---|---|
| **BCUninstaller** (github.com/BCUninstaller/Bulk-Crap-Uninstaller) | Apache 2.0 | C# | ★★★★★ Batch-uninstall textbook: 14 UninstallerType classes, **auto-generated silent-switch args**, residue scan, uninstaller certificate checks; needs a maintainer — ideas portable |
| **System Informer** (ex Process Hacker, github.com/winsiderss/systeminformer) | MIT | C/C++ | ★★★★★ Low-level treasure: handle enumeration/closing, hidden-process detection (DKOM), service/driver management, KSystemInformer driver source |
| Kill-Malware / Rogueware-Killer scripts | Open source (scripts) | PowerShell/BAT | ★★★ Certificate blacklisting, directory blocking, hosts blocking, Huorong rule JSON — "prevention layer" reference |
| Dism++ (discontinued) | MIT | C/C++ | ★★ Cleaning engine ideas (architecture docs still valuable) |

### 2.2 Rule repos (directly usable)

| Repo | URL | Format | Use |
|---|---|---|---|
| **SoftCnKiller data repo** (四海一叶秋, actively updated as of 2026-10) | gitee.com/softcnkiller/data · github.com/SiHaiYiYeQiu/SoftCnKiller | sign.txt / folder.txt / whitepath.txt, 3000+ entries | **Primary signature-DB source**; minimal format, trivial to parse |
| anti-AD | github.com/privacy-protection-tools/anti-AD | hosts / easylist | Ad/popup callback domain blocking |
| yhosts | github.com/vokins/yhosts | hosts | CN site/app ad domains |
| ad-wars | github.com/jdlingyu/ad-wars | hosts | CN video-site ad domains |
| neoHosts | hosts.nfz.moe | hosts | Mining/telemetry/hijack domains |
| StevenBlack/hosts | github.com/StevenBlack/hosts | hosts (aggregated) | International ads + malware domains |
| Neo23x0/signature-base | github.com/Neo23x0/signature-base | YARA + IOC (**DRL 1.1 — review terms before commercial use**) | PUP/adware file YARA rules (advanced) |
| Huorong community rules | bbs.huorong.cn (JSON) | Huorong custom-protection JSON | Rule semantics reference (translate to our format) |

### 2.3 Closed-source but methodologically instructive

- **Malwarebytes AdwCleaner** (free, closed): **quarantine + restore** model,
  browser policy reset, `/scan /clean` CLI — the engineering template for
  security tools.
- **Sysinternals MoveFile/PendMoves**: `PendingFileRenameOperations`
  reboot-delete (Microsoft's official safe-delete channel).

---

## 3. Product definition: Purge Center (6 planned modules)

Positioning: **single-file, portable, fully reversible, aimed primarily at
Chinese rogue software** — detect → identify → act in one place.

| # | Module | Name | Function |
|---|---|---|---|
| 1 | `purg.scan` | **Full scan** | Enumerate processes/services/drivers/autoruns/tasks/shell extensions/IMEs/file associations/browser extensions/hosts/proxy → match signature+directory DB → tiered action (service disable, driver stop+manual, task delete, autorun delete, ShellEx remove, association restore) |
| 2 | `purg.uninst` | **Uninstall center** | Install inventory (registry incl. hidden + MSI API + UWP + store) → native/silent uninstall (auto-generated switches) → residue scan (files+registry) → one-click cleanup |
| 3 | `purg.popup` | **Popup attribution** | Drag-crosshair window pick (WindowFromPoint + WinEventHook) → owning process/path/signature → related autorun scan → one-click cleanup (reuses scan engine) |
| 4 | `purg.unlock` | **Force delete** | Locate locking handles → 4-step escalation: direct delete → ACL/attribute repair → handle close → `PendingFileRenameOperations` reboot delete |
| 5 | `purg.net` | **Hijack repair** | Homepage/search-engine/shortcut hijack detect & repair, hosts tamper detect, proxy hijack detect, optional domain blocking (anti-AD subscription) |
| 6 | `purg.guard` | **Revival monitoring** (optional, advanced) | RegNotifyChangeKeyValue watches on Run/service keys, install snapshot diff, revival alerts |

**Rule engine (cross-cutting)**: unified JSON rule pack — signature (incl.
certificate thumbprint), path/directory, filename/resource, registry key;
built-in SoftCnKiller DB (credited); whitelist support; online updates from
gitee/GitHub raw (WinHTTP, hash-verified).
**Safety engineering (cross-cutting)**: every action writes an **operation log +
registry export backup** (one-click restore); optional quarantine directory;
danger-confirmed red dialogs for high-risk actions; never delete personal files
outside the program body.

---

## 4. Advanced skills required (research conclusions)

| Domain | Techniques | Used by | Our status |
|---|---|---|---|
| Deep Win32 | WinVerifyTrust/Authenticode, WinEventHook, WindowFromPoint, ShellExecuteEx | scan/popup | Mostly known; signature verification new |
| COM automation | ITaskService (mastered), IPackageManager (UWP), MsiEnumProducts* (MSI), IPersistFile (lnk) | uninstall/scan | Task COM battle-tested |
| ACL & ownership | SetNamedSecurityInfo/GetNamedSecurityInfo, take-ownership ladder | force delete | DeleteFileSmart already has L1–L4 |
| Handle tech | NtQuerySystemInformation(SystemHandleInformation), DuplicateHandle+close (System Informer style) | force delete | **New** (MIT source available) |
| Reboot delete | PendingFileRenameOperations REG_MULTI_SZ | force delete | Understood; simple to implement |
| Registry watch | RegNotifyChangeKeyValue multi-threaded tree watch | revival monitor | New, low barrier |
| Browser anatomy | Chromium `Secure Preferences`/`Preferences` JSON, Edge/Firefox profiles | scan/hijack repair | New; HMAC handling needed |
| Network update | WinHTTP + cert + hash verification | rule updates | New (easy) |
| Test engineering | VM snapshots + real-sample regression set | all | Reuse QA process |

**Frontier judgments**:
1. **No kernel driver in v1** — PUP cleanup is 95% covered by user-mode APIs +
   handle tech + reboot-delete; drivers add signing/BSOD/false-positive risk
   (SoftCnKiller succeeded without one). If 360/2345-class driver protection
   shows up, evaluate a minimal KSystemInformer-derived driver (MIT).
2. **libyara deferred** — size and rule licensing (DRL 1.1) need review; v1's
   "signature + path + resource string" channels suffice. (v2.14 note: we ended
   up shipping a **self-implemented YARA subset**, zero dependencies.)
3. **No background resident defense in v1** — matches the "scan on demand, exit
   clean" philosophy; revival monitoring as an optional light watcher thread.

---

## 5. Milestones (each independently shippable & regressable)

| Milestone | Content | Depends |
|---|---|---|
| **M1 (v2.13 ✅ shipped)** `purg.scan` + online updates | Full autorun enumeration + SoftCnKiller DB adaptation (sign/folder/whitepath parsers) + signature verification + tiered action + log/restore + **WinHTTP dual-source online updates (pulled forward from M4)** | None (reuses IModule/procrun/common) |
| **M2 (planned v2.14)** `purg.uninst` + `purg.unlock` | Install inventory + silent-switch library (NSIS /S, Inno /VERYSILENT, MSI /qn, InstallShield) + residue scan + handle unlock + reboot delete | M1 |
| **M3 (planned v2.15)** `purg.popup` + `purg.net` | Crosshair pick + attribution + Chromium extension parsing + homepage/association/hosts/proxy repair | M1 |
| **M4 (planned v2.16)** `purg.guard` + rule subscription | RegNotify watch + snapshot diff + WinHTTP rule subscription | M1–M3 |
| **M5 (long-term)** (✅ partially landed in v2.14: self-implemented YARA subset, user-mode kernel-module + hidden-process detection, homepage lock as its own module; full driver & libyara remain under evaluation) | Driven by M1–M4 feedback | M4 |

**M1 task breakdown (T01–T06, all done ✅)**:
- ✅ T01 signature-DB engine `ruleset.h/cpp` — 3 file-format parsers
  (UTF-16LE/UTF-8/GBK adaptive) + match index + whitelist; DB **base64-embedded**
  (avoids Defender PUA:Win32/Softcnapp content false positive); `rules\`
  overlay for updates & own additions (extra_*.txt)
- ✅ T02 enumeration layer `purgcore.h/cpp` — 11 detection points + WinVerifyTrust
  + certificate subject extraction (CryptQueryObject→CMSG_SIGNER_INFO→CertGetNameString)
  + in-process cache
- ✅ T03 action layer — backup first (purg-backup.tsv, escaped TSV) then tiered
  action + reverse-order "undo last action"
- ✅ T04 GUI — Purge Center group + purg.scan (Tweaks+Actions combo) + RunLabel fix
- ✅ T05 online updates — WinHTTP dual source + row-threshold check + SHA256 +
  `rules\update.ini` configurable
- ✅ T06 tests — tools/t213_gui_check.ps1 (GUI assertions PASS) + tools/gen_softcn.py
  generator + 4-step verdict live tests (0 MS false positives / controlled hits on
  the other three channels) + version sync 2.13

---

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Killing system components | MS/WHQL-signed allow by default + whitepath whitelist + registry backup before action |
| Signature DB licensing | SoftCnKiller repo is public & maintained; **credit the source**, adapt format without altering content |
| AVs flagging *us* | Code signing (later), transparent logging, false-positive appeals |
| 360/2345-class driver protection | Explicitly unsupported in v1 with notice; M5 evaluation slot |
| Scope creep | Strict M1–M4 milestones, each independently released |

---

## 7. Pre-work decisions (all answered at kickoff)

1. **Integration**: as a new v2.13 toolbox group "Purge Center" (chosen — reuses
   all infrastructure) vs standalone product.
2. **M1 scope**: ship `purg.scan` first (confirmed).
3. **DB source**: SoftCnKiller DB + own additions (confirmed).
4. **Online updates**: pulled forward into M1 (confirmed; originally M4).
