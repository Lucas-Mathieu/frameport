# Batman audio geometry input lifetime

Batman: Arkham Shadow 1.4.1 could abort while loading a save or streaming the area
around a real-time cutscene. Unity reported requests for 26,388,238,103 or
147,520,448,640 bytes through `MetaXRAudioUnityMemoryAllocator.cpp`, followed by
`SIGTRAP` on an IL2CPP worker. The first captured stack reached
`GeometricAudioMeshPreprocessor::readMeshInfo` through
`ovrAudio_AudioGeometryReadMeshMemory`.

The serialized asset on disk was intact. In one failed run its vertex count was
read as 1 instead of 17,251. The managed-code trace showed a lifetime risk:
`LoadGeometryAsync` obtains a `NativeArray<byte>.ReadOnly` from a
`UnityWebRequest` download handler, calls async-void `LoadGeometryFromMemory`,
then returns. The background task retains the pointer/length pair, but not the
request owning those bytes. Its `GC.KeepAlive` boxes the read-only view rather
than retaining the request. The trace and device test support premature source
reuse as the cause; no hardware watch established the exact freeing instruction.

## Repair and isolation

`audio_geometry_owned.h` copies the input synchronously at
`LoadGeometryFromMemory`, before `Task.Run` can outlive the request. A mutex
protects the list of copies. The original worker still parses and processes the
audio geometry; its completion wrapper removes and frees the corresponding copy.
This also handles a worker finishing before the queuing call returns. NativeArray
length/padding bits and IL2CPP's hidden method-info arguments are preserved.
Null/invalid lengths and allocation failure retain the original call. A task
that never runs retains its copy until process exit; there is no timed expiry
that could free data beneath a late worker.

`batman_audio_geometry.h` enables the repair only in `com.camouflaj.manta`, on
AArch64, after the existing audio metadata initializer verifies Meta XR Audio
Wwise build `e1619e7fb839badf0ea5ecc22d9f028d0b17c34d`. It additionally requires
IL2CPP build `1d48a41d7e9011bef5fae0c23c404c62a4e7a73c` and exact 16-byte entry
sequences at both managed hooks. Unknown builds are left untouched. The live
IL2CPP addresses are `0x2e61bb4` (queue) and `0x2e63a28` (worker); text file
offsets in this build are 0x1000 lower. These offsets must not be reused for
another game or build.

This is separate from the existing [audio metadata retirement repair](AUDIO_METADATA.md).
It does not change the decoder, acoustic processing, tracking, foveation,
resolution, refresh rate, or other games' recipes. Temporary input dumps, native
parser bypasses and redundant late copies used during investigation are not
included. Production logs are limited to installation and the first four
copy/completion pairs. No game assets are included in the repository.

The `frame.adapter` package revision increases only for Batman. Existing Batman
installs therefore offer **Update on Frame**; other games are not marked outdated
by this repair. Updating/rebuilding Batman installs the new adapter through the
normal path; no launcher script or additional option is needed.

## Validation

- Native regressions cover deferred and inline completion, source destruction
  before worker execution, concurrent loads, cleanup, allocation failure,
  argument preservation, and rejection of unknown build IDs/function entries.
- On the headset, the affected save loaded successfully with the repair. Logs
  showed private copies of 2,599,300, 159,020 and 628,088 bytes made before dispatch;
  each original parser completed and each copy was released. Another 180,120-byte
  geometry input also completed normally.
- The game continued for over seven minutes after that load with zero allocation
  failures, rejected meshes or fatal signals. Acoustic geometry was loaded, not
  disabled. That device test used the ownership repair with diagnostic guards;
  none of the guards rejected a mesh. The production repair omits those guards.
- The rebuilt latest adapter passes the repository checks recorded in the PR;
  the device result above is for the experimental build tested before integration.

This does not establish long-term stability or resolve the separately observed
whole-headset power-offs. Those require their own power/reset investigation.
