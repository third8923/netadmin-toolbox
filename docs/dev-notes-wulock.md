# Dev Notes · `purg.wulock` Windows Update Locking (v2.15, requirement #1)

> Research goal: reconstruct the complete "lock Windows Update" playbook used by
> ghost / ESD install images, and map it to this toolbox's lock / restore
> implementation. Researched during v2.15 development.

## 1. The four-layer image-locking playbook

### Layer 1 — Group policy registry lockdown (core)

Key: `HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate`
and its `AU` subkey: `HKLM\...\WindowsUpdate\AU`

| Value | Meaning | Locked value |
| --- | --- | --- |
| `NoAutoUpdate` (DWORD) | Disable automatic updates | 1 |
| `DisableWindowsUpdateAccess` (DWORD) | Block WU access (UX + API) | 1 |
| `DoNotConnectToWindowsUpdateInternetLocations` (DWORD) | Block WU internet locations (WSUS semantics, also effective on public network) | 1 |
| `SetDisableUXWUAccess` (DWORD) | Hide "Check for updates" in Settings (AU subkey) | 1 |
| `ExcludeWUDriversInQualityUpdate` (DWORD) | Exclude drivers from quality updates | 1 |

Ghost images also typically write `WUServer` / `WUStatusServer` pointing at a
fake 127.0.0.1 WSUS. The scanner lists these; locking does **not** write fake
addresses (leaving them empty is enough — `DoNotConnect...=1` covers it).

### Layer 2 — Service disablement (self-heal countermeasures)

| Service | Default start | Notes |
| --- | --- | --- |
| `wuauserv` | manual (demand) | Windows Update main service |
| `UsoSvc` | automatic | Update Session Orchestrator |
| `WaaSMedicSvc` | manual (demand) | **Windows Update Medic — ACL-protected; `ChangeServiceConfig` is denied.** Requires direct registry write `HKLM\SYSTEM\CurrentControlSet\Services\WaaSMedicSvc` `Start=4` |
| `DoSvc` | automatic (delayed) | Delivery Optimization |

WaaSMedic self-heal countermeasure: its `FailureActions` (REG_BINARY) contains
`01 00 00 00` (SC_ACTION_RESTART), so the service is relaunched after being
stopped. Locking patches those action bytes to `00 00 00 00` (SC_ACTION_NONE)
to prevent "disabled, then healed back".
(The key is TrustedInstaller-protected: take ownership first, or write as
SYSTEM; non-elevated runs honestly report denial and count it.)

### Layer 3 — Scheduled task disablement

All tasks in two folders set to `Enabled=false` (taskschd COM `put_Enabled`):

* `\Microsoft\Windows\WindowsUpdate\*` (Automatic App Update, Scheduled Start, SIH, ...)
* `\Microsoft\Windows\UpdateOrchestrator\*` (a dozen+ orchestrator tasks)

Tasks that refuse (protected) are recorded and skipped — no aborting, no fake success.

### Layer 4 — hosts blocking (optional toggle, off by default)

Appends a flagged comment block to `C:\Windows\System32\drivers\etc\hosts`:

```
# >>> NetAdmin Toolbox WU Lock (v2.15) >>>
127.0.0.1 download.windowsupdate.com
127.0.0.1 update.microsoft.com
... (a small set of common update domains — no long lists, to avoid collateral damage / AV heuristics)
# <<< NetAdmin Toolbox WU Lock <<<
```

Restore = delete everything between the two marker lines. Note: when policy
lockdown alone suffices, leave this off (hosts blocking of CDN-fronted domains
ages poorly; it is a last-resort layer).

### Layer 5 — Homepage locking (covered by v2.14, not re-implemented here)

The ghost-image "homepage lock" trio — SetupComplete.cmd writing Start Page,
Default User's NTUSER.DAT, browser policy Homepage — is already covered by
v2.14 `purg.homepage` (scan + repair + policy lock/unlock). See purgmods2.cpp.

## 2. Implementation map (purgmods3.cpp / purg.wulock)

| Technique | Scan (Run) | Lock (Actions[0]) | Restore (Actions[1]) |
| --- | --- | --- | --- |
| Policy lockdown | Enumerate all values under WindowsUpdate & AU | Write 5 lock values | Write back original / delete per backup TSV |
| Service disable | OpenSCManager + QueryServiceConfig + QueryServiceStatus (no sc.exe — sandbox blacklisted) | ChangeServiceConfig → DISABLED; if denied (WaaSMedicSvc) direct-write Start=4 + FailureActions 01→00; attempt ControlService STOP | Restore Start: wuauserv=demand / UsoSvc=auto / WaaSMedicSvc=demand / DoSvc=auto (original values when backed up) |
| Scheduled tasks | taskschd COM enumerate both folders (MinGW get_Item quirk: Task/FolderCollection take VARIANT subscripts — see taskmods.cpp) | All `put_Enabled(false)`, refusals recorded & skipped | `put_Enabled(true)` |
| hosts block | Check for marker block | Append only when the Field toggle is set (default off) | Remove marker block |

* Backup: `wulock-backup.tsv` next to the exe; row formats
  `SET \t HKLM \t subkey \t value \t type \t data` / `SVC \t name \t origStart` /
  `TASK \t task path \t origEnabled` (reuses purgcore `EscapeField` escaping).
* Permissions: the CLI never elevates; HKLM/service/task write failures are
  honestly counted with `ErrorTextZh` attribution (same semantics as
  purg.homepage). GUI users can hit "Restart as administrator" and retry.
