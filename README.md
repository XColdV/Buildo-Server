# Buildo-Server

A server emulator for **Buildo**, the original Growtopia prototype from November 2012 (`Buildo.exe` v0.01).

Seth Robinson shared the archived 2012 build back in August 2026, but the original server has been gone for over a decade. This project lets you connect and play on a local or private server with an unmodified client.

You will need the client files from Seth's archive:
<https://rtsoft.com/temp/OldAssBuildoWinFrom2012.zip>

## What works

Pretty much everything you need for regular gameplay:

- Guest and GrowID login (saved accounts)
- World generation and world saving
- Breaking and placing blocks, gem drops, and seeds
- Tree planting, growth timers, and harvesting
- Inventory, clothing, and skin colors
- Locks: the original 200-tile area lock, plus Small, Big, Huge, and World Locks
- Signs and doors (wrench to edit sign text or door destinations)
- Multiplayer with chat, movement sync, `/wave`, and `/dance`
- Dynamic `items.dat` built on the fly from game definitions

## How to run

1. Download and extract Buildo.
2. Add this line to your `hosts` file (`C:\Windows\System32\drivers\etc\hosts`):
   ```
   127.0.0.1 rtsoft.com
   ```
   Buildo checks `rtsoft.com/growtopia/server_data.php` to find the server address.
3. Put `buildo-server.exe` and `extra_items.txt` inside your Buildo game folder (next to `Buildo.exe`).
4. Run `buildo-server.exe`.
5. Open `Buildo.exe` and click **Play Online**.

### Playing with friends

If you want to host for friends over LAN or VPN:
```
buildo-server --host buildo.lan
```
Have everyone add your server IP to their `hosts` file pointing to both `rtsoft.com` and your hostname (e.g. `192.168.1.50 rtsoft.com` and `192.168.1.50 buildo.lan`).

## In-game commands

- `/item <id> [count]` - Spawn items
- `/items` or `/find <name>` - Search item IDs
- `/who` - See who is online
- `/wave`, `/dance` - Emotes
- `/respawn` - Return to the entrance door
- `/gems` - Check your gem count
- `/help` - Show all commands

Console commands in the server window: `players`, `say <text>`, `save`, `stop`.

## Locks

The original 2012 build only had one lock (ID 60, "Lock"), which claimed 200 tiles around it. We also added Small, Big, Huge, and World Locks in `extra_items.txt`.

Wrench any lock you own to give access to friends, clear access, or re-apply the covered area.

## Building

To build the executable yourself (requires MSVC Build Tools, x86):
```bat
build.bat
```

For reverse engineering notes and packet specifications, check out the [`docs/`](docs) folder.

## License

MIT. Buildo and Growtopia assets belong to their original owners.
