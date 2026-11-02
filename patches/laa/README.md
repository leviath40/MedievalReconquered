# Large Address Aware

Sets the Large Address Aware flag on the exe so the (32-bit) game can use 4 GB of address space on
64-bit Windows instead of 2 GB. One header bit (`IMAGE_FILE_LARGE_ADDRESS_AWARE`) plus the PE checksum.

    python patches\laa\apply_laa.py [--exe IN] [--out OUT]

Default: reads `MedievalReconquered.exe`, writes `MedievalReconquered_laa.exe`. Combine with the icon patch by
chaining (`--exe` = output of the other script). Risk: old code that treats pointers as signed breaks only once
it gets addresses above 2 GB, so play-test large battles and long campaigns.
