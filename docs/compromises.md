# Compromises

Everything the port changes from the original game to run near 60 fps. The setting next to each brings back the
original; settings go in `ux0:data/gms/katanazero/yyl.cfg`.

**Everywhere**

| Change | Undo with |
|---|---|
| Enemies think and move every other frame; hits still land on the frame they would have | `enemyHalfRate=0`, or **GRAPHICS → ENEMY AI** |
| Settled smoke and burning-corpse embers update every 4th frame | — |
| Smoke is drawn with half the clouds, each one larger; the hidden ones still block sight and get pushed | `smokeClouds=1` (`smokeClouds=3`: a third, larger still; 1 to 3) |
| Lighting updates 30 times a second | — |
| Past the first 20 in a frame, effect particles are drawn every other frame | — |
| With BLOOD SPRAY off, smoke clouds show no blood drops, and the hidden drops' lifetimes (they set how many other particles spawn) are approximated | — |
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

Over a long session video memory fills up. When it does, the next room load pauses for about a second while the
textures reload.
