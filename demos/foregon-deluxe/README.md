# Foregon Deluxe

The Oregon journey the way a lot of people remember it from a school
computer lab: pick a profession, name your family, outfit the wagon at the
general store in Independence, and then watch the days go by. Choose the
pace and the rations, decide how to cross each river, hunt when the food
runs low, and at The Dalles take your chances on the Columbia or pay for
the Barlow Road.

Written in FC, on raylib, at a 320x180 DOS-sized screen stretched to fit,
with an emulated OPL2 for the sound.

```
./demos/foregon-deluxe/run.sh        # fetches and builds raylib on first run
```

raylib is fetched and built into `demos/shared/raylib/` on first run
(gitignored, shared with the other raylib demos). The Top Ten and the graves
are kept in `~/.foregon-deluxe/`.

Its older sibling, [foregon-frail](../foregon-frail/), is the 1971 teletype
game, rebuilt from the published 1978 listing's rules.

## Licence, and what this is after

**This directory is public domain** under the Unlicense (`UNLICENSE`, next to
this file), a deliberate exception to the repository's BSD 2-Clause licence
that covers everything in `demos/foregon-deluxe/`. The shared engine code it
builds with (`demos/shared/`, the stdlib) keeps the repository's licence.

**It is a new game in the spirit of the classroom versions, not a copy of
any of them.** The 1985 Apple II game and its DOS and *Deluxe* successors
are commercial software whose code, text, art and music belong to their
owners; none of it was consulted, and none of it is here. What this game
takes is what anyone may take: the *kinds of things* those games had you do,
the familiar shape of a day on the trail.

- **The numbers are this game's own.** Prices, speeds, rations, illness
  odds, river risks, hunting yields and the scoring table were chosen for
  this game and tuned by letting a scripted player take a few thousand
  journeys. They don't reproduce anyone else's formulas.
- **Every word is this game's own**: the menus, the instructions, the people
  you meet and what they say, the store (Hollis & Sons), the Top Ten's
  imaginary travelers, the ratings (Wagon Master, Seasoned Hand,
  Tenderfoot).
- **The art and sound are this game's own**: pixel art kept as text in
  `sprites.fc`, scenes painted in `art.fc`, a 5x8 font in `font.fc`, OPL2
  instruments in `sound.fc`. The only borrowed tunes are long in the public
  domain: Stephen Foster's "Oh! Susanna" (1848) on the title screen, and the
  opening of Chopin's funeral march (1839) at a grave.
- **The places are history.** The landmarks, forts, rivers and their rough
  mileages are the real Oregon Trail's, and the people along the way say
  things travelers of 1848 might have.

The name is the demo series' usual first-letter swap, and it keeps clear of
anyone's trademark.

## Playing

- **Menus:** press the number of a choice. **RETURN** continues and
  confirms; **ESC** backs out.
- **Typing:** names, amounts and the epitaph are typed, then RETURN. Leave a
  name blank and one is chosen for you.
- **The hunt:** walk with the **arrow keys**. You face the way you last
  walked, and **SPACE** fires that way. Animals cross from either side:
  squirrels and rabbits are quick and small, deer and elk bigger, buffalo on
  the plains and bears in the mountains bigger still. You have thirty
  seconds of light, and whatever you shoot, you can carry only 100 pounds
  back. Hunting the same country again finds it thinner. ESC walks away.
- **The Columbia:** **LEFT** and **RIGHT** steer the raft between the banks
  and the rocks. Every rock struck costs cargo, and sometimes a life.
- **F11** toggles full screen; closing the window quits.

### How the trail works

- **Professions:** a banker brings $1600, a carpenter $800, a farmer $400.
  Carpenters mend broken wagons more often; farmers' oxen go lame less
  often. The final score is multiplied by 1, 2 or 3.
- **Leaving:** March through July. Early means thin grass, cold nights and
  high rivers; late means snow in the mountains. A new year on the trail
  ends the journey.
- **Each day** the wagon covers 16, 20 or 24 miles on good ground at a
  steady, strenuous or grueling pace. Fewer oxen, poor health, the
  mountains, rain, snow or a broken wagon all slow it. The family eats 2,
  1.5 or 1 pounds a person a day on filling, meager or bare-bones rations.
- **Health** is per person. Rest, full rations and a gentle pace restore it.
  Hard driving, hunger, cold without two sets of clothing each, and illness
  wear it down. Illness is likelier in the weak. The party dies with its
  leader: if the wagon leader dies, the journey is over.
- **Rivers:** fording is safe under about two and a half feet and a gamble
  past three. Floating wants deep water and a narrow river. Ferries cost
  money and waiting; the guide at the Snake costs three sets of clothing.
  Rain raises every river, and spring melt raises them most.
- **Forts** sell supplies at prices that climb with the miles. **Trading**
  costs a day and brings one offer. A traveler with a spare ox is never far
  from a party that has none.
- **Breakdowns:** a spare part fixes a broken wheel, axle or tongue on the
  spot. Without one you might mend it or you might not, and a wagon that
  can't be mended limps on at half speed until a spare turns up.
- **Score:** points for everyone who arrives (by health) and for everything
  they bring, times the profession's multiplier.
- **Graves:** when a wagon leader dies you write the marker. Every later
  journey that passes that spot on the trail stops to read it.

## How it's built

fario's layering, with menus and typing where the pad was:

| File | What it is |
|------|------------|
| `plat.fc` | the contract: one `input` per frame (typed characters, RETURN, rubout, ESC, space, arrows) |
| `gfx.fc` | `module gfx` on raylib: a 320x180 target scaled with point sampling, audio, input |
| `lore.fc` | the route and its landmarks, the rivers, everything people say, the instructions |
| `records.fc` | the Top Ten and the graves, read and written in `~/.foregon-deluxe/` |
| `game.fc` | the rules and the screen state machine: no pixels, no keys, no library |
| `hunt.fc`, `raft.fc` | the two action scenes, stepped by `game.fc` |
| `font.fc` | the 5x8 type, kept as text, with word wrap |
| `sprites.fc` | the pixel art, kept as text |
| `art.fc` | every screen: menus, scenes, the map, the hunt, the gorge |
| `sound.fc` | OPL2 instruments and effect scripts |
| `main.fc` | the loop |
