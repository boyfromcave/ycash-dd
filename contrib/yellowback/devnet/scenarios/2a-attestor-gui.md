# Scenario 2a — "I am an attestor": the bonding ceremony (GUI, the `attestor` seat, node 8)

```bash
yellowback-devnet up --role attestor    # ~3 min; node 8 is funded (13 YEC) and unregistered
yellowback-devnet wallet                # opens YecWallet on node 8, your seat
```

**Automated around you:** 3 pools, 3 other attestors (the layer is already ARMED on them), a
heartbeat at 15 s, the price walk, and the simulated population minting and being liquidated
against the prices you will help set.

The bond is a one-off ceremony where a GUI earns its place; everything after it (2b) is a
server that must keep running for `bondMinLock` blocks. A design pleasant in one half and
hostile in the other is not one an operator will accept.

## Walk-through

1. **Read the Attestors page before registering**, as someone who has not read the spec. Is it
   clear what an attestor *is*, and what you are about to commit?
   - notes:

2. **Register** (the Register button → `yed_registerattestor 10 200 0`). The bond locks for
   `bondMinLock = 200` blocks and earns nothing while locked. **Is that clear before you click,
   or only after?**
   - notes:

3. **The wallet.dat warning.** Both keys come from the keypool; a backup taken a minute earlier
   does not contain them, and losing it loses the bond. **Is that warning proportionate to the
   consequence?**
   - notes:

4. **Wait out `BOND_MATURITY`** (8 blocks ≈ 2 min at the default heartbeat) and watch PENDING →
   ELIGIBLE, then seating at the next snapshot. (`yellowback-devnet status` prints every
   registration with its `seq`; yours is the newest.)
   - notes:

Continue with 2b — close the GUI first.
