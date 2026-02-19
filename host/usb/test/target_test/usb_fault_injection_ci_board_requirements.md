### Hardware Requirement List (English)

**Title:** USB Fault-Injection CI Board Requirements (USB2 + USB Type-C CC)

**1. Goal**

- The board shall simulate USB fault conditions for CI testing without physically unplugging a cable.
- All controlled signals shall be independently switchable.

**2. Independently Controllable Signals**

- `VBUS (5V)`: connect/disconnect
- `D+`: connect/disconnect
- `D-`: connect/disconnect
- `CC1`: connect/disconnect
- `CC2`: connect/disconnect

**3. Required Switching Profiles**

- Clean unplug/plug: disconnect then reconnect `VBUS + D+ + D- + CC1 + CC2`
- VBUS-only drop: disconnect only `VBUS`, keep `D+/D- + CC` connected
- DATA-only drop: disconnect `D+ + D-`, keep `VBUS + CC` connected
- Partial DATA fault: disconnect only `D+` or only `D-`
- CC-only detach/attach: disconnect/reconnect only `CC1 + CC2`
- Partial CC fault: disconnect only `CC1` or only `CC2`
- Orientation flip (Type-C):
- Normal: `A.CC1 <-> B.CC1`, `A.CC2 <-> B.CC2`
- Flipped: `A.CC1 <-> B.CC2`, `A.CC2 <-> B.CC1`
- Glitch/bounce mode: rapid toggling of selected net/group (`VBUS`, `DATA`, or `CC`)

**4. Minimum Dynamic Capability**

- Single disconnect pulse width: `10 ms` to `10 s`
- Cycle mode: `50-200` disconnect/reconnect cycles, period `0.5-2 s`
- Bounce/glitch: `2-5` toggles within `50-200 ms`

**5. Electrical Behavior Requirements**

- "Disconnect" must be true electrical open (`Hi-Z`), not forced low/high.
- Channels must be independent (no unintended coupling between switched lines).
- Must be controllable from external GPIO (firmware/timing handled in software).
