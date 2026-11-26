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
| Highlighted words in dialogue glow as before, but drawn in one batch: the glow's outline can sit a tiny fraction of a pixel off | `letterGlow=1` (`letterGlow=0`: no glow) |
| The wavy dialogue backgrounds sway in coarser steps | `waveStrips=1` (`waveStrips=2`: half as coarse) |
| The paused room picture behind the pause and options menus is drawn in one pass instead of ten stacked layers; about 20 of its 518,400 pixels per frame differ by 1/255 | `pauseStack=0` |
| Laser deaths cut the body into fewer, thicker slices | `dissolveChunkScale=1` |
| At most 3 copies of one sound and 32 voices at once | `sameSoundLimit=0`, `maxVoices=0` |
| Checkpoints are saved in the background: a crash right after one can lose it, never corrupt it | `asyncSave=0` |
| In slow motion the HUD's battery meter throws no sparks. Everything else on the HUD stays, and the game's random numbers come out the same | `hudSparks=1` |
| Rain is plain dark streaks and grey splashes, without the game's fading trails and lamp-lit drops | `rain=2` (`rain=0`: no rain) |
| No end-of-level replay and no death rewind | `replays=1` |
| No touch controls: the mobile build's virtual stick and buttons are gone, and the front touch screen does nothing | `touchControls=1` (`touchControls=2`: also drawn) |
| The options menu has no CONTROLS page: it only set up the touch controls (stick, interact and cutscene buttons, left-handed layout, button placement). Their saved values are kept | `touchControls=1` |
| The game starts in its controller mode instead of waiting for the first stick move: prompts show buttons from the first frame ("press attack to begin planning", not "tap screen"), and slashes follow the stick's exact angle as on PC | `touchControls=1` |
| Five **GRAPHICS** options are off by default: RESTART FX, BLOOM FX, REWIND FX, SHADERS and BLOOD SPRAY. A fresh install and the game's own option resets start with them off. A saved choice is kept, so an option you had saved on stays on. Bloom, RESTART FX and REWIND FX also need SHADERS on, and REWIND FX only shows with `replays=1`. Measured on the Vita with each one on: bloom drops mansion_3 to 30 fps from 54 and bunker2_3 to 40 from 47; blood spray adds 0.2 ms per frame across club_4_2 and about 1 ms while 8 to 10 drops are in the air; SHADERS with bloom off costs nothing measurable (mansion_3, bunker2_3, factory_0); the restart effect's frames (about 0.3 s per restart) take 18 to 20 ms, as the room's other frames do | **GRAPHICS** → the row: ON, or `bloomDefault=1`, `restartFxDefault=1`, `rewindFxDefault=1`, `shadersDefault=1`, `bloodSprayDefault=1` |

**In some rooms**

| Where | Change | Undo with |
|---|---|---|
| Club | The dancers' rim glow is one outline per side with all the nearby lights mixed into it, not one per light; only the outline corners can differ | `dancerGlow=1` (in club_0 also `roomLighting=1`; `dancerGlow=0`: no glow) |
| club_0, haunted_cart2 | No lamp glow on walls and floors | `roomLighting=1` |
| Clotheslines | No coloured light on them | `ropeLights=1` |
| mansion_2_2, mansion_3 | The flamethrower's flames don't light up the room. They burn, spread and alert enemies as before | `flameLights=1` |
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
| Enemy melee cooldowns are the PC ones, 4 frames shorter: after every melee attack (pistol-whips, grunts, brawlers, the red pompadour) and after a gunman's shot | the same |
| A slash can't kill through the temporary door a broken door leaves behind | the same |
| A slash that opens a door stops being deadly on the next frame instead of 2-3 frames later, and stays turned the way the door turned it | the same |
| An enemy that loses you in smoke stops falling at once (Android lets it fall for 3 more frames) | the same |
| The slow motion after you die starts on the 6th frame, during the hit freeze, and ramps down more gently; the Android release starts it once the freeze ends and ramps it faster | the same |
| Level time limits are the PC ones, two thirds of the Android release's (club_0: 8100 instead of 11850 frames) | **GAME → TIMER & SLOW-MO: ANDROID**, or `timerSlowmo=1` |
| Zero's slow-mo meter is the PC one, half as long (about 6 seconds instead of 13) | the same |

**Options (off by default)**

| Option | What it does | Turn on with |
|---|---|---|
| Steadier lag mode | The game turns rim shading off while the frame rate is low and tries it again once it recovers. In chinatown_4 that try fails 4 times in the first ~10 s of every entry and restart, each time with a quarter second at ~30 fps, before the game gives up for the room. With this option the game gives up after the first failed try, and a room that failed once starts later entries and restarts with rim shading already off. Rim shading then stays off where the game would have shown it for a moment between the stutters; the rest of the room looks as in the game | `lagLatch=2` (`lagLatch=1`: without remembering the room) |
| 30 fps in combat | Rooms with enemies or a boss (70 level rooms, the Headhunter, Kissyface, V, the bike chase and the psychiatrist fight) run at a steady 30 fps with the game's clock doubled, so they play at full speed instead of slowing down below 60. Every other room, the tutorial's scripted part and every cutscene stay at 60. At 30 the game reads input 30 times a second, and a few things the game counts in frames run at half speed in real time: the death slow-mo, the pause menu's effects and the lamps' sway. Decided when a room starts or restarts; a change applies from the next room | **GRAPHICS → 30 FPS IN COMBAT: ON**, or `combat30=1` |

The ENEMY TIMING and TIMER & SLOW-MO choices are saved with the game options. The time limits change from the next
room or restart, the slow-mo meter from the next spawn, enemy attack timing for enemies that appear after the change,
the other ENEMY TIMING rules at once.

Over a long session video memory fills up. Texture pages the current room hasn't drawn are then unloaded to make
room, so most room loads only load their own pages. Some rooms need more than fits next to the earlier rooms' pages
(club_1, chinatown_3, tiedup, club_7 and a few more, and the psychiatrist's turn into the giant): there the load
still pauses for about a second while every page reloads.
