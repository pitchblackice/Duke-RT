# LIGHTOVR Authoring Guide

`LIGHTOVR` is Duke-RT's text overlay database for authored ray-traced lighting behavior. This guide covers the file format, load/reload behavior, and the current in-game actor-light edit workflow.

## LIGHTOVR Authoring and Usage

`LIGHTOVR` is the text overlay database for RT-authored lighting behavior. The parser scans all mounted root-level `LIGHTOVR` lumps, merges them in load order, and uses last-wins replacement for duplicate rule ids within the same scope.

General workflow:

- mount a loose overlay directory with `-file M:\Raze\overlay`
- place a file at `M:\Raze\overlay\LIGHTOVR`
- use `lightoverlay_reload` console command after edits
- inspect the parsed database with `lightoverlay_dump`
- inspect the resolved current-map view with `lightoverlay_dumpresolved [mapname]`
- inspect normalized round-trippable output with `lightoverlay_dumpnormalized`
- export the normalized database with `lightoverlay_export <path>`

Current top-level `LIGHTOVR` blocks:

- `defaults`
  Placeholder root block. It currently exists for future shared defaults, but no fields are consumed yet.
- `actorrule <id>`
  Global actor-bound analytic light rule. This is the main authored point-light path for sprite-driven actors such as fires, rockets, and similar cases.
- `muzzleflashrule <event_id>`
  Global event-driven muzzle-flash rule. This binds a weapon-fire event id to a transient analytic light definition.
- `map <mapname> { ... }`
  Map-local scope for persistent placed lights and overrides.

Current map-local blocks:

- `directional <id>`
  One authored directional fill/shadow light for a map.
- `light <id>`
  A placed analytic point light anchored to a Build/world position, sector, or wall.
- `actoroverride <id>`
  Map-local actor shadow override. This adjusts shadow receive/cast policy without creating a light by itself.

Current parser fields by block:

- `actorrule`
  `actorclass`, `shadowreceive`, `shadowcast`, `fullbright`, `tile`, `type`, `color`, `intensity`, `radius`, `range`, `offset`, `direction`, `flicker`, `random`, `localspace`
- `muzzleflashrule`
  `color`, `intensity`, `intensityrandom`, `radius`, `radiusrandom`, `delayseconds`, `delayrandomseconds`, `durationseconds`, `durationrandomseconds`, `offset`
- `directional`
  `color`, `intensity`, `direction`, `angularsize`, `shadow`
- `light`
  `type`, `anchor`, `offset`, `direction`, `color`, `intensity`, `radius`, `range`, `flicker`
- `actoroverride`
  `actorclass`, `shadowreceive`, `shadowcast`

Current practical notes:

- duplicate `actorrule` and `muzzleflashrule` ids are global and last-wins
- duplicate `directional`, `light`, and `actoroverride` ids are last-wins within the same map
- actor and map analytic overlays are currently consumed as point lights on the PT path even if extra shape fields such as `range` or `direction` are authored
- `light` `anchor position` and `offset` values are authored in Build/world coordinates; the NRI renderer converts them to path-tracing render coordinates internally
- `actorrule fullbright on` forces matching actor sprite and voxel surfaces onto the PT fullbright material path so they ignore scene lighting and render at full brightness
- `actorrule random <min> <max>` adds a per-render-frame random intensity offset to the base intensity and is an alternative to `flicker`
- `actoroverride` is applied after `actorrule`, so explicit per-map shadow overrides win

Minimal example:

```text
LIGHTOVR
{
    actorrule "TrashFire"
    {
        actorclass BurningBarrel
        fullbright on
        type point
        color 1.0 0.52 0.18
        intensity 7.5
        radius 192.0
        random -2.5 2.5
    }

    muzzleflashrule "duke.shotgun.primary"
    {
        color 1.0 0.76 0.35
        intensity 18.0
        intensityrandom 0.92 1.08
        radius 224.0
        radiusrandom 0.95 1.05
        delayseconds 0.0
        delayrandomseconds 0.0 0.0
        durationseconds 0.06
        durationrandomseconds -0.01 0.01
        offset 0.0 10.0 0.0
    }

    map "E1L1"
    {
        directional "Sun"
        {
            color 1.0 0.97 0.92
            intensity 1.25
            direction 0.3 0.85 -0.4
            angularsize 0.03
            shadow on
        }

        light "LobbyFill"
        {
            type point
            anchor position 1024.0 512.0 -64.0
            color 1.0 0.84 0.6
            intensity 5.0
            radius 256.0
        }

        actoroverride "DeadPigCopNoReceive"
        {
            actorclass PigCop
            shadowreceive off
        }
    }
}
```

## Muzzle Flash Rule Breakdown

`muzzleflashrule <event_id>` defines a transient analytic light that is triggered by a weapon-fire event. The event id is matched case-insensitively against the gameplay emitters wired into the renderer bridge.

- `color <r> <g> <b>`
  Base RGB light color.
- `intensity <value>`
  Base peak intensity before per-shot randomization.
- `intensityrandom <min> <max>`
  Multiplier range applied once per shot to the base intensity.
- `radius <value>`
  Base light radius before per-shot randomization.
- `radiusrandom <min> <max>`
  Multiplier range applied once per shot to the base radius.
- `delayseconds <seconds>`
  Base delay before the flash becomes visible.
- `delayrandomseconds <min> <max>`
  Extra randomized delay range, resolved once per shot.
- `durationseconds <seconds>`
  Base visible lifetime of the flash after activation.
- `durationrandomseconds <min> <max>`
  Extra randomized lifetime range, resolved once per shot. A single authored value in normalized output becomes a symmetric signed range.
- `offset <x> <y> <z>`
  Local event-space offset from the emitted weapon origin.

Current runtime behavior:

- each shot resolves one randomized peak intensity and one randomized radius
- each shot also resolves randomized delay and duration in real seconds
- the light stays off until the resolved delay expires
- when it activates, it starts at the resolved peak intensity
- it then fades to zero over the resolved duration using an expo-out easing curve
- the transient slot topology stays stable so repeated shots do not churn PT light-history topology

Useful muzzle-flash diagnostics:

- `lightoverlay_dumpresolved [mapname]`
  Confirms that the `muzzleflashrule` was parsed and resolved.
- `nri_ptmuzzleflash_test <rule_id>`
  Queues a synthetic muzzle-flash event against a resolved rule id.
- `nri_ptstatus`
  Prints analytic-light counts, including muzzle-slot counts, once PT is active in-level.


## Actor Light Edit Mode

The actor light editor is a runtime helper for authoring global `actorrule` placeholders against live actors. It writes only to a writable loose mounted `LIGHTOVR`; archive-backed sources such as `.pk3`, `.zip`, `.wad`, `.grp`, and similar mounted bundles remain readable but are intentionally not edit targets.

Launch with a loose overlay mount so the editor has a file it can rewrite:

```powershell
build\terminal-ninja\raze.exe -nosound -file M:\Raze\overlay +set vid_preferbackend 4 +set nri_api d3d12 +map e1l1
```

Recommended setup:

- create or mount a loose overlay directory, for example `M:\Raze\overlay`
- place the writable file at `M:\Raze\overlay\LIGHTOVR`
- start a live map with the RT backend active
- run `nri_ptactorlighteditwritable` to confirm which mounted `LIGHTOVR` path will be used for writeback
- run `nri_ptactorlighteditmode 1` to enable edit mode

While edit mode is enabled:

- aiming at an actor shows the actor class through the native pickup-style notify path
- `p` prints the current target data; actor hits print class/index/position/sector/state details, and surface hits route through the RT surface-probe status hook
- `o` on an actor creates a placeholder global `actorrule`, writes the normalized database back to the mounted loose `LIGHTOVR`, and reloads overlays immediately
- `l` reloads `LIGHTOVR` without editing the file, useful after changing the file externally
- `nri_ptactorlightedittarget` performs a one-shot target sample and prints the current actor/surface/miss classification

The placeholder rule created by `o` uses the actor class as its id base and starts as a point light with warm color, `intensity 8.0`, `radius 96.0`, and zero offset. Edit the generated rule in the mounted `LIGHTOVR`, then press `l` or run `lightoverlay_reload` to apply the tuned values.

Disable edit mode with:

```text
nri_ptactorlighteditmode 0
```

## Map Light Edit Mode

The map light editor is a runtime helper for placing map-scoped `light` rules at explicit world positions. It uses the same writable loose-mounted `LIGHTOVR` writeback rules as actor light edit mode.

Recommended setup is the same as actor light editing, then run:

```text
nri_ptmaplighteditmode 1
```

While map light edit mode is enabled:

- a white point-light preview floats in front of the local camera
- `[` moves the preview closer to the camera, clamped at distance `0`
- `]` moves the preview farther from the camera
- `p` writes a map-local `light` rule at the preview position, with `type point`, `anchor position`, white color, `intensity 1.0`, `radius 200.0`, and no flicker field, then reloads `LIGHTOVR`
- `l` reloads `LIGHTOVR` without editing the file

Disable edit mode with:

```text
nri_ptmaplighteditmode 0
```
