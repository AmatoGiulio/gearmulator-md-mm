# Machinedrum official OS 1.63 SysEx: ROM/RAM sample follow-up

The official-updater firmware path is considered separately from UW sample
content.

## Proven core path

With `Elektron_SPS1-1UW_OS1.63.syx` supplied locally, the reconstruction path
has demonstrated:

- exact reconstruction of the updater-programmed OS flash layout;
- normal cold boot of the reconstructed image;
- both DSPs booted;
- panel and MIDI readiness;
- firmware SysEx request/reply;
- UW first-run flash initialization and subsequent cold reboot;
- audible factory-default core synthesis.

The functional smoke result used for this split reported a default-machine peak
of approximately `0.223144`.

## Remaining sample-machine discrepancy

The same run exposed CTR, ROM and RAM machine families in the expected picker
order, selected a ROM machine and triggered it, but measured only approximately
`1.19e-07` peak.

This does not indicate a dead DSP or codec path because core synthesis is
audible in the same boot.

## Investigation

Do not assume the two 512 KiB data sections in the OS updater contain every
factory/user sample needed by ROM/RAM machines.

Determine:

1. which flash regions and DSP tables are read by a selected ROM machine;
2. whether those bytes are derivable from the official OS 1.63 updater;
3. whether first-run initialization should rebuild additional lookup/marker
   state that the emulator currently misses;
4. whether the updater factory-data banks are being interpreted correctly.

If all required data is derivable from the updater, make the existing
ROM-machine audio oracle pass from the reconstructed image. If not, keep that
sample-content dependency explicit and independent from core OS boot support.

Firmware-derived binaries, samples and raw private captures must remain outside
the repository.
