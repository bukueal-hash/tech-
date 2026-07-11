# UnknownCheats forum notes (pages 184–185)

Thread: [ARC Raiders, Structs and Offsets](https://www.unknowncheats.me/forum/arc-raiders/590414-arc-raiders-structs-offsets.html)

## Adopted (matches project CL-1315578)

| Topic | Source | Project location |
|-------|--------|------------------|
| FName decryption CL-1315578 | qwe900 #3687, Yarem #3679 | `SteamDecrypt.hpp`, `help/CL-1315578_REFERENCE.md` |
| Player name decrypt | Yarem #3681, qwe900 | `SteamDecrypt.hpp` `DecryptPlayerName` |
| Bone array decrypt | qwe900 #3687 | `SteamDecrypt.hpp` @ mesh+0x7A0, XOR `0x878588013124D57F`, LOD @ 0x830 |
| Level actors 0x108 / count 0x110 | yemmy #3678, qwe900 | `Offsets.h` |
| Full offset block CL-1315578 | qwe900 #3687 | `Offsets.h`, `SAVED_OFFSETS_LOCKED.txt` |

## Encrypted vis-check fallback (yemmy #3689)

When plain float reads at mesh `LastSubmitTime` (0x4C4) are implausible, fallback path:

- `LastRenderTimeOnScreen` encrypted @ **mesh+0x488**, XOR key `0xFA3CBF38`, then `_byteswap_ulong`
- `UActorComponent::WorldPrivate` @ **mesh+0x148**
- `UWorld::TimeSeconds` @ **worldPrivate+0x950** (double)
- Visible if `(timeSeconds - decryptedLrtos) < 0.2f`

**Note:** mesh+0x488 is a **component** offset, not actor pickup `Pickup_DefaultPickupDataAsset` (0x488).

Implemented in `Project/Functions/VisCheck.cpp` as fallback only.

## Not adopted

| Topic | Reason |
|-------|--------|
| zarboz alternate FName (p184) | Different RVAs/constants; project uses CL-1315578 scalar pipeline |
| BoneDecrypt_CL1233465 (yemmy #3683) | Multiple reports "doesn't work"; superseded by CL-1315578 help path |
