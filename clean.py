import os
from pathlib import Path

# Dictionary mapping file paths to a list of (original_text, replacement_text) tuples.
# Note: These strings exactly match the source files (excluding the '+' prefix from the patch).
REPLACEMENTS = {
    "AzaharLink/ConfigApp/source/lobby_json.hpp": [
        (
            "// The public room list the game's plugin menu reads (public_rooms.txt next to AzaharLink.ini): one line per room, \"host:port|<players>/<max> <name>\", \n"
            "// with a third field \"?\" when the room has a password (the menu asks for it). The same line format as the saved room lists (servers.txt). The \n"
            "// plugin does no HTTP inside the game, so the app writes this after every refresh. Rooms the server marks inactive, and rooms whose address the \n"
            "// plugin could not use (anything but letters, digits . - _), are left out; every room of the page (at most `max_rooms` = what lobby::Parse keeps, 300) goes in; the whole name (100 characters at most; letters outside ASCII become \"?\").",
            
            "// Generates the public room list for the plugin menu (public_rooms.txt).\n"
            "// Format: host:port|<players>/<max> <name>|?\n"
            "// Note: Plugin does no HTTP in-game, so the app writes this after refreshing.\n"
            "// Filters out inactive rooms and invalid hostnames."
        )
    ],
    "AzaharLink/Includes/azl/chat.hpp": [
        (
            "/// The last lines received (or sent by us). Push comes from the network thread, Read from the game's frame: a tiny spin lock guards the slots.",
            "/// Thread-safe ring buffer for recent chat messages."
        ),
        (
            "/// A message longer than a line goes on over several (cut at a space when there is one; the next lines start with two spaces).",
            "/// Wraps long messages into multiple lines, attempting to break at spaces."
        ),
        (
            "/// Hides the touch screen from the game while the keyboard is open. The game reads the touch straight from the HID shared page (no service \n"
            "/// call to catch), so the only way is to clear it there. The layout of that page is not trusted: the scrubber LEARNS where the current \n"
            "/// touch lives (a word that holds exactly the position the overlay just read, a pad-state word that has the touch key set) and only ever \n"
            "/// writes to words it has seen holding the live touch. Nothing is learned when nobody touches, so nothing is written then.",
            
            "/// Scrubs touch data directly from the HID shared memory while the keyboard is open.\n"
            "/// We have to do this because the game reads the raw memory, not just service calls.\n"
            "/// It dynamically learns where the touch data lives to avoid corrupting other memory."
        )
    ],
    "AzaharLink/Sources/ctr/chat_overlay.cpp": [
        (
            "// NOT YET COMPILED against the framework in this tree: the calls below are the CTRPFramework API as the framework documents it \n"
            "// (OSD::Run with a Screen callback, Screen::Draw / DrawRect, Touch::IsDown / GetPosition). If the compiler disagrees with one of them, \n"
            "// only this file needs to change: the logic is in Includes/azl/chat.hpp and is tested on a PC (Tests/chat_test.cpp).",
            
            "// TODO: Verify against latest CTRPF API. Core logic is tested in chat.hpp, \n"
            "// so only the drawing/touch calls here should need tweaking if the API changed."
        ),
        (
            "        // The game reads the touch straight from the HID shared page, so the only way to keep a tap on a key from reaching it is to clear it there.\n"
            "        // The HID service rewrites the page about once per frame; clearing it every millisecond while the keyboard is open keeps the game's window to see it tiny.",
            
            "        // Aggressively clear the HID page (every 1ms) while the keyboard is open to minimize \n"
            "        // the game's chance of reading the touch before we scrub it."
        ),
        (
            "        // The HID service signals this event after it writes a pad / touch sample: clearing right then leaves the game almost no time to read it.\n"
            "        // (The wait clears the event first, so it can never spin; if the event does not come the 1 ms thread above still runs.)",
            
            "        // Clear the HID page immediately when the HID service signals a new sample. \n"
            "        // (Wait clears the event first to prevent spinning; the 1ms polling thread acts as a fallback)."
        )
    ],
    "AzaharLink/Sources/ctr/plugin.cpp": [
        (
            "/// A small text file of the data folder, read whole (at most 16 KB, 64 KB for the public rooms). The only reader: room lists and the chat position.",
            "/// Reads a small text file (room list, chat pos) from the data directory into memory."
        )
    ],
    "AzaharLink/Sources/main.cpp": [
        (
            "/// The public rooms, 5 to a page (public_rooms.txt: written by the Setup app's lobby page, because the plugin does no HTTP inside the game).\n"
            "/// The room visited last is always first (marked *), then the lobby's order. A tap on a room joins it (the room of the settings file is not\n"
            "/// changed). Under the rooms: \"Go to page...\", then the page buttons.",
            
            "/// Plugin menu UI for browsing public rooms. \n"
            "/// Moves the last visited room to the top. Handles pagination and password prompts."
        )
    ]
}

def main():
    base_dir = Path.cwd()
    files_processed = 0

    for rel_path, replacements in REPLACEMENTS.items():
        filepath = base_dir / rel_path
        
        if not filepath.exists():
            print(f"⚠️  Warning: {filepath} not found. Skipping.")
            continue

        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()

        # Normalize line endings to prevent mismatch issues on Windows
        content = content.replace('\r\n', '\n')
        modified = False

        for old_text, new_text in replacements:
            # Normalize the search string as well
            old_text = old_text.replace('\r\n', '\n')
            
            if old_text in content:
                content = content.replace(old_text, new_text)
                modified = True
            else:
                print(f"❌ Could not find exact match in {rel_path} for:")
                # Print the first line of the target to help debug
                print(f"   -> {old_text.splitlines()[0][:70]}...")

        if modified:
            with open(filepath, 'w', encoding='utf-8', newline='\n') as f:
                f.write(content)
            print(f"✅ Cleaned {rel_path}")
            files_processed += 1

    print(f"\nDone! Processed {files_processed} files.")

if __name__ == "__main__":
    main()
