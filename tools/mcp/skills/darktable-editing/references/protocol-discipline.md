# Protocol discipline

Cross-call rules that no single tool docstring can express. The
docstrings are authoritative on argument *shapes* — read them for that
and do not look for a second copy here.

Source: `tools/mcp/src/darktable_mcp/server.py`,
`docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
at commit `7537128c55`.

## Revisions and compare-and-swap

The session carries a monotonic `revision`. Every mutation advances it.
Every read returns it.

Mutating tools: `set_module_params`, `set_module_enabled`,
`reset_module`, `create_module_instance`, `undo`, `create_mask_shape`,
`update_mask_shape`, `delete_mask_shape`, and state-changing
`set_mask_attachment`.

All of them accept an optional `expected_revision`. **`undo` requires
it** — there is no unguarded undo, by design.

Pass the revision you last observed. If it no longer matches, the call
fails `revision_conflict`, retryable, **changing nothing**. That is not
a transient error to paper over: a human moved a slider, and any plan
you built on the old state may now be wrong. Re-read, re-plan, then act.

Omitting `expected_revision` means "apply regardless". Only do that for
the first mutation after a fresh read, and never in a session where the
user is actively editing alongside you.

## Reads are free

These never change the edit and never advance the revision:

`get_current_image`, `list_modules`, `get_module_schema`,
`get_module_params`, `get_history`, `render_preview`, `get_scopes`,
`list_mask_shapes`.

Read as often as you need. The only cost of `render_preview` and
`get_scopes` is tokens and a little of darktable's CPU.

## Patch atomicity

`set_module_params` is all-or-nothing. One bad field fails the whole
request with `invalid_value` or `unknown_field` and **nothing changes** —
you never end up half-applied. Send a coherent group of fields together
rather than a sequence of single-field calls: fewer history items for
the user to wade through, and one revision instead of five.

The same call carries `values` (native scalars), `curves`, `vectors`,
`bands`, `quantities`, and `blend`. A semantic ID given in two of the
class arguments is rejected client-side before any wire traffic.

## Enabling is explicit

`set_module_params` does not enable a disabled module. Pass
`enable: true` to switch it on in the same history step. Setting
parameters on a disabled module is legal and silent — the user sees no
change and you will think the edit failed.

## Instance addressing

`instance` is the module's `multi_priority`, not its position in the
list. `0` is the default or only instance. `list_modules` reports the
live ones. A module flagged `ONE_INSTANCE` rejects
`create_module_instance` with `instance_not_supported`.

`get_module_schema` is **per-op, not per-instance** — one schema covers
every instance of `exposure`. Current *values* are per-instance
(`get_module_params`).

## History and undo granularity

`get_history` returns metadata only — `seq`, `op`, `instance`,
`display_name`, `instance_name`, `enabled` — oldest to newest, no
parameter blobs. `limit` defaults to 20, clamped to [1, 100]. For values,
call `get_module_params`.

**One `undo` reverts one history item.** Some operations record more
than one:

| Operation | History items |
|---|---|
| `set_module_params`, `set_module_enabled`, `reset_module` | 1 |
| `create_mask_shape` without `attach` | 1 |
| `create_mask_shape` with `attach` | 2 (shape, then membership) |
| `delete_mask_shape` | 1 + one per module whose base group is retired |
| `set_mask_attachment` that changes state | 1 |
| `set_mask_attachment` idempotent detach | 0 (revision unchanged) |

So reverting a create-with-attach takes two `undo` calls. This is a
known granularity wart, not hidden batching. Check `get_history` rather
than assuming.

## Preview-pipe freshness

Mask coordinate calls need a valid preview pipe. After a
distortion-changing edit (`crop`, `ashift`, `lens`, `flip`), the first
coordinate call enqueues a reprocess and polls. If it times out you get
retryable `retry_later` — wait and retry once.

This polling happens on darktable's GTK main thread, so it can briefly
stall a co-located user's UI. Do geometry edits first, then masks, not
interleaved.

## Limits

- Frames cap at 16 MiB; overflow is `request_too_large`.
- `render_preview`: `max_px` clamped to [64, 2048], `quality` to
  [50, 95]. Use 512 when you only need to check that an edit went the
  right direction.
- `get_scopes`: `image_size` clamped to [128, 1024]. `include_bins`
  adds 256 per-channel numbers — leave it off unless you need them.
- Preview and mask renders return **native image blocks**, never base64
  text. `get_scopes` and mask renders return mixed content: one JSON
  text block, then the images.
