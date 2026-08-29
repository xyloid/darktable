# Running the AI features from an MCP build tree

`ubuntu-mcp-client-setup.md` builds with `--disable-ai` and runs the binary
straight out of `build-mcp/bin/darktable`, never running `cmake --install`.
If you drop `--disable-ai` to use the AI modules (object masks, AI denoise,
upscale), two things that a real install would have set up are missing. Both
show up only at runtime, and both surface as the same unhelpful message on
the "add AI object" mask tool:

```text
object mask preparation failed
```

That string comes from `src/develop/masks/object.c` and covers every failure
of the background encode thread, so always get the real reason from the log
first:

```sh
build-mcp/bin/darktable -d ai --configdir ~/.config/darktable-mcp-dev ...
```

Look for `[darktable_ai]`, `[ai_models]`, and `[segmentation]` lines.

## 1. ONNX Runtime is not in the plugin directory

Symptom in the `-d ai` log:

```text
[darktable_ai] failed to load ONNX Runtime library 'libonnxruntime.so':
    .../build-mcp/lib/darktable/libonnxruntime.so: cannot open shared object file
[darktable_ai] failed to init ONNX runtime API
[segmentation] failed to load encoder for mask-object-sam21-small
```

On Linux, `darktable_ai` is compiled with `ORT_LAZY_LOAD` and opens ORT by
bare filename at runtime (`src/ai/backend_onnx.c`), trying, in order:

1. `g_module_open("libonnxruntime.so")` — the system linker search path,
   which does *not* include the build tree's `_deps/`
2. `$plugindir/libonnxruntime.so` — i.e. `build-mcp/lib/darktable/`

CMake only populates the second location with `install(FILES ...)`
(`src/ai/CMakeLists.txt`), an install-time rule. Running from the build tree
skips it, so the bundled library stays in `build-mcp/_deps/onnxruntime/lib/`
where nothing looks for it.

Fix — mirror what `install` would place there:

```sh
cd build-mcp/lib/darktable
for f in libonnxruntime.so.1.24.4 libonnxruntime.so.1 libonnxruntime.so \
         libonnxruntime_providers_shared.so; do
  ln -sfn "../../_deps/onnxruntime/lib/$f" "$f"
done
```

Verify the exact path darktable probes actually loads and exports the symbol
it looks up (`OrtGetApiBase`):

```sh
python3 -c "import ctypes; h=ctypes.CDLL('$PWD/libonnxruntime.so'); print(bool(h.OrtGetApiBase))"
```

These symlinks live in the build tree, so a clean rebuild or a fresh CMake
configure of `build-mcp/` removes them. The durable alternative is an
absolute path in preferences -> AI, which is checked *before* the plugindir
fallback:

```text
plugins/ai/ort_library_path=<REPO>/build-mcp/_deps/onnxruntime/lib/libonnxruntime.so
```

Note the auto-downloaded ORT package here is CPU-only — there is no
`libonnxruntime_providers_cuda.so` — so `plugins/ai/provider=auto` resolves
to CPU even on an NVIDIA machine. That is a clean degrade, not an error:
`_resolve_provider` probes each execution provider before selecting it.

## 2. Models land in a snap-confined data directory

Symptom in the `-d ai` log:

```text
[darktable_ai] ID not found: mask-object-sam21-small
```

darktable resolves its models directory as
`g_get_user_data_dir()/darktable/models` (`src/ai/backend_common.c`), which
follows `XDG_DATA_HOME`. Snap-confined editors — VS Code and friends — export

```text
XDG_DATA_HOME=$HOME/snap/<snap>/<rev>/.local/share
```

to everything started from their integrated terminal. A darktable launched
from such a terminal downloads its models into a snap *revision* directory;
the next snap update bumps `<rev>` and the models silently disappear from
darktable's view. Launching the same binary from a plain terminal cannot see
them either.

`desnap-models.sh` (in `tools/mcp/`) migrates any such models to
`~/.local/share/darktable/models` and clears the snap-side leftovers. It is
dry-run by default, refuses to run against a live darktable, and never
touches `~/snap/darktable` — that belongs to the Snap Store darktable
package, not to this leakage.

```sh
tools/mcp/desnap-models.sh            # report
tools/mcp/desnap-models.sh --apply    # migrate and clean up
```

Then pin the path so `XDG_DATA_HOME` cannot move it again — with darktable
**closed**, since it rewrites `darktablerc` on exit — in every profile you
use (`~/.config/darktable-mcp-dev/darktablerc` for the MCP profile):

```text
plugins/ai/models_path=/home/<user>/.local/share/darktable/models
```

`dt_ai_resolve_models_path_override()` honours this ahead of the XDG
default, and expands a leading `~`.

## Checklist

Before reporting an AI failure from an MCP build tree, confirm:

- `plugins/ai/enabled=TRUE` in the profile you actually launch with
  (`--configdir` matters; the MCP profile is *not* `~/.config/darktable`)
- `build-mcp/lib/darktable/libonnxruntime.so` resolves
- `plugins/ai/models_path` points at a directory holding
  `<model-id>/config.json`, `encoder.onnx`, `decoder.onnx`
- the failure reproduces under `-d ai`, with the log line quoted
