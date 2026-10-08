# Buildo v0.01 session and player protocol

Read from the original `Buildo.exe` (11 Nov 2012, 970,752 bytes, SHA-256 `e72979281a39b8cd775e61b6a1584257d0ded9ba05a9de59924e86016bfb659a`).

Addresses are client VAs. Unless marked UNCONFIRMED, each claim was read from the disassembly.

## 0. Framing

- ENet message = u32 message type + payload.
  - Client text sender 0x43c760: `u32 type` + text bytes + **one extra trailing byte** (packet length = strlen+5,
    enet_packet_create with NULL data, so the last byte is uninitialised). Server: strip trailing byte/NULs. Channel 0, reliable.
  - Client tank sender 0x43c4d0(type=4, hdr, 0x38, ext, peer, flags): `u32 4` + 56-byte header (+ ext data when flags&8).
- Server→client message types, dispatcher 0x40caf0, jump table 0x40d628:
  - 1 → HELLO: client builds and sends the login (0x40cb59). Payload only needs to be ≥4 bytes (0x43c600).
  - 2 → "Got unknown packet type" (0x40d5ee). **Never send type 2 to the client.**
  - 3 → text command handler 0x4324c0 (section 4).
  - 4 → tank packet, 0x4338d0. Validated by 0x43c5b0: total len ≥ 0x3c; if header flags (+0xc) & 8, len ≥ 0x3c + extSize(+0x34).
  - 5 → 0x40d4d4: wraps the text in VariantList(text, u32 5) and fires a signal at this+0x1c. UNCONFIRMED purpose; the server does not need it.
- Tank header (56 bytes, offsets from after the u32 4): +0 u8 type, +4 netID, +0xc flags (bit 8 = ext data present),
  +0x14 int (item id / call delay), +0x18 float x, +0x1c float y, +0x20 float vx, +0x24 float vy, +0x2c int tileX,
  +0x30 int tileY, +0x34 ext size, then ext data at +0x38.

## 1. Function calls (tank type 1)

- Ext data = VariantList. Arg 0 = function name. The handler receives the list with arg 0 removed (0x4579c0 copy-from-1).
- Routing (0x433961): netID(+4) == -1 → global function map. Then delay(+0x14) == -1 → call immediately (0x4601e0 path);
  otherwise the call is queued with that delay (0x45d400).
  netID != -1 → NetAvatar with that netID (0x40dd90 lookup) → its map at avatar+0x38, always queued with delay +0x14.
  Missing avatar → "Server wants to call %s ... on netObj %d, but it doesn't exist".
- Recommendation: global calls netID=-1, delay=-1. Avatar calls netID=target, **delay=0** (the queue path sets
  deliver time from the value; -1 is not special there, 0x45cef0).
- Variant types read by the handlers: 1 float, 2 string, 3 vec2, 4 vec3, 5 u32, 9 s32. Handlers coerce an empty slot
  to the expected type with a 0 value, so a missing trailing arg reads as 0/empty.

### Global handlers (registration 0x434600..0x435200, handler address precedes the name push)

| Call | Handler | Args (after name) | Effect |
|---|---|---|---|
| OnInitialLogonAccepted | 0x433350 → 0x414480 | u32 itemsHash, string host, string path | v1 stored at 0x4e6920, v2 at 0x4e693c (used by file updater 0x413aa3/0x413ac4: host + path prefix for texture downloads; UNCONFIRMED exact URL shape). Loads cached `items.dat`, hashes it (0x453410), compares with v0. Different → sends type 2 `action|refresh_item_data\n` (0x41459e). Same → loads it and runs 0x414270 (file check, then enter_game). |
| OnRequestWorldSelectMenu | 0x431c80 | string (ignored) | Clears world/GUI (0x430940, 0x431b20) and builds the "WorldSelectMenu" (0x42f220): text box "World Name:", default = saved var `lastworld` else "START", button "Enter World", "Back". The string arg is not parsed (passed only when the menu already exists, then also unused). |
| OnSpawn | 0x4316f0 | string (key/value text) | See 1.1 |
| OnRemove | 0x430e80 | string `netID|<n>` | Removes that net object. |
| OnConsoleMessage | 0x431220 | string | Adds a console line (0x4222b0). `` ` `` colour codes are used by the client's own strings. |
| OnTalkBubble | 0x432af0 | u32 netID, string text, u32 a, u32 b | Bubble over avatar netID (must exist, else "Unable to find netObj %d"). a and b go to 0x40f5d0; b is used as a bool. UNCONFIRMED meaning of a/b; send 0,0. |
| OnDialogRequest | 0x4314d0 | string | "GenericDialog". Ignored ("Ignoring dialog request") if a GenericDialog is already open. Format in 1.3. |
| OnZoomCamera | 0x433050 | float zoom, u32 ms | |
| OnPinchMod | 0x432f30 | float | Scales the "scale2d" of the world view. |
| OnFailedToEnterWorld | 0x4309d0 → 0x42de60 | none | Re-enables the WorldSelectMenu/"Start" button after a failed join. |
| OnSetBux | 0x4330f0 | s32 (type 9) | Sets the bux/gem counter (0x438a80, 0x438a60). |
| OnReconnect | 0x4309b0 | none | Tears down GUI (0x415860) and reconnects (0x414e00). |

### 1.1 OnSpawn text (0x4316f0)

Key lookup 0x43c7c0 = plain substring search for `key` anywhere in the text, value runs to `\r`/`\n`/NUL.
Ints via atoi (0x43ca20), vec2 `key|x|y` (0x43c8e0), rect `key|a|b|c|d` (0x43d340).

Keys read: `spawn|`, `type|`, `posXY|`, `netID|`, `userID|`, `name|`, `country|`, `colrect|`. Nothing else
(no mstate/smstate/invis/onlineID).

- `spawn|avatar` required (compare 0x431871); any other value → nothing is created.
- netID → avatar netID (0x468ba0; stored at avatar+0x30). userID → avatar+0x194. name → 0x468c30. country → 0x447cb0.
- `type|local` (0x43195b) → local player: input component 0x44c780 and net-sync component 0x44c600, and
  game+0xe0 = avatar. Anything else → remote component 0x44c340.
- colrect = x1|y1|x2|y2: collision size = (x2-x1, y2-y1) via 0x4498a0. Send `colrect|0|0|20|30`.
- posXY = pixel position, written straight to avatar+4/+8 (32 px per tile).

Example:
```
spawn|avatar
netID|1
userID|1
colrect|0|0|20|30
posXY|96|704
name|PlayaGrip
country|us
type|local
```
(omit `type|local` for other players.)

### 1.2 Avatar handlers (registration 0x448da0, called with netID = avatar)

| Call | Handler | Args | Effect |
|---|---|---|---|
| OnSetPos | 0x4489d0 | vec2 | Position = vec2; velocity zeroed. |
| OnSetFreezeState | 0x448a60 | u32 state | Stored at avatar+0x190. 2 → 0 on the local player sets state flag 0x2000 and forces a resend (respawn effect, 0x44a4c0). UNCONFIRMED full meaning of values (2 = frozen/dead). |
| OnPlayPositioned | 0x447300 | string file | Plays a sound at the avatar (0x431050). |
| OnKilled | 0x447660 | none | Scream sound, death anim (0x44b730). On the local player sets state flags 0x900 and forces a reliable state send. |
| OnSetClothing | 0x448ad0 | vec3 a, vec3 b, u32 skin, u32 playSound | Six clothing slots as u16: a.x, a.y, a.z, b.x, b.y, b.z (slot index = item field +0x68). Then skin colour (0x449ff0). playSound≠0 → change_clothes.wav. Local player also syncs inventory equip flags (0x43dac0). UNCONFIRMED slot→body-part names (likely hat, shirt, pants, shoes, face, hand). |
| OnNameChanged | 0x4476c0 | string | New display name. |
| OnChangeSkin | 0x448a20 | u32 colour | |
| OnAction | 0x4471f0 | string | `/dance` or `/wave` animation; else "Unknown action". |

### 1.3 Dialog text (parser 0x41aa00..0x41b800, lines split on `|` and `\n`)

Same shape as later Growtopia. Minimum token counts (including the command):
`add_label|big/small|text|left/center|` (≥4), `add_label_with_icon|size|text|align|itemID|` (≥5),
`add_textbox|text|align|` (≥3), `add_player_picker|name|text|` (≥3), `add_button|name|text|` (≥3),
`add_checkbox|name|text|0/1|` (≥3), `add_text_input|name|label|default|maxLen|` (≥5),
`end_dialog|name|cancelText|okText|` (≥4), `add_spacer|...`, `disable_resize`, `set_default_color|`, `embed_data|k|v`.
Reply (0x418115, type 2): `action|dialog_return\ndialog_name|<name>\n` + embed_data lines + `buttonClicked|<btn>\n` +
`<input>|<text>\n` / `<checkbox>|0/1\n`. UNCONFIRMED exact order of the reply lines.

## 2. Client → server, in order

1. HELLO received → **type 2** login (0x40cb59..0x40d4aa):
   ```
   [tankIDName|<n>\ntankIDPass|<p>\n]   (only if the "tankid_checkbox" var is set)
   requestedName|PlayaGrip
   protocol|1
   game_version|0.01
   hash|<int>
   hash2|<int>
   platformID|0
   deviceVersion|0
   country|us
   [reconnect|1]                       (when reconnecting, 0x40d48b)
   ```
2. Server: OnInitialLogonAccepted(hash, host, path).
3. Client, if hash mismatch: **type 2** `action|refresh_item_data\n`. Server: tank 16 with items.dat as ext data;
   client writes cache `items.dat`, loads it, then 0x414270.
4. 0x414270 checks per-item texture files (0x43c180: skipped when filename empty or texture hash == 0;
   else compares `game/<file>` hash and queues a download). With no downloads queued, 0x413870 sends
   **type 2** `action|enter_game\n` (0x4138c7). Send texture hash 0 to avoid the HTTP updater.
5. Server should then send: **tank 9 inventory** (this is the only server packet that closes the "InitConnection"
   dialog: 0x4131e0 callers are 0x41487b (Cancel) and the tank-9 case 0x434130), OnSetBux,
   OnRequestWorldSelectMenu, optional OnConsoleMessage.
6. "Enter World" → **type 3** `action|join_request\nname|<WORLD>\n` (0x42e296). Text box content is saved to
   `lastworld`. UNCONFIRMED whether 0x423ba0 upper-cases the name; uppercase it server-side.
7. Server: tank 4 world, then OnSpawn (local, `type|local`) and OnSpawn for each other player; send the new
   player's OnSpawn (without type|local) to the others. On failure: OnFailedToEnterWorld.
8. Nothing else is sent on world load; the local avatar starts the state stream (section 3).
- In-world menu (0x4298e0): **type 3** `action|quit_to_exit` (no newline, 0x429bd8); **type 2**
  `action|respawn\n` (0x429c96); **type 2** `action|growid\n` (0x429d44).
  Server for quit_to_exit: OnRemove to others, then OnRequestWorldSelectMenu to the player.
  Server for respawn: OnKilled (avatar call), then OnSetPos(spawn) and OnSetFreezeState(0). UNCONFIRMED timing.
- Chat (0x415fc5): **type 2** `action|input\n|text|<msg>` — no trailing newline, only sent while connected
  (game+0x7cc +0xb5). The client does not draw its own message; the server echoes OnTalkBubble(netID, msg, 0, 0) and
  OnConsoleMessage to everyone in the world.
- Inventory menu: **type 2** `action|drop\n|itemID|<id>\n` (0x41ed5b), `action|info\n|itemID|<id>\n` (0x41ee8b).
- Skin: **type 2** `action|setSkin\ncolor|<n>\n` (0x4334ae, when "sendSkinColor" var set after map load 0x43403d).
- Quit from the main menu: type 2 `action|quit` (0x40988b).

## 3. Tank packets

### Client → server
| Type | Sender | Fields | Reliability |
|---|---|---|---|
| 0 state | 0x44c650 every 200 ms (0xc8) from the local net-sync component; filled by 0x4467d0 | +0x18/+0x1c pos (rounded to int), +0x20/+0x24 velocity, flags: 0x10 facing left, 0x20 (avatar+0xe4, UNCONFIRMED: on ground/jumping), plus one-shot event bits from avatar+0x18c (0x4 first packet/teleport, 0x40, 0x80, 0x800\|0x400 punch, 0x900 death, 0xa00 OnPlayPositioned, 0x1000 jump, 0x2000 respawn, 0x4000 pickup), +0x2c/+0x30 punched tile x/y or -1. netID left 0. | Changed state: unreliable (flags 0) unless a forced send; first unchanged repeat sent once reliable. |
| 3 tile change | 0x430f00 | +0x14 item id (18 = fist), +0x2c/+0x30 tile x/y, flags 0x10 if facing left | reliable |
| 7 tile activate | 0x430c90 | +0x2c/+0x30 tile x/y (door enter etc.) | reliable |
| 10 item activate | 0x430cf0 | +0x14 item id (equip/use) | reliable |
| 11 object pickup | 0x431bb0 | +0x14 dropped-object id | reliable |

### Server → client (switch 0x434418)
| Type | Handler | Use |
|---|---|---|
| 0 | 0x433936 → avatar vtbl+0x10 (0x446480 → 0x44c270) | Remote player state. Byte 0 must be 0. Copy the sender's packet, set +4 = sender netID, relay to others. Flag 4 = snap position. Sent to the local player's own netID it is ignored ("sent to a local player, that's weird"). |
| 1 | 0x433961 | Variant call |
| 3 | 0x433c03 | Tile change: netID +4, item +0x14, tile +0x2c/+0x30. Item material (item+4) 0 = fist/punch; material 0xd = seed (tree_plant.wav). For the local avatar a placed item is removed from the inventory (0x43d800(item,1)). Applied by 0x43f270. |
| 4 | 0x433ff4 | World (ext data → 0x43f450) |
| 5 | 0x433f28 | One tile update, ext data = serialized tile, x/y at +0x2c/+0x30 |
| 6 | 0x433f86 | Several tile updates: ext = repeated (x u32, y u32, tile…) ended by x = -1 |
| 8 | 0x433b26 | Tile damage (punch anim on netID +4, wood_damage.wav) |
| 9 | 0x434130 | Inventory: closes InitConnection; ext = u8 (UNCONFIRMED: version/slots, send 1), u8 count, count × {u16 itemID, u8 amount, u8 flags (bit0 = equipped; only for material 0xe clothes)} (0x43db00) |
| 12 | 0x433e72 | Tree state (+8 == 1 damage sound, -1 harvest by netID) |
| 13 | 0x433e26 | Remove item from inventory: id +0x14, amount byte +2 |
| 14 | 0x434173 | Dropped object change: netID -1 spawn at +0x18/+0x1c; netID ≥0 or -2 = collect object id +0x14 (item 0x70 adds to bux) |
| 15 | 0x433e59 | Generic world apply (0x43f270) |
| 16 | 0x434304 | items.dat as ext data |
| 2, 7, 10, 11 | 0x4343e0 | "we don't know how to handle it yet" |

## 4. Server text commands (message type 3, handler 0x4324c0)

Must start with `action|`, else "bad net game message".
- `action|log\nmsg|<text>` → console line (0x423920).
- `action|play_music\nfile|<path>` (`stop` stops).
- `action|play_sfx\nfile|<path>\ndelayMS|<n>`.
- `action|add_notification\nimageFile|<rttex>\nmsg|<text>\naudioFile|<wav>\ndelayMS|<n>`.
- `action|logoff` → disconnects (0x499e30).
- `action|logon_fail` → 0x409840: sends `action|quit` and disconnects; use `action|log` first to show a reason.
- Anything else → "Unknown server message".

## 5. Accounts

- GrowID fields only appear when the saved var `tankid_checkbox` is set; values from vars `tankid_name`,
  `tankid_password` (0x40cbf3..0x40ccda). The in-world menu can send `action|growid\n`.
- Rejecting a login: `action|log\nmsg|...` then `action|logon_fail` (type 3).

## 6. Item facts seen along the way

- Hash 0x453410 = Proton file hash: h = 0x55555555; for each byte h = ((h<<5) + (h>>27)) + byte (32-bit). Run over the whole file.
- Item material (item+4): 0 = fist, 0xd = seed, 0xe = clothes. item+0x68 = clothing slot. item 0x70 (112) is the bux/gem pickup.
- Texture download is skipped when texture hash (item+0x28) is 0 or the filename is empty (0x43c1d8, 0x43c1e8).
