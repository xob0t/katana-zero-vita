# Changes

Everything the port changes from the original game to run near 60 fps. The setting next to each brings back the
original; settings go in `ux0:data/gms/katanazero/yyl.cfg`.

**Everywhere**

| Change | Undo with |
|---|---|
| Enemies think and move every other frame; hits still land on the frame they would have | `enemyHalfRate=0`, or **GAME → ENEMY AI** |
| Settled smoke and burning-corpse embers update every 4th frame, smoke also while you stand still in it | `smokePlayerStill=0` (only the standing-still part) |
| Smoke is drawn with half the clouds, each one larger; the hidden ones still block sight and get pushed | `smokeClouds=1` (`smokeClouds=3`: a third, larger still; 1 to 3) |
| Swaying smoke no longer reshuffles the game's collision order every frame. Hit areas are the same; when two enemies stand in one cloud, which one it holds back can differ (in the original it changed from frame to frame) | `smokeWobble=0` |
| Lighting updates 30 times a second | — |
| Past the first 20 in a frame, effect particles are drawn every other frame | — |
| With BLOOD SPRAY off, smoke clouds show no blood drops, and the hidden drops' lifetimes (they set how many other particles spawn) are approximated | — |
| Collision checks keep their results, but when several objects overlap the probed spot, a check that names one of them may name another of them | `collisionRoundTrip=0` |
| Swaying lamps change course every 4th frame (they still move every frame, with the same random pushes) | `lampSway=1` (`lampSway=2`: every other frame) |
| Roll dust is unlit, and afterimages have no light tint | `trailLights=1` (`fogTrailLights=1`: only in the foggy rooms) |
| No glow around highlighted words in dialogue | `letterGlow=1` |
| The wavy dialogue backgrounds sway in coarser steps | `waveStrips=1` (`waveStrips=2`: half as coarse) |
| Laser deaths cut the body into fewer, thicker slices | `dissolveChunkScale=1` |
| At most 3 copies of one sound and 32 voices at once | `sameSoundLimit=0`, `maxVoices=0` |
| Checkpoints are saved in the background: a crash right after one can lose it, never corrupt it | `asyncSave=0` |
| Rain is plain dark streaks and grey splashes, without the game's fading trails and lamp-lit drops | `rain=2` (`rain=0`: no rain) |
| No end-of-level replay and no death rewind | `replays=1` |
| No touch controls: the mobile build's virtual stick and buttons are gone, and the front touch screen does nothing | `touchControls=1` (`touchControls=2`: also drawn) |

**In some rooms**

| Where | Change | Undo with |
|---|---|---|
| Club | No rim glow on the dancers | `dancerGlow=1` (in club_0 also `roomLighting=1`) |
| club_0, haunted_cart2 | No lamp glow on walls and floors | `roomLighting=1` |
| Clotheslines | No coloured light on them | `ropeLights=1` |
| Clotheslines | Ropes and their cloth move every other frame, and stop while more than 512 px outside the view | — |
| Cathedral | No dust in the window light | `cathedralDust=1` |
| Prison | Thinner fog | `fogScale=1` |
| Dossier burn | No small decorative burn circles; the burn spreads the same | `dossierCircles=1` |
| Motorcycle chase | No cone shadows; distant rockets unlit; explosions with a third of the particles | `coneShadows=1`, `farRocketLights=1`, `fullExplosions=1` |
| Psychiatrist fight, black void | No screen-copy effect pass; it changes nothing you can see there, except colours where the screen is partly transparent | `psychEffects=1` |
| Psychiatrist fight, giant | Only half the rose petals are drawn; all of them still fall and settle | `psychPetals=1` (`psychPetals=3`: a third; 1 to 4) |

**Changed back to the PC version**

| Change | Undo with |
|---|---|
| Enemies attack on the PC version's timing: the first attack 15 frames sooner and every shot (gunmen, the Headhunter, drones) 5-6 frames sooner than in the Android release. In the first room's tutorial this shows the bullet in the slow-mo; with the Android timing it stays hidden in the muzzle flash | **GAME → ENEMY TIMING: ANDROID**, or `enemyTiming=1` |

Over a long session video memory fills up. Texture pages the current room hasn't drawn are then unloaded to make
room, so most room loads only load their own pages. Some rooms need more than fits next to the earlier rooms' pages
(club_1, chinatown_3, tiedup, club_7 and a few more, and the psychiatrist's turn into the giant): there the load
still pauses for about a second while every page reloads.
