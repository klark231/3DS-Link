## MHGU crossplay (experimental)

> In development. The design is in place and the bridge is tested off-console; the on-console build and play testing are the work still to do. Not in a release yet.

Monster Hunter Generations Ultimate on the Switch and Android (the MHGU Recomp port) and Monster Hunter XX on the 3DS were the same game and once played together over Nintendo Network. That server is gone. MHGU Recomp players meet instead on community *hub servers* that carry its local wireless over IP. Crossplay mode points a real 3DS at those hubs: Monster Hunter XX's local play is bridged to the MHGU hub, so a console hunter is in the same gathering as MHGU players on a Switch or a phone.

It is a per-feature switch in Settings — **MHGU Crossplay**. With it on, local play does not go to an Azahar room; the plugin speaks the hub's protocol straight from the console's Wi-Fi. There is no PC and nothing else running anywhere: the conversion between the two games' wireless (the room description and the Pia packets whose layout differs between the 3DS and MHGU) happens on the console, in the plugin. It needs no account and no official server, like the rest of 3DS Link.

The bridge itself is the same code the emulator forks use for this; here it is fed the console's own captured frames instead of an emulator's. Hosting (the console hosts a gathering, MHGU players join) comes first; searching from the console for a hub to join is the next piece. See `AzaharLink/CROSSPLAY_INTEGRATION.md` for the build and the current state.
