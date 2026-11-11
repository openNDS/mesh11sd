# Keeping LuCI usable

## Goal

Leave LuCI enabled so a user can browse and change ordinary settings,
without those changes fighting mesh11sd or breaking the node on reboot.

## Sequence on a new node

1. `auto_config` runs and brings the wireless network up.
2. The daemon writes a **portal** configuration and runs
   `commit_all commit` (the same path as today's `auto_config=2`
   / `mesh_maybe_silent_commit_all` in `commit_ops.c`).
3. Only then does portal detection run.
4. If the node is not actually a portal (no upstream), the daemon
   applies the **peer** configuration as uncommitted UCI changes.

A reboot or a LuCI save therefore always lands on the committed
portal configuration. Peer is a runtime overlay only. If a peer later
gains an upstream link, it drops the uncommitted changes and returns
to the committed portal configuration.

This is a change from the current `auto_config=2` timing, which
commits after the network is already up and after detection has
chosen a role.

## Detecting a LuCI commit

The existing `_mesh11sd_default` backups (taken by
`mesh_backup_configs` on every daemon start, never overwritten)
remain the factory restore used by `revert_all revert`. They cover
`mesh11sd`, `wireless`, `dhcp`, `network`, `firewall` and `system` —
the same set `commit_all` writes (`COMMIT_PKGS` in `commit_ops.c`).

Alongside those, keep a copy of the portal configuration the daemon
itself committed, for example `/etc/config/<name>_mesh11sd_portal`.

On daemon start, compare `/etc/config` against that portal copy and
restore **only the options mesh11sd manages**. User changes to
unrelated settings (an extra SSID, firewall rules, and so on) survive.

## Settings mesh11sd must own

mesh11sd already overrides everything of consequence at runtime, so
those settings must also be configurable in the mesh11sd config.
Candidates, still to be checked against the current options:

- country code (`country` already exists)
- channel per band (`portal_channel` exists for 2.4 GHz only)
- SSID(s) (`mesh_gate_base_ssid`, `vtun_base_ssid`, `ssid_suffix_enable`)
- possibly txpower (`txpower` already exists)

## Other `portal_detect` modes

The design above is for auto-detect (`portal_detect=1`, the default).
The other modes need further consideration:

| Value | Code | Role |
| ----- | ---- | ---- |
| 0 | MRP | Forced routed portal |
| 1 | MRP or MPE | Auto detect (default) |
| 2 | — | Unused; treated as 1 (`config.c`) |
| 3 | CPE | Customer premises: backhaul is WAN, LAN is a private NAT network |
| 4 | MBP | Bridge portal (today's silent `commit_all` is already skipped here) |
| 5 | TPE | Trunk peer (documentation also calls this TPN) |

For the first release, the simplest path is to keep LuCI disabled in
modes other than 1. That is only a fallback. As those modes are
tested, it may turn out to be straightforward to leave LuCI enabled
there too; that can be decided then.

## Open points and next steps

1. Confirm the managed-option list (and add any that are missing,
   such as a 5 GHz channel).
2. First implementation step: write the `_mesh11sd_portal` copies
   when the portal configuration is committed, and do a selective
   restore of managed options on daemon start.

## Decision 1: when managed options are reverted (Rob, Nov 11 2026)

The daemon checks for and reverts changes in its main loop, not only at start.
A changed managed option (mesh key, channel, country, encryption and so on)
is already live once LuCI applies it, and on a peer it can drop the node off
the mesh immediately. Waiting for a reboot leaves the node broken and possibly
unreachable.

Guards:

- Only managed options are reverted. Other LuCI changes are left alone and
  cause no extra restart.
- Wait until LuCI's apply has finished: skip the check while a pending
  apply/rollback exists or the config file is less than about two minutes old.
- Reload only the affected package (`wifi reload` or `ifup`), not a full
  network restart.
- Log a notice naming the option and the restored value.
- Back off: if the same option keeps changing, revert once, then only warn,
  so there is never a restart loop.

## Decision 2: changes from any source (Rob, Nov 11 2026)

The loop check from decision 1 covers every source of change, not just LuCI:
LuCI Save & Apply, `uci` commands followed by a restart, or editing the
config files directly. The result is the same, so it is handled the same way.
Each pass checks:

- whether any managed option differs from the portal copy (revert as in
  decision 1);
- on a peer, whether the uncommitted peer changes are still present
  (`uci changes`); if an apply or a direct edit dropped them, re-apply them;
- whether the mesh is still up (peer links ESTAB, as the existing watchdog
  already does).

## Decision 3: changing a mesh11sd override (Rob, Nov 11 2026)

The mesh11sd config is the single place to change managed settings. A change
there (for example `portal_channel`) is legitimate: when the loop sees it, the
daemon rebuilds the portal configuration from the new values, commits it,
refreshes the `_mesh11sd_portal` copy, and applies it with one reload. A
change to the same setting made directly in wireless or network is reverted
as in decision 1.

## Decision 4: existing nodes (Rob, Nov 11 2026)

- `auto_config=2` nodes keep v7's commit-all mode with the role locked,
  unchanged. A future release may deprecate `auto_config=2`, since this
  design makes it largely redundant.
- `auto_config=1` nodes upgraded from v7 or an earlier v8 have no portal
  copy. On first start the daemon treats them as a new node: it commits the
  portal configuration, writes the `_mesh11sd_portal` copy, then runs
  detection.
- If the portal copy is missing or unreadable, the daemon rebuilds it
  rather than restoring from nothing, and logs that it did so.
