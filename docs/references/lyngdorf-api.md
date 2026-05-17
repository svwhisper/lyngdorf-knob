# Lyngdorf control API — what we know

Two parallel control surfaces. The amp accepts both; we use each for what it
does well.

## Streaming / playback API — HTTP/JSON on port 8080

This is the surface for anything related to *what's playing* (the streaming
module on the amp). It's the same StreamMagic-derived API that KEF, Linn DS,
and Steinway-Lyngdorf processors expose — so working code from any of those
projects is a useful template.

### Endpoint shape

```
GET http://<amp-ip>:8080/api/setData
        ?path=<dotted-resource-path>
        &roles=<role>
        &value=<URL-encoded-JSON>
```

The "GET despite the name `setData`" thing is intentional — the streaming
module models commands as state writes. The amp toggles state internally,
so the same command can be used for both directions of a binary action.

### Confirmed control values (player:player/control, role=activate)

| Action      | JSON value sent                  | Notes                                                |
| ----------- | -------------------------------- | ---------------------------------------------------- |
| Play/pause  | `{"control":"pause"}`            | Acts as a toggle regardless of current state         |
| Next track  | `{"control":"next"}`             | Confirmed via siegeld/steinway_lyngdorf              |
| Prev track  | `{"control":"previous"}`         | Confirmed via siegeld/steinway_lyngdorf              |
| Stop        | `{"control":"stop"}`             | Confirmed via hilli/go-kef-w2 (untested on Lyngdorf) |

The URL-encoded form of `{"control":"next"}` is
`%7B%22control%22%3A%22next%22%7D` — see `metadata_play_pause()` in
`main/metadata.c` for the snprintf template.

### Reading state (getData)

```
GET http://<amp-ip>:8080/api/getData?path=player:player/data&roles=title,mediaData,value
```

The response is a JSON array; the playback payload is the first array
element that's an object containing a `trackRoles` key. Our
`fetch_now_playing()` in `metadata.c` does exactly that walk — see comments
there about why the spec's `"player:player/data"` marker string is
unreliable in practice.

## RIO control API — TCP on port 84

This is the surface for *everything else about the amp* — volume, mute,
source selection, RoomPerfect focus, voicings, balance, tone controls,
power. Track navigation is **not** on this surface; it lives only on the
HTTP/JSON streaming API above.

Each command is a single text line starting with `!`, terminated by
carriage return. Examples:

| Command       | Effect                                                       |
| ------------- | ------------------------------------------------------------ |
| `!VOLCH(N)`   | Step volume by N (positive or negative, 0.1 dB units)        |
| `!VOL?`       | Query current volume                                         |
| `!MUTE(ON)`   | Mute                                                         |
| `!MUTE(OFF)`  | Unmute                                                       |
| `!MUTE?`      | Query mute state                                             |
| `!SRC(N)`     | Select input source by index                                 |
| `!RP(N)`      | RoomPerfect focus index                                      |
| `!VOI(N)`     | Voicing index                                                |
| `!VOIUP`      | Next voicing                                                 |
| `!VOIDN`      | Previous voicing                                             |
| `!BAL(L5)`    | Balance left/right (e.g. L5, R3, 0)                          |

Mute syntax was a sharp edge — `!MUTE` and `!MUTEON` are both rejected;
parens are required.

## Important: which surface for what

| Action                 | Surface                | Reason                                         |
| ---------------------- | ---------------------- | ---------------------------------------------- |
| Volume change          | RIO TCP                | Sub-50 ms latency over a kept-open socket      |
| Mute toggle            | RIO TCP                | Same — fast confirmation                       |
| Play / pause           | HTTP setData           | Streaming module owns playback, not RIO        |
| Next / previous track  | HTTP setData           | Same — RIO has no track-skip commands          |
| Now playing metadata   | HTTP getData           | Polls every 3 s (panel awake) or 60 s (idle)   |
| Power on/off           | RIO TCP (`!PWRON/OFF`) | Streaming API doesn't reach the amp section    |

## Sources

- [siegeld/steinway_lyngdorf](https://github.com/siegeld/steinway_lyngdorf) — definitive next/previous JSON values for `player:player/control`.
- [thejens/lyngdorf-mcp](https://github.com/thejens/lyngdorf-mcp) — most complete RIO TCP command reference; confirms RIO does not cover track navigation.
- [fishloa/lyngdorf](https://github.com/fishloa/lyngdorf) — Python library with the official PDF spec files committed in `spec/` (TDAI-3400, TDAI-2170, TDAI-1120, MP-40/50/60).
- [homeassistant-projects/hass-lyngdorf](https://github.com/homeassistant-projects/hass-lyngdorf) — Home Assistant integration (does not implement track navigation; useful for source / RoomPerfect handling).
- [DaftMunk/tdai2170pi](https://github.com/DaftMunk/tdai2170pi) — TDAI-2170 RS-232 control (older protocol generation).
- [jsoutter/uc-intg-lyngdorf](https://github.com/jsoutter/uc-intg-lyngdorf) — Unfolded Circle integration.
- [Lyngdorf TDAI-1120 External Control Manual](https://lyngdorf.steinwaylyngdorf.com/downloads/lyngdorf-tdai-1120-external-control-manual/) — official PDF (download link on the page).
