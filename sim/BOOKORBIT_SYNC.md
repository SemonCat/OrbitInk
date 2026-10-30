# Opt-in BookOrbit sync acceptance

The default simulator has no network transport. For an explicitly authorized live test:

1. Download the actual EPUB through the application's supported file download endpoint.
2. Download its supported KOReader plugin package through the authenticated web application.
   Keep that ZIP private: `bookorbit_provision.lua` contains device authentication material.
3. Snapshot the original book's plugin and web progress before any write. Use fresh test device
   IDs, never a physical reader's ID. Establish a conditional restoration plan first.
4. Set `CROSSFADE_SIM_HTTP_BRIDGE` to the absolute executable path of
   `scripts/sim_bookorbit_http_bridge.py`, and `CROSSFADE_SIM_BOOKORBIT_CONFIG` to a private JSON
   configuration file:

   ```json
   {
     "origin": "https://your-bookorbit.example",
     "document": "the_actual_32_hex_document_hash",
     "provision_zip": "/absolute/private/bookorbit-plugin.zip",
     "log": "/absolute/test/http.jsonl"
   }
   ```

   This permits only GETs for that document, authentication and server capabilities. Other
   endpoints, including stats/bookmark writes, are refused. The helper does not follow redirects.
5. For progress PUTs, also set `device_id` to the fresh simulator installation ID and `write_guard`
   to a JSON file containing the most recent owned GET response **body**. Every PUT is preceded
   by a GET and refused if the result differs. Successful PUTs are read back and logged. The guard
   advances only when the response identifies the requested document, device, position and origin
   timestamp. The server has no atomic compare-and-set contract here: the GET/PUT check narrows,
   but cannot eliminate, the concurrent-write race. Stop if another client changes the record.
6. Configure the simulator's ordinary SD stores and drive its ordinary UI. With the provisioned
   helper, SD credentials may be non-secret test placeholders: it supplies the package's native
   `x-auth-user`/`x-auth-key` at the transport boundary. Thus this tests production URL/body
   serialization and sync behavior, but not password-to-key derivation. That remains host-tested.
7. Verify normal pull, normal push, older local/newer remote, and newer local/older remote using
   saved origin timestamps and fresh HTTP readbacks. Include backwards positions. Retain logs,
   progress.bin records and screenshots. Do not count policy mocks as live acceptance.
8. Restore only your own still-current mutations using supported endpoints; remove only fresh
   test devices. Record any timestamp metadata the server cannot restore. Never overwrite a
   concurrent user's newer state to make a test look clean.

`CROSSFADE_SIM_RTC_UNSET=1` models a present RTC that lost its valid time until firmware calls
`syncFromNTP()`. It does not change existing progress timestamps. The simulator's NTP operation
uses host time; it cannot prove UDP/NTP reachability or RTC writes on hardware. This seam exposes
the saved-WiFi recovery regression: before recovery, actual page turns save timestamp zero and
both automatic and manual sync preserve the server record without pushing. After recovery,
subsequent real page turns acquire origin timestamps and can push. Already-unknown saved events
remain unknown until another genuine navigation; reconnecting must never stamp them as new.

The owned bridge clears host input edge latches before abort polling to mirror the device
`InputManager::update()` semantics. Otherwise the simulator retains the Confirm edge that entered
manual sync and cancels its first HTTP request, an artifact of the pinned host HAL rather than
production behavior. No dependency or generated source is patched.
