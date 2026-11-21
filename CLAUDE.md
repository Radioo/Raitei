# Raitei

Hook DLL that tunnels a KONAMI game's peer-to-peer matching traffic over WebRTC data channels
(ICE direct connection first, TURN relay fallback). Loaded into the game by spice2x.

## Where knowledge lives

How each game's network layer works (functions, wire formats, timeouts, re-find anchors) is
documented in the parent notes repository, not here. This repository holds code and the minimum
needed to build and run it.

## Rules

- Never write comments in source code: no inline comments, docblocks or commented-out code. Make code
  readable through naming and structure.
- Never add dead code. Every symbol added has a caller in the same change.
- Never use em-dashes or en-dashes (characters or HTML entities) anywhere.
- Reach for a maintained library before writing something yourself. Dependencies come from vcpkg.
- No absolute machine paths in tracked files. Local settings go in a gitignored file with a committed
  `.example` next to it.
- Build output, logs and captures are gitignored.

## Hooking

- Inline-hook only functions the game itself calls during matching (the game's network layer exports).
- Never inline-hook functions the AVS runtime also calls on its own threads during boot (`avs_net_*`,
  `xrpc_*` in `avs2-core.dll` / `avs2-ea3.dll`): the detour runs on those threads too and crashes the
  game around `services.get`. If such a function must be intercepted, patch only the game module's
  import table so only the game's own calls are diverted.
- Never block the game thread. Network work runs on Raitei's own threads; hooked calls only move data
  through queues.
