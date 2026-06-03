# Stable HomeKit IDs (no re-adding the bridge after reboots)

This explains why the bridge survives restarts without the Home app getting
"confused" and forcing you to remove and re-add it.

## The problem it solves

In a HomeKit bridge, every bridged accessory has an **AID** (Accessory ID), and
the Home app stores everything — names, rooms, automations, "which device is
this" — keyed by AID (and per-characteristic IID). Those IDs **must be identical
on every boot**, or Home thinks the bridge turned into a different set of devices.

If AIDs were assigned implicitly by *creation order*, they'd drift, because the
WiZ accessory list is built from discovery, which varies between boots:

- device **count** varies (a bulb missed/slow on one boot shifts every later AID),
- the **order** could change, and
- **DHCP** could hand a device a different IP.

Any of those reshuffles the AID→device mapping → Home's stored layout no longer
matches → you have to re-add the bridge.

## The fix: AID derived from MAC

Each WiZ device's AID is computed **deterministically from its MAC address**:

```c
aid = 100 + (low 3 bytes of the MAC)      // aidFromMac()
```

- The MAC is burned into the device and never changes (across reboots, DHCP,
  firmware updates).
- So the **same physical device always gets the same AID**, regardless of
  discovery order, count, or IP — with **no persistent storage** needed.

Static accessories get fixed low AIDs that can't collide with the MAC-derived
range:

| Accessory | AID |
|-----------|-----|
| Bridge | 1 |
| Reed contact sensor | 2 |
| WiZ devices | `100 + low24(MAC)` |

## What this guarantees

- **Restart → same AIDs** for every device present → Home keeps all names, rooms,
  and automations. No re-adding.
- **DHCP IP change** is invisible to HomeKit (identity is the MAC, not the IP; the
  new IP is just used for control/polling).

## The one tradeoff (pure-deterministic, no NVS)

Accessories are built from **live discovery** only — there's no persisted list.
So a device that is **powered off when the ESP32 boots** isn't created that boot;
it appears when the next re-scan finds it (with the *same* AID as always). During
that gap it's absent from Home and may briefly lose its tile/room until it
returns. For always-on devices this never happens.

(If you ever want offline-at-boot devices to stay present as "No Response"
instead, that requires persisting the device list to NVS — a deliberate
trade-off not taken here, in favour of simplicity.)

## Collision note

The AID uses the low 24 bits of the MAC. Two WiZ devices in one home sharing the
same low three MAC bytes is astronomically unlikely. If you ever ran an enormous
fleet and hit a clash, switching to the low 4 bytes would remove it.

## Migrating an existing install

If you previously ran a version with implicit AIDs, the WiZ AIDs change once when
you move to MAC-based IDs. **Remove and re-add the bridge a final time**; after
that, IDs are permanent and reboots are safe.
