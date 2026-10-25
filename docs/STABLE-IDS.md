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

## Saved device list (NVS)

Stable AIDs alone aren't enough. When HomeKit connects to the bridge it compares
the accessory list with what it has stored, and **deletes every bridged
accessory that's missing** — together with its name, room and automations. If
the device comes back later, Home treats it as brand new.

HomeSpan starts answering HomeKit within seconds of boot, but WiZ discovery runs
in a background task and takes ~10 s. So if accessories were built from live
discovery only, your home hub would connect while just the bridge and the door
sensor exist, and Home would drop every WiZ device on **every** restart.

So every device the bridge has ever bridged is saved to NVS (namespace
`wizbridge`: MAC, type, module, firmware, last IP) and **recreated in `setup()`,
before HomeKit can connect**. Discovery then only refreshes IPs and adds new
devices, and the saved copy is updated when an IP or firmware version changes.

## What this guarantees

- **Restart → same accessories, same AIDs** → Home keeps all names, rooms and
  automations — even for a device that is powered off while the ESP32 boots.
- **DHCP IP change** is invisible to HomeKit (identity is the MAC, not the IP; the
  new IP is just used for control/polling).

## Devices that are gone for good

Because the list is saved, a device you remove from your home stays in Home
(showing its last state; HomeSpan can't mark a bridged accessory "No Response").
To remove it, type this in the serial monitor (MAC from the boot log or the Home
app's serial number field):

```
@f9877d5af83d2
```

That deletes its accessory from Home and from the saved list. If the device is
actually still on the network, the next scan adds it back as a new accessory.

A factory reset (serial `F`, or holding BOOT 10 s) clears the list too, so the
bridge starts completely fresh: HomeSpan only erases its own data, so on the
next boot the sketch sees HomeSpan's identity and WiFi both gone and wipes the
list (`[WiZ] HomeSpan was factory reset - cleared the saved device list`).
Erasing all NVS (serial `E`) clears it as well.

## Collision note

The AID uses the low 24 bits of the MAC. Two WiZ devices in one home sharing the
same low three MAC bytes is astronomically unlikely. If you ever ran an enormous
fleet and hit a clash, switching to the low 4 bytes would remove it.

## Migrating an existing install

If you previously ran a version with implicit AIDs, the WiZ AIDs change once when
you move to MAC-based IDs. **Remove and re-add the bridge a final time**; after
that, IDs are permanent and reboots are safe.

The **first boot with the saved device list** starts with an empty list, so Home
drops the WiZ devices one last time (exactly what happened on every reboot
before). Once they reappear, set their names and rooms again — from then on
they survive restarts.
