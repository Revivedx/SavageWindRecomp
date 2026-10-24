# DLC setup

The runtime can discover, list and mount separately extracted add-on content. Bring your own DLC files; no
decryption or package installation is performed. Windows gameplay has not been verified. This implements the
runtime support needed for DLC, but does not establish that every DLC quest or game mode works.

Put a `dlc` folder beside the base game's `romfs` folder. Keep each add-on's RomFS separate:

```
extracted/
  exefs/
  romfs/
  romfs.bin
  dlc/
    1/
      romfs/
        ...the first add-on's extracted files...
    2/
      romfs/
        ...the second add-on's extracted files...
```

Folder names are the add-on's actual decimal content index (0–4095), or its full 16-digit hexadecimal title ID.
For example, `01007ef00011f001` selects index 1 for base application `01007ef00011e000`. Use the index or title ID
from your extraction; assigning an arbitrary index does not turn files into a different add-on. Title IDs for a
different base application are ignored. Use only one folder per index.

For the Windows release, `extracted` is next to `savagewind.exe`. You can add the `dlc` folder there after setting
up the base game. Alternatively, place it beside the source `romfs` before choosing **Set up game files**; setup
copies it along with the game. Setting up again or deleting game files also removes the installed DLC copy, so
keep your original extraction elsewhere.

On the next game launch, the runtime builds `romfs.bin` inside each add-on folder that has `romfs/` but no image.
This may take time and needs enough free space for a second copy of the DLC data. Images are written to a temporary
file and renamed after completion. You can also supply an already extracted, raw RomFS image as
`dlc/<index>/romfs.bin`. Encrypted containers such as NCA or NSP files cannot be read.

After replacing extracted DLC files, quit the application and delete that add-on's `romfs.bin` to rebuild it at
the next launch. To uninstall an add-on, quit and remove its whole folder. Discovery happens on each game boot;
there is no live installation while the game is running. Existing save files are used normally; keep a backup
before trying DLC with a valued save.

Only available, structurally valid images are reported as installed. Missing or invalid DLC is skipped, with
diagnostics prefixed `[dlc]` in `savagewind.log` on Windows (or stderr in development builds). Check these messages
if content is missing. Base-game mod overlays and voice redirection are not applied to the separate DLC images.

## Developer checks

`make test-dlc` builds synthetic images and checks discovery, sorting, pagination, bounded IPC output, missing
content, preparation, mounting, raw reads, overlay isolation and catalog refresh between boots. It needs no game
files. The harness also supports Windows builds with the project's MSYS2 toolchain.

The service command mapping follows [Switchbrew's add-on content interface](https://switchbrew.org/wiki/NS_services#aoc:u).
Actual Windows playthroughs are still needed to verify the title screen's DLC detection, Master Mode, Trial of
the Sword, the Champions' Ballad and saves that reference DLC content.
