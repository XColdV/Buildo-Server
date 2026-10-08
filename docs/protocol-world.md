# Buildo v0.01 world, tile and inventory packets

Read from the original `Buildo.exe` (11 Nov 2012, 970,752 bytes, SHA-256 `e72979281a39b8cd775e61b6a1584257d0ded9ba05a9de59924e86016bfb659a`).

All integers little-endian. "str" = u16 length + bytes, no terminator (0x456380).
Tank packet = u32 message type 4, then 56-byte header, then ext data if flags & 8.
Header offsets below are relative to the start of the 56-byte header.

| off  | size | name used here |
|------|------|----------------|
| 0x00 | u8   | packet type |
| 0x01 | u8   | b1 (object flags for type 14) |
| 0x02 | u8   | b2 (count / tree flag) |
| 0x03 | u8   | b3 (tree flag) |
| 0x04 | i32  | netID |
| 0x08 | i32  | aux (type 12 tree state, type 15 tile count) |
| 0x0C | u32  | flags (8 = ext data follows; 4 = snap position; 0x10 = facing left) |
| 0x10 | f32  | float (type 14: drop count as float) |
| 0x14 | i32  | intData (item id / damage / object id) |
| 0x18 | f32  | x (pixels) |
| 0x1C | f32  | y |
| 0x20 | f32  | speedX |
| 0x24 | f32  | speedY |
| 0x28 | u32  | unused here |
| 0x2C | i32  | tileX |
| 0x30 | i32  | tileY |
| 0x34 | u32  | ext data size |

Client sends tank packets as 4 + 0x38 + ext + **1 trailing byte** (0x43c4d0: size = hdr+ext+5).
Server should accept >= 60 bytes. Tank dispatcher: 0x4338d0, table 0x434418.

## Tank packet types (server → client)

| type | handler | name | fields used |
|---|---|---|---|
| 0 | 0x433936 | State (remote avatar movement) | netID picks avatar; whole header copied (0x44c270). Must not be the local player's netID ("sent to a local player, that's weird", 0x446492). flags&4 = snap. |
| 1 | 0x433961 | CallFunction (VariantList in ext) | netID -1 = global; intData(0x14) = delay ms, -1 = immediate |
| 3 | 0x433c03 | TileChangeRequest (apply) | tileX, tileY, intData = item id, netID = actor (-1 none). b2/b3 + flags&0x10 used for trees. |
| 4 | 0x433ff4 | MapData | ext = World (below), uncompressed |
| 5 | 0x433f28 | SendTileUpdateData | tileX, tileY; ext = one Tile (net mode) |
| 6 | 0x433f86 | SendTileUpdateDataMultiple | ext = repeat { i32 x; i32 y; Tile } until i32 x == -1 |
| 8 | 0x433b26 | TileApplyDamage | tileX, tileY, intData = hits to add, netID = puncher (punch anim; -1 none). Plays wood_damage. |
| 9 | 0x434130 | SendInventoryState | ext = Inventory (below). Also kills the "InitConnection" menu (0x4131e0). |
| 12 | 0x433e72 | TreeState / harvest | tileX, tileY; aux = -1 → clear tile (harvested); aux == 1 → damage sound; else aux = grow-time add, intData low byte = fruit count, b2==1 → tile flag 0x08, b3==1 → flag 0x10 |
| 13 | 0x433e26 | ModifyItemInventory | intData = item id, b2 = amount to **remove** (0x43d800) |
| 14 | 0x434173 | ItemChangeObject (dropped items) | see below |
| 15 | 0x433e59 | SendLock | see below |
| 16 | 0x434304 | items.dat (ext = file) | — |
| 2,7,10,11 | 0x4343e0 | unhandled ("Server sent game packet type %d…") | — |

## Tank packets the client sends

| type | sender | meaning | fields |
|---|---|---|---|
| 0 | 0x44c650 / fill 0x4467d0 | movement state, every ~200 ms when changed | x,y (rounded to .5), speedX/Y, flags (0x10 = facing left, 0x20 = 0xe4 state, plus pending 0x18c bits), tileX/tileY = punch/target tile or -1. netID left 0. |
| 3 | 0x430f00 | punch / place | tileX = tile.x, tileY = tile.y, intData = item id (18 = Fist for punch, from 0x4493b0), flags |= 0x10 if facing left |
| 7 | 0x430c90 | enter door | tileX, tileY of the door tile (sent from 0x4493b0 when touching a door; client freezes itself first) |
| 10 | 0x430cf0 | item activate (e.g. wear clothes) | intData = item id (from inventory UI 0x436050) |
| 11 | 0x431bb0 | pick up dropped object | intData = objectID |

Text packets the client sends (message type 2): `action|join_request\nname|<world>`,
`action|input\n|text|<chat>`, `action|respawn\n`, `action|quit_to_exit`, `action|drop\n|itemID|`,
`action|info\n|itemID|`, `action|dialog_return\ndialog_name|`, `action|setSkin\ncolor|`,
`action|enter_game\n`, `action|refresh_item_data\n` (strings at 0x4c9d98..0x4cb684).

Client-side placement flow (0x430f00): online, it only sends type 3; nothing changes until the
server echoes a type 3. When the echoed type 3 has netID == local avatar and the item's
max-can-hold byte (item+0x86) != 0, the client removes 1 of that item itself (0x433c95 →
0x43d800(item,1)). So: do not also send type 13 for normal placement.

## Map data (type 4 ext) — World::Serialize read path 0x43f450

```
u16  version                 -> world+0x38 (never checked)
u32  flags                   -> world+0x74 (never checked by load)
str  name
-- TileMap 0x441b60 --
u32  width                   (tile x/y stored as u8 per tile → keep <= 255; 0x43e210)
u32  height
u32  tileCount               (= width*height; row-major, index = y*width + x, 0x430e10)
Tile[tileCount]
-- WorldObjectMap 0x43ffc0 --
u32  objectCount
u32  lastObjectID            (client assigns new drops ++lastObjectID, 0x43ff83)
Object[objectCount]
(end — nothing else is read)
```
No decompression. Prints "Loaded map, %d bytes, decompressed. …" with the consumed size.

### Tile (net mode) — 0x43e850
```
u16 foreground
u16 background
u16 parent        -> tile+0x34 (lock parent index)
u16 flags
if flags & 0x02:  u16 lockParentIndex -> tile+0x34 again
if flags & 0x01:  TileExtra
```
Flag bits seen: 0x01 extra present, 0x02 locked (0x43e2e0), 0x04 and 0x40 cleared when fg = 0
(0x43e7a6), 0x40 = toggled state (material 6 on hit, material 5 on timer, 0x43e55c),
0x08/0x10/0x20 tree state (0x43f2f1..0x43f31e, UNCONFIRMED meaning).
Send flags = 0 for plain tiles; 0x01 when the fg item needs an extra (see below).

After reading fg/bg the client rebuilds the extra for the fg item (0x43e6d0): if the fg item's
material is one that owns an extra, a default extra is created and flag 0x01 is set by the
client, then the serialized extra (if any) overwrites it.

### TileExtra — 0x43f070 (first byte = extra type)
| type | made for item material (0x43ed24 table) | net-mode layout (read) |
|---|---|---|
| 1 door | 2 and 7 | str label (+0x28), u8 flags (+5) |
| 2 sign | 4 | str text (+0x28), u32 (+8, UNCONFIRMED: author id) |
| 3 lock | 3 | u8 flags (+5), u32 ownerUserID (+8), u32 accessCount, u32[accessCount] |
| 4 tree | 13 (seed) | u32 growSeconds (+0x64), u8 fruitCount (+0x68) |

File/save mode door (arg5 = 0) is 3 strings (+0xc, +0x28, +0x44) + u8; the network packet always
uses the 1-string form (World::Load passes 1, 0x43401b).

### Object — 0x43f990 (16 bytes)
```
u16 itemID
f32 x, f32 y   (pixels)
u8  count
u8  flags
u32 objectID
```

## Dropped objects — type 14 (0x434173 → 0x440150)
- netID == -1: **create**. item = intData, x = f32 @0x18, y = f32 @0x1c, count = (int) f32 @0x10,
  flags = b1. objectID = ++lastObjectID on the client (server must mirror the counter). Plays
  object_spawn2.
- netID >= 0 or -2: **remove** object id = (u16) intData. If netID == local avatar's netID the
  client adds item/count to its inventory (item 112 instead adds to a counter at +0x130,
  UNCONFIRMED: bux); other netIDs play the pickup animation.
- Client pickup request = type 11 with intData = objectID.

## Lock — type 15 (0x433e59 -> 0x43f270 -> 0x441070)
tileX, tileY = lock tile; intData = lock item id (placed as fg); netID = owner **userID**
(0x43ee10); aux (0x08) = N; ext = u16[N] tile indices that become locked (parent = lock
tile's index, flag 0x02).

What the client does with it, in order:
1. Sets the lock tile's foreground (0x43e6d0) and the owner in its lock extra.
2. Unlocks every tile whose parent is this lock (0x440cb0 -> 0x43e2e0(0)), so a type 15
   always replaces the lock's previous area rather than adding to it.
3. Locks each listed index (0x43e2e0(lockIndex): parent = index, flag 0x02).

Parent 0 means "not locked" (0x43e2e0 clears flag 0x02 for 0), so a lock can't sit at tile
index 0. Replacing a lock tile's foreground, e.g. breaking it with a fist, also runs 0x440cb0
(called from 0x43e770), so the client frees the area itself. A type 5 tile update of the lock
tile does the same, so send the type 15 again after one.

The lock sprite sheet (`tiles_lock.rttex`, 4 frames of 32x32) is plain / open / red / green;
the client picks the frame from the owner and access list in the lock extra (0x444662).
The client has no notion of lock sizes. The area is entirely the server's list.

## Inventory — type 9 ext (0x43db00)
```
u8  unknown (+4, UNCONFIRMED: version/size; send 1)
u8  count
count × { u16 itemID, u8 amount, u8 flags }   flags bit0 = equipped
```
Equipped items whose material is 14 (clothes) are put on using item+0x68 (body part)
(0x43dcbf).

## Item fields seen along the way
- item+0x04 = material/action type. Seen values: 0 Fist (punch, 0x433c60), 2 and 7 door extra,
  3 lock, 4 sign, 5 timed toggle, 6 toggle on hit, 12 background (SetTile → bg, 0x4412de),
  13 seed (plant, 0x4412ba; tree_plant sound 0x433daa), 14 clothes (0x43dcbf).
  Door is 7 and user door is 2; see protocol-items.md.
- item+0x60 = HP (hits to break; damage capped at it, 0x43e519).
- item+0x64 = seconds before healing (×1000, 0x43e545).
- item+0x68 = clothing body part.
- item+0x86 = max can hold; 0 = infinite / not consumed (0x43aba0).
- item+0xa8 = ms for material-5 toggle timer.

## After the map loads
0x433ff4 → 0x4336b0 builds the world GUI, loads the map, then optionally sends
`action|setSkin` if the `sendSkinColor` var is set. The local player only appears through
`OnSpawn` (keys parsed at 0x4316f0: `spawn|`, `type|`, `posXY|`, `netID|`, `userID|`, `name|`,
`country|`, `colrect|`, value `avatar`, `type|local`). The client does not pick a spawn door
itself; the server places the player with posXY.

Tile pixel size is 32 (0x4cbf50); tile (x,y) pixel pos = x*32, y*32.
