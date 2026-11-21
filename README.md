# Raitei

Raitei (雷霆, "thunder") is a hook DLL that makes KONAMI arcade peer-to-peer multiplayer work across the
internet with no port forwarding.

The game keeps its own matching logic: host election, handshake and in-match messages are untouched.
Raitei sits under the game's network layer and carries each peer link over a WebRTC data channel:
ICE tries a direct connection first and falls back to a TURN relay when the NATs on both ends do not
allow one. The matching server pairs the cabinets, relays the WebRTC signaling between them and hands
out short-lived TURN credentials.

## Status

Design stage. The first target is SOUND VOLTEX 7 (NABLA).
