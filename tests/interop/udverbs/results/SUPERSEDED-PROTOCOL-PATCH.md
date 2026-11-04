# Superseded reference-dependent experiment

The reference-dependent results in `dgram-final-*` directories and associated
provenance/audit files used the additional reference ROD GoBackN patch in
commit `3fba0c7`. That patch has been removed. These are historical evidence,
not results for the original reference protocol. Current reruns use the
`dgram-original-protocol-*` prefix; see the performance README.

Native-only tests in the old experiment did not use the reference protocol;
the unchanged native SUET installation remains at `03f9f4d`. Original CRC-on/off
results predate the withdrawn protocol patch and are preserved separately.
