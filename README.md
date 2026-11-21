# Raitei

Raitei is a hook DLL that makes KONAMI arcade peer-to-peer multiplayer work across the
internet with no port forwarding.

The game keeps its own matching logic: host election, handshake and in-match messages are untouched.
Raitei sits under the game's network layer and carries each peer link over a WebRTC data channel:
ICE tries a direct connection first and falls back to a TURN relay when the NATs on both ends do not
allow one. The matching server pairs the cabinets, relays the WebRTC signaling between them and hands
out short-lived TURN credentials.

Supported: SOUND VOLTEX 7 (NABLA) online matching.

## Run

Copy `raitei.dll` into the game folder and load it as a spice2x hook.

```bash
spice64.exe -k raitei.dll
```
