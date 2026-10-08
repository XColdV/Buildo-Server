# Buildo v0.01 items.dat

Read from the original `Buildo.exe` (11 Nov 2012, 970,752 bytes, SHA-256 `e72979281a39b8cd775e61b6a1584257d0ded9ba05a9de59924e86016bfb659a`).

All addresses are VAs in the original exe. "UNCONFIRMED" = not proven from client code;
those fields are not read by the client outside (de)serialization, so any value works.

## 1. Delivery flow

1. Server calls `OnInitialLogonAccepted(uint hash, string cdnHost, string cdnPath)`
   (handler 0x433350 -> worker 0x414480). Arg 0 must be variant type 5 (uint).
   arg1 -> global 0x4e6920, arg2 -> global 0x4e693c (0x4144d1 / 0x4144e6).
2. Client loads `items.dat` from its cache dir (0x414506..0x41457b), hashes the bytes with
   0x453410 (see §4). File missing -> hash stays 0 (edi zeroed at 0x41455a).
3. If hash != arg0 -> sends message type 2 text `action|refresh_item_data\n` (0x41459e, 0x4145d6).
   If equal -> parses cached file (0x4145f9 -> 0x43c090) and runs 0x414270.
   **Never send arg0 = 0**: with no cache file hash 0 == 0, the client then calls the parser on
   a NULL buffer (0x4145f8 pushes esi=0) and crashes.
4. Server answers refresh with a tank packet, type byte 0x10 (switch table 0x434418, case 16 ->
   0x434304), flags bit 0x8 set, extended data = the whole items.dat; ext size at tank +0x34.
   Client parses it (0x434315 -> 0x43c090), writes it to `items.dat` in the cache ("wb", 0x434379,
   size = tank+0x34 at 0x4343b1), then runs 0x414270.
5. 0x414270 builds a list of files needing download (§5). If the list is empty it calls
   0x413870, which sends message type 2 text `action|enter_game\n` (0x4138c7, 0x413902) and logs
   "Entering game...".

Server hash to send = hash of exactly the bytes it sends in step 4.

## 2. File layout

| Off | Size | Meaning | Evidence |
|---|---|---|---|
| 0 | u16 | version. **Not read by the client.** Use 1 (UNCONFIRMED value). | 0x43c0df only reads +2 |
| 2 | u32 | item count N | `mov edi,[ebp+2]` 0x43c0df; vector resized to N with default item, 0x43bf70 |
| 6 | ... | N item records, back to back | read pos initialised 2 then 6: `[esp+0xbc]` = local+0x10 set at 0x43c0d0/0x43c0e4, passed to each item read 0x43c14e..0x43c15b |

No trailer is read. Records go into the vector by index (record i -> slot i); every lookup
indexes the vector by item ID (`imul id,0xac`, e.g. 0x413030, 0x43e428), so the file must be
dense: record i must be item ID i, IDs 0..N-1 with no gaps.

## 3. Item record (deserializer 0x43acc0, struct size 0xAC)

Strings are `u16 length` + bytes, no terminator (0x456380 read path 0x4563c9). All ints little-endian.
Read in this exact order:

| # | Size | Struct off | Meaning | Evidence |
|---|---|---|---|---|
| 1 | u32 | +0x00 | item ID (must equal index) | 0x43acdf |
| 2 | u8 | +0x04 | material / action type (enum below) | 0x43acf7; many uses |
| 3 | u8 | +0x08 | visual effect / break-sound type. 1 -> plays `audio/punch_glass.wav` instead of `wood_break.wav` when the tile is destroyed. defs all `TILE_VISUAL_EFFECT_NONE` -> 0 | 0x433d31..0x433d66 |
| 4 | str | +0x0C | name | printed by "Selected `w%s``. Rarity..." 0x4355df |
| 5 | str | +0x2C | texture filename (relative to `game/`) | 0x43c1fa..0x43c1fe ("game/"+name) |
| 6 | u32 | +0x28 | texture file hash; 0 = never download | 0x43c1e8, 0x43c321 |
| 7 | u8 | +0x48 | "layer" column of add_tile (always 0). Client never reads it. UNCONFIRMED name | order in defs; no reads |
| 8 | u32 | +0x4C | tint colour (set_color), default 0xFFFFFFFF | ctor 0x43b44e; used as draw colour 0x435d6b, 0x439537 |
| 9 | u8 | +0x50 | frame X | 0x44452f; clothes 0x44a059 |
| 10 | u8 | +0x51 | frame Y | 0x444528; clothes 0x44a055 |
| 11 | u8 | +0x54 | storage (enum below) | 0x440f2f, 0x443ae2, 0x444522 |
| 12 | u8 | +0x58 | unknown, never read by client; write 0. UNCONFIRMED | no reads |
| 13 | u8 | +0x5C | collision: 0 NONE, 1 SOLID (default 1) | copied to tile, `tile.solid = coll != 0` 0x43e793..0x43e7a1; ctor 0x43b46c |
| 14 | u8 | +0x60 | HP = hits to break | damage capped at it 0x43e519..0x43e530; crack ratio 0x43e616 |
| 15 | u32 | +0x64 | seconds before healing | `*1000` ms 0x43e545..0x43e557 |
| 16 | u8 | +0x68 | clothing body part (enum below) | slot index 0x43d98a, 0x43dcc5 |
| 17 | u16 | +0x84 | rarity | "Rarity: %d" 0x4355df |
| 18 | u8 | +0x86 | max can hold (default 99; `set_max_can_hold|id|n`) | ctor 0x43b479; 0x43aba0 tests ==0 |
| 19 | str | +0x88 | extra file (audio). Material 6: looping music while on (0x4445d3); material 5: one-shot sound when punched (0x444600). Path used as-is, no "game/" prefix (0x43c370) | |
| 20 | u32 | +0xA4 | extra file hash; 0 = never download | 0x43c358 |
| 21 | u32 | +0xA8 | animation interval ms (default 400). Material 6 frame toggles every N ms (0x444564); material 5 stays in "hit" frame N ms after a punch (0x43e58f) | ctor 0x43b489 |
| 22 | u8 | +0x6C | seed icon base sprite (frame in seed.rttex), tinted by +0x70 | 0x44341f with colour 0x443401 |
| 23 | u8 | +0x6D | seed icon overlay sprite, tinted by +0x74 | 0x4434a3 with colour 0x44346e |
| 24 | u8 | +0x6E | tree trunk sprite (tree_trunks.rttex) | 0x444a4b, 0x445db0 |
| 25 | u8 | +0x6F | tree leaves sprite (tree_greens.rttex) | 0x444ac7, 0x445e21 |
| 26 | u32 | +0x70 | seed colour 1 = setup_seed `bg_color` (default 0xFFFFFFFF) | ctor 0x43b3ff |
| 27 | u32 | +0x74 | seed colour 2 = setup_seed `fg_color` (default 0xFFFFFFFF) | ctor 0x43b406 |
| 28 | u16 | +0x78 | setup_seed `seed1` (splice ingredient 1) | 0x43af73; no client use. Name UNCONFIRMED |
| 29 | u16 | +0x7A | setup_seed `seed2` (splice ingredient 2) | 0x43af8e; no client use. Name UNCONFIRMED |
| 30 | u32 | +0x7C | grow time in seconds (`seconds_to_bloom`); growth = elapsed/this | 0x43e684..0x43e69f |

Total bytes per record = 4+1+1+(2+len name)+(2+len tex)+4+1+4+1+1+1+1+1+1+4+1+2+1+(2+len extra)+4+4+1+1+1+1+4+4+2+2+4.

`max_fruit` from setup_seed is not in the record: the client sets a planted seed's fruit count to
3 itself (0x43ed00) and the real count comes from the server's tile data. Server-side only.

Colours: Proton `MAKE_RGBA(r,g,b,a) = a | r<<8 | g<<16 | b<<24` (file bytes A,R,G,B).
The client's colour combiner (0x454920) treats all four bytes symmetrically, so the channel
order is UNCONFIRMED from this binary; it matches Proton SDK and later items.dat files.
`set_color|255|255|255|140` -> 0xFFFFFF8C. `bg_color|96,57,19,255` (r,g,b,a) -> 0x133960FF.

Default item (ctor 0x43b3a0, used to fill the vector before parsing): material 11, storage 2,
collision 1, maxHold 99, colours 0xFFFFFFFF, +0xA8 400, everything else 0 / empty.

## 4. Enums

### Material (+0x04)
| Value | Name | Evidence |
|---|---|---|
| 0 | FIST | selected item material 0 -> punch (0x433c60, 0x436d0a, 0x441281); also tile clear path |
| 1 | WRENCH | `cmp eax,1` place logic: empty tile -> cant_place, else only on wrenchables (0x436f03..0x437036) |
| 2 | USER_DOOR | wrenchable (2..4, 0x43e48e); creates door extra type 1 (tables 0x43e83c / 0x43ed24) |
| 3 | LOCK | wrenchable; extra type 3; lock frame logic (+3 owner, +2 access) 0x444662 |
| 4 | SIGN | wrenchable; extra type 2 |
| 5 | (sound block: plays +0x88 once on punch, hit frame for +0xA8 ms) — not used by defs | 0x43e56b, 0x4445ea |
| 6 | BOOMBOX | punch toggles on (tile flag 0x40) 0x43e55c; when on animates and plays +0x88 0x444556 |
| 7 | DOOR (world main door) | extra type 1 (door) but not wrenchable; unpunchable with 9 (0x43e432) |
| 8 | no client behaviour. Candidate ROCK/WOOD. UNCONFIRMED | no compares found |
| 9 | BEDROCK (unpunchable, 0x43e3f0 returns false for 7 and 9). Name UNCONFIRMED | 0x43e432 |
| 10 | LAVA | touching -> burn handler 0x447480 (0x447527, 0x4475d5) |
| 11 | default material; no client behaviour. Candidate DIRT. UNCONFIRMED | ctor 0x43b455 |
| 12 | BACKGROUND (placed in background layer) | 0x4412de -> SetBackground 0x43e1f0 |
| 13 | SEED (plant only into empty fg; extra type 4 with plant time) | 0x4412ba -> 0x43e9a0; 0x43ecdd |
| 14 | CLOTHES (equip, not added to quick bar) | 0x43d5b3, 0x43614c, 0x43dcbf |
| 15 | special drop render (likely gems/bux, tiles_bux.rttex). Not in defs. UNCONFIRMED | 0x442cd6, 0x4454ca |

Client behaviour does not distinguish 8 from 11 (and from any value >= 16), so for DIRT / ROCK /
WOOD the server may use 11 (or 8); the only rule is: don't give ordinary blocks 5, 6, 7 or 9.
Suggested map: DIRT 11, ROCK 8, WOOD 8, BEDROCK 9.

### Visual effect (+0x08)
NONE = 0. Value 1 switches the break sound to punch_glass (0x433d37). Others UNCONFIRMED.

### Storage (+0x54)
| Value | Name | Texture grid | Frame used |
|---|---|---|---|
| 0 | none | — | tile frame 0 (0x440f5f) |
| 1 | SINGLE_FRAME_IN_TILESHEET | 32x32 px frames (0x443b1e -> 0x464c20(32,32)) | fy * framesPerRow + fx (0x444528) |
| 2 | SMART_EDGE | 8x8 frame grid (0x443b09 -> 0x464be0(8,8)) | neighbour frame from 0x4404a0 (0x440f3f); fx/fy ignored |
| 3 | (horizontal smart edge, 4x1 grid) | 0x443af4 -> 0x464be0(4,1) | neighbour frame from 0x440390 |

### Collision (+0x5C)
NONE 0, SOLID 1 (0x43e793..0x43e7a1, ctor default 1).

### Clothing body part (+0x68) — index into the avatar's 6 u16 clothing slots
| Value | Name | Texture | Evidence (frame-array offset in avatar render) |
|---|---|---|---|
| 0 | HAT (hair) | player_hair.rttex | [ebp+0] with surface +0x54, 0x44b47f |
| 1 | SHIRT | player_shirt.rttex | [ebp+4] with +0x58, 0x44b33b |
| 2 | PANTS | player_pants.rttex | [ebp+8] with +0x5c, 0x44b5ec |
| 3 | SHOES | player_feet.rttex | [ebp+0xc] with +0x60, 0x44b513 |
| 4 | FACEITEM | player_faceitem.rttex | [ebp+0x10] with +0x6c, 0x44b41c |
| 5 | HAND | player_handitem.rttex | [ebp+0x14] with +0x74, 0x44b704 |
Clothing frame = fx + fy*8 (0x44a055..0x44a05d). Surface/texture mapping from 0x44bcb6..0x44c132.

### Seeds
Seed ID = item ID + 1: the tree code reads the grown item as `fg - 1` (0x43ea1e..0x43ea2d).
The client does not synthesise seed records; every odd ID must be a real record in the file
with material 13, its own name, seed/tree sprites (+0x6C..+0x6F), colours (+0x70/+0x74) and
grow time (+0x7C). `setup_seed|<id>` lines set those fields on record <id>; odd IDs without a
setup_seed line still need a record (server picks sprites/colours; zeros render frame 0).
Odd IDs that are not used as seeds can be plain records with material 11 and empty texture.

## 5. Hash (0x453410)

```cpp
// h starts at 0x55555555; per byte: h = rol(h, 5) + byte. len 0 => hash a C string.
uint32_t BuildoHash(const uint8_t* p, int32_t len) {
    if (!p) return 0;
    uint32_t h = 0x55555555;
    if (len == 0) { while (*p) h = ((h << 5) | (h >> 27)) + *p++; return h; }
    for (int32_t i = 0; i < len; ++i) h = ((h << 5) | (h >> 27)) + p[i];
    return h;
}
```
(0x45342c..0x453449 C-string loop, 0x453450..0x453465 length loop; `shl 5` + `shr 0x1b` = rol 5.)
items.dat check: `BuildoHash(fileBytes, fileSize) == OnInitialLogonAccepted arg0` (0x41458c..0x41459a).
File hashes for textures (0x4538c0) load the file (0x49cc00) and call the same 0x453410.

## 6. File update / download (0x414270)

- For every item (0x4142e4 loop, stride 0xAC) 0x43c180 checks two files:
  - texture: only if name non-empty (+0x40 size) **and** +0x28 != 0 (0x43c1d8, 0x43c1e8).
    Path `game/<texture>`; hashed with 0x4538c0; marked for download if hash != +0x28 (0x43c321).
  - extra file: only if +0x9C size != 0 **and** +0xA4 != 0 (0x43c34c, 0x43c358). Path as stored.
- Hash 0 => file skipped entirely => no download. Send 0 for every texture/extra hash.
- Entries with the download flag (byte +0x38 of the 0x3C-byte entry, 0x414395) are kept; if none,
  0x413870 sends `action|enter_game\n` (message type 2) immediately.
- If any: "Updating items... (`w%d``/`w%d``)" and a Downloader fetches each from host = arg1
  (0x4e6920, 0x413ac4) and path = arg2 + entry path (0x4e693c + path, 0x413aa3..0x413aa9).
  Scheme/port of the downloader (0x46b060) UNCONFIRMED. With hash 0 everywhere this path never runs,
  so args 1 and 2 can be any strings (e.g. "localhost", "cache/").
