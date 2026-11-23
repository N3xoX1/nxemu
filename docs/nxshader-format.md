# NXShader file format, version 1

An `.nxshader` file lists the shader pipelines a game draws, so that an emulator can build them
before they are first needed. One file serves every GPU, driver and operating system:

- Pipeline state is stored as the **guest** defines it: the GPU's 3D class register values and
  texture descriptor (TIC) fields. It is never stored as an emulator's host key. Each emulator projects
  the state onto its own device (dynamic state, driver quirks) when it reads the file.
- Shader programs are **not** in the file. A program is named by the start of the SHA-256 of its
  bytes and by where it lies in the player's own game files. The reader loads it from there and
  checks the hash. The file holds no guest instructions, no SPIR-V or driver binaries, nothing
  of the game's code.

The code that writes and reads it is `src/yuzu_video_core/shader_profile_codec.cpp` (container)
and `shader_profile_compact.cpp` (payload). This document is the reference: a reader built from
it alone must read every file a conforming writer makes.

## Rules for every version

These rules do not change. They are what lets the format grow without breaking old files or old
readers.

1. **Little endian** everywhere.
2. **Varint**: unsigned LEB128, 7 bits a byte, low bits first, value at most 64 bits. Writers
   use the shortest form.
3. **Text**: varint byte count, then UTF-8 bytes.
4. **SHA-256** fields are 32 raw bytes.
5. A **required** thing the reader does not know (a header feature, an extension tag with bit 15,
   a section with the required flag, a fact tag below `0x4000`, a schema) makes it refuse the
   whole file with "Shader file made by a newer version". An **optional** thing it does not know
   is skipped. New data that changes what gets compiled is always required; new data that only
   helps (hints, statistics, provenance) is always optional.
6. Anything else that does not match this document (a bad size, a value out of range, trailing
   bytes inside a section or after the frame, a duplicated tag) makes the reader refuse the file.
   Readers never guess.
7. Limits: payload at most 128 MiB; 65,536 programs, 262,144 environments, 65,536 contexts,
   8 locators per program; header extensions at most 4 KiB.

## Container

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic `NXSHADR1` |
| 8 | 4 | Major version: 1. Any other value is refused. |
| 12 | 4 | Minor version: 0. A newer minor only adds optional data, so readers accept any minor. |
| 16 | 8 | Payload size, uncompressed |
| 24 | 8 | Size of the zstd frame |
| 32 | 4 | Platform: 1 Switch, 2 Switch 2. Other values are refused. |
| 36 | 4 | Schema: 1. Other values are refused. |
| 40 | 4 | Required features: 0. Any set bit is refused. |
| 44 | 4 | Extension bytes (at most 4096) |
| 48 | n | Extensions |
| 48 + n | | One zstd frame |

**Extensions** are a list of entries: tag (u16, not 0, strictly ascending), size (u16), then the
bytes. A tag with bit 15 set is required. None is defined yet.

**The frame** is one standard zstd frame, with the content size and the checksum set and no
external dictionary. Its decoded size must equal the payload size, and nothing may follow it.

A reader refuses a file for another platform than the one it runs. The payload layout is the
same on both platforms; what differs is listed in [Platform 2](#platform-2-switch-2).

## Payload

```
varint  section count (at most 64)
count × { u32 tag (4 ASCII letters), varint flags (bit 0: required), varint size }
the section bodies, in directory order, back to back
```

Each tag appears once. Every section body must be read to its last byte. Version 1.0 writes all
of the sections below, all required, in this order: `META SRCF CTXT RSRC PROG ENVS STAT BIND RCPS`.
Readers look sections up by tag, not by position.

### META: the file

```
u64     title id
text    label (at most 4096 bytes)
text    revision (at most 32 bytes)
text    note (at most 1024 bytes)
varint  flags: bit 0, the recordings cover the whole game. Other bits are refused.
```

### SRCF: game files

The NCAs the profile was made from, so that a reader finds the same files in an NSP or an XCI.

```
varint  count (at most 1024)
count × { text name, varint size, sha256 of the first 4 KiB }
```

### CTXT: game versions

```
varint  count
count × { sha256 context, text base version, text update version }
varint  the file's own context: 0 none, else index + 1
```

A context is the identity of one game version (base and update). The version texts are only for
people.

### RSRC: resources

A resource is a run of bytes in the game files that holds programs, once decoded.

```
varint  count
count × {
  varint kind: 0 raw, 1 yaz0, 2 nso-lz4, 3 bktr-raw, 4 bktr-yaz0, 5 zstd, 6 bktr-zstd
  kinds 0, 1, 2, 5:  text file (a name from SRCF)
  kinds 1, 2, 5:     varint container offset, varint container size
  kind 2:            varint decoded size, sha256 container, sha256 segment
  kinds 3, 4, 6:     varint resource size, sha256 resource,
                     varint span count, spans × { text file, varint offset, varint size }
  kinds 5, 6:        varint decoded size,
                     varint dictionary: 0 none, else resource index + 1, then
                     varint offset, varint size, sha256 dictionary
}
```

Kinds 3, 4 and 6 are spread over an update's BKTR patch: their spans list the physical pieces.
A dictionary's resource comes before the resources that use it.

### PROG: programs

```
varint  count
count × {
  varint  size / 8
  8 bytes identity: the first 8 bytes of the SHA-256 of the program
  varint  locator count (1 to 8)
  count × { varint resource, varint ZigZag(offset - end of the previous program in this resource) }
}
```

A program is the bytes the GPU runs from its start address, header included, as the game stores
them. Programs are sorted by first locator, so offsets are mostly 0. A program both game versions hold
has one locator per version. The reader decodes the resource, takes `size` bytes at the offset
and accepts them only if their SHA-256 starts with the identity.

The emulator's own shader hash is computed from the loaded bytes, never stored.

### ENVS: environments

An environment is one program with what its translation asked the guest: the facts.

```
varint  count
count × {
  varint  program: gap after the previous environment's program (environments are sorted)
  varint  portable stage
  varint  fact count
  count × { varint tag gap, varint key, value }
}
```

**Portable stages**: 0 vertex A, 1 vertex B, 2 tessellation control, 3 tessellation evaluation,
4 geometry, 5 fragment, 6 compute; on platform 2 only, 7 task and 8 mesh.

**Facts** are sorted by (tag, key). The tag is the gap after the previous fact's tag. The key is
the gap after the previous key minus one when the tag is the same, else the key itself. The value
is 4 fixed bytes for tag 8 and a varint for every other tag. A fact that is absent is 0.

| Tag | Fact | Key | Value |
|---|---|---|---|
| 1 | Program start address | 0 | |
| 2 | Local memory size | 0 | |
| 3 | Texture bound buffer | 0 | |
| 4 | Viewport transform state | 0 | |
| 5 | Shared memory size (compute) | 0 | |
| 6 | Workgroup size (compute) | axis 0-2 | |
| 7 | Geometry passthrough mask | word 0-7 | |
| 8 | Constant buffer value | bank << 32 \| byte offset | 4 bytes |
| 9 | Constant buffer size | bank | |
| 10 | Texture table entries (compute) | 0 | |
| 11 | Texture pool entries (graphics) | 0 | |
| 12 | Sampler pool entries (graphics) | 0 | entries \| linked (bit 31) |
| 13 | Texture type | texture handle | TIC type (bits 0-3) \| normalized coordinates (bit 4) |
| 14 | Texture format | texture handle | TIC word 0 bits 0-18 \| sRGB (bit 19) |
| 15 | Constant buffer replacement | bank << 32 \| byte offset | 0 base instance, 1 base vertex, 2 draw id |
| 16 | Mapped pointer | bank << 32 \| byte offset | 1: the 8 bytes there are a mapped GPU address |
| 17 | Mapped indirect pointer | offset << 40 \| bank << 32 \| byte offset | 1: the 8 bytes at (the address at bank and byte offset) + offset are a mapped GPU address |
| 18 | 3D class register | register index | register value |
| 19 | Geometry slot header (mesh) | word 0-19 | |

Facts 5 and 6 are for compute only, 7 for geometry only, 19 for mesh only.

Facts 16 to 19 are for platform 2 only (native programs and mesh pipelines): a platform 1 file
with one is refused.

- 16 and 17: a GPU address is mapped when its first byte is. In fact 17 the bank is below 256 and
  the offset (added to the address read at bank and byte offset) below 2^24.
- 18: a register of the state class (see STAT) that the translation read, such as the mesh shader
  registers for task and mesh programs. A recipe's state still holds it too.
- 19: the first 20 words of the program the guest binds in the geometry slot of a mesh pipeline:
  only its header, which names the per-primitive outputs, is used.

Tags below `0x4000` change the compiled shader, so an unknown one refuses the file. Tags from
`0x4000` are optional and skipped.

### STAT: graphics states

Each state is the set of 3D class registers the pipeline depends on (Maxwell B197 on platform 1,
the Switch 2 3D class C797 on platform 2), as the guest wrote them, plus two keys past the register file: `0x10000` the draw's primitive topology and
`0x10001` the engine hint (1: drawn by a high-level macro). Registers are sorted by key; a
register that is absent is 0.

```
register list: base, the most common value of each register
varint  state count
count × register list: patch, where this state differs from the base (value 0 removes it)

register list = varint count, count × { varint key gap, varint value }
```

The first key is absolute, then each key is the gap after the previous one minus one.

### BIND: bindings

```
varint  graphics count
count × {
  5 stages × { varint mask of the 18 constant buffer banks, then each bank's size where its bit is set }
      (the GPU's 5 graphics program slots: vertex, tessellation control, tessellation evaluation,
      geometry, fragment; vertex A and B share the vertex slot)
  varint texture pool entries, varint sampler pool entries
}
varint  compute count
count × { varint mask of 8 banks, the sizes, varint texture table entries }
```

### RCPS: recipes

A recipe is one pipeline to build.

```
varint  count
count × {
  varint  origin: 0 recorded in play, 1 synthetic (predicted, never seen drawn),
          3 synthetic and later seen drawn
  varint  context count, then each context index (an index into CTXT, + 1)
  varint  kind: 0 graphics, 1 compute
  graphics: varint state (into STAT), varint stage mask (bit n: portable stage n, n < 6;
            on platform 2 also a mesh pipeline, see below)
  varint  binding (into BIND's graphics or compute table)
  an environment index (into ENVS) per stage in the mask, lowest stage first;
  compute: one environment, whose stage is 6
}
```

Recipes are in the order they were recorded (the oldest game version first after a merge), which
is close to the order of first use. Readers should build them in this order. Identical recipes are
stored once.

Synthetic recipes (origin 1) of one program set (the same programs at the same stages) are in rank
order, the most likely first. A reader may build only the first ones of each set that give it a
pipeline it does not have yet, and skip the rest, which are drawn less often. Recorded recipes and
those of origin 3 are always built.

A file holds only what was drawn in play when it has no recipe of origin 1. There is no flag for
this: the origins say it, and they stay true when a file is merged or exported again, where a flag
set by the file's maker would not. A file with origin 1 recipes holds guesses, such as a base pack
made from the game's files. An export from play leaves origin 1 recipes out by default, so a player
who imported a base pack and then played shares only what the game drew (origin 3 for the guesses
it confirmed).

## What the reader does

1. Check the container. Refuse another major version, a platform other than the one it runs,
   required features or extensions.
2. Read the sections. Refuse unknown required ones, skip the others.
3. For each recipe of this game version (its context), load each program from the player's files
   and check its SHA-256. A recipe with a missing or different program is skipped.
4. Rebuild each environment from its program and facts, and each guest state from its registers.
5. Project the guest state onto the device: dynamic state the device sets at draw time is left out
   of the key. Recipes that end up with the same key and the same environments are one pipeline.

## Platform 2 (Switch 2)

A platform 2 file uses the layout above with these differences. A platform 1 file uses none of
them.

- **Guest state** (STAT) holds the registers of the Switch 2 3D class (C797), with the same key
  space: register index, then `0x10000` topology and `0x10001` engine hint. The mesh pipeline
  registers are registers like any other.
- **Programs** are the native programs the game hands the GPU. Their SHA-256 is that of these
  bytes, never of a lowered or translated form.
- **Stages** 7 task and 8 mesh are allowed, with facts 16 to 19.
- **Mesh pipelines**: a graphics recipe's stage mask is either a platform 1 mask (bits below 6),
  or mesh (bit 8) with an optional task (bit 7) and an optional fragment (bit 5), and no other
  bit. Its environments come lowest stage first, like every recipe: fragment, task, mesh.
- **Slots**: as the guest binds them, a task program is in the vertex slot, and a mesh program is
  in the tessellation control slot when there is a task, else in the vertex slot. Its constant
  buffers (BIND) are those of that slot. What the guest binds in the geometry slot of a mesh
  pipeline is not an environment: the mesh program's fact 19 holds its header.
