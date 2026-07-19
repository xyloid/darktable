# Darktable MCP Codex Installation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generalize the Ubuntu darktable MCP installation guide so it gives complete, accurate setup instructions for both Claude Code and Codex.

**Architecture:** Keep one shared end-to-end guide because the darktable build, Python sidecar, isolated profile, discovery, tests, and security model are identical for both clients. Rename the guide to a client-neutral path, branch only the registration and launch commands, then return to shared operation and troubleshooting prose.

**Tech Stack:** Markdown, POSIX shell examples, Claude Code CLI, Codex CLI

## Global Constraints

- Rename `tools/mcp/docs/ubuntu-claude-code-setup.md` to `tools/mcp/docs/ubuntu-mcp-client-setup.md`.
- The guide must name both Claude Code and Codex in its title, introduction, and process layout.
- Steps 1 through 7 remain a shared setup path; do not duplicate the darktable build or Python sidecar installation.
- Keep the existing Claude Code `--scope local` command and its user/project scope explanation.
- Document Codex stdio registration with `codex mcp add darktable -- "$MCP_BIN" --config-dir "$DT_CONFIG" --request-timeout 30`.
- State that `codex mcp add` writes to `~/.codex/config.toml` by default, that local Codex clients on the same host share that configuration, and that the command has no `--scope` flag.
- Keep project-scoped `.codex/config.toml` as an optional note for trusted repositories; do not require or create that file.
- Document add, get/list, interactive `/mcp` verification, and removal for both clients.
- Continue using the absolute `MCP_BIN` and isolated `DT_CONFIG` paths established by the guide.
- Do not store or suggest copying darktable's short-lived session token into either client configuration.
- Do not alter MCP implementation code, packaging, tests, or historical implementation plans that mention the old guide path.
- The pre-change MCP baseline is 134 passed and 57 deselected; rerun it only if a non-documentation file changes.

---

### Task 1: Generalize the Ubuntu MCP client guide

**Files:**
- Rename: `tools/mcp/docs/ubuntu-claude-code-setup.md` to `tools/mcp/docs/ubuntu-mcp-client-setup.md`
- Reference: `docs/superpowers/specs/2026-07-19-darktable-mcp-codex-installation-design.md`
- Verify: `tools/mcp/docs/ubuntu-mcp-client-setup.md`

**Interfaces:**
- Consumes: The existing `REPO`, `MCP_BIN`, `DT_CONFIG`, and `DARKTABLE_BIN` shell variables defined by the guide; the sidecar CLI arguments `--config-dir` and `--request-timeout`.
- Produces: One client-neutral Ubuntu guide with complete Claude Code and Codex MCP registration lifecycles.

- [ ] **Step 1: Rename the guide and generalize the shared setup language**

Use `apply_patch` to move the file to
`tools/mcp/docs/ubuntu-mcp-client-setup.md` and make these exact shared-copy
changes:

```diff
-# Build and use darktable MCP with Claude Code on Ubuntu
+# Build and use darktable MCP with Claude Code or Codex on Ubuntu

-sidecar, and registers that sidecar with Claude Code.
+sidecar, and registers that sidecar with Claude Code or Codex.

-Claude Code
+Claude Code or Codex

-Claude Code starts the sidecar. You start darktable separately and keep it
+Your MCP client starts the sidecar. You start darktable separately and keep it

-Use an absolute path for `REPO`. Later Claude Code configuration stores the
+Use an absolute path for `REPO`. Later MCP client configuration stores the

-stdio MCP server; Claude Code launches it and communicates through its
+stdio MCP server; your MCP client launches it and communicates through its

-## 7. Verify discovery before configuring Claude Code
+## 7. Verify discovery before configuring an MCP client
```

Expected: the file exists only at the new path and shared setup steps no longer
assume Claude Code is the sole MCP host.

- [ ] **Step 2: Replace registration section 8 with parallel client instructions**

Replace the existing section from `## 8.` through the line immediately before
`## 9.` with this exact Markdown:

````markdown
## 8. Register the sidecar with an MCP client

Choose the client you use. Both commands register the same sidecar with the
same isolated darktable profile and 30-second request timeout. The longer
timeout gives RAW preview rendering room to finish.

### Claude Code

Run the registration command from the project in which you use Claude Code.
Local scope keeps this machine-specific absolute path out of the repository:

```sh
claude mcp add --scope local darktable -- \
  "$MCP_BIN" \
  --config-dir "$DT_CONFIG" \
  --request-timeout 30
```

Verify the saved entry and connection status:

```sh
claude mcp get darktable
claude mcp list
```

Use `--scope user` instead of `--scope local` if the same sidecar should be
available from every Claude Code project. Use `--scope project` only if you
intend to share and review a `.mcp.json`; it will contain checkout-specific
absolute paths unless you deliberately make the command portable.

If the sidecar was registered previously, remove it with
`claude mcp remove darktable`, then rerun `claude mcp add`.

### Codex

Register the sidecar with Codex:

```sh
codex mcp add darktable -- \
  "$MCP_BIN" \
  --config-dir "$DT_CONFIG" \
  --request-timeout 30
```

Verify the saved entry:

```sh
codex mcp get darktable
codex mcp list
```

`codex mcp add` writes to `~/.codex/config.toml` by default. Local Codex
clients on the same host, including the CLI and IDE extension, share that
configuration. The command has no `--scope` flag. A trusted repository may
instead define a project-scoped server in `.codex/config.toml`, but this
guide does not require or create project configuration.

If the sidecar was registered previously, remove it with
`codex mcp remove darktable`, then rerun `codex mcp add`.
````

Expected: Claude Code retains its scope guidance, while Codex has exact
add/get/list/remove commands and an accurate configuration-scope explanation.

- [ ] **Step 3: Generalize tool verification and operation language**

Replace section 9's heading and launch introduction with:

````markdown
## 9. Verify the tools from your MCP client

Start the client you registered above:

### Claude Code

```sh
claude
```

### Codex

```sh
codex
```

Run `/mcp` inside the client. Confirm that `darktable` is connected and
allow the tool when prompted. Useful first requests are:
````

Keep the four existing example prompts unchanged. After them, change the image
paragraph to:

```markdown
The MCP client receives previews and rendered scopes as native MCP image
content, not base64 text. The preview is a developed sRGB JPEG, not the
original RAW payload. Keep `max_px` at 1024 or lower in image-heavy
conversations to reduce context usage.
```

In section 10, make this exact one-line change:

```diff
-The sidecar currently exposes these Claude Code tools:
+The sidecar currently exposes these MCP tools:
```

Expected: both clients have an explicit launch command, while prompts,
operations, and image behavior remain shared.

- [ ] **Step 4: Generalize everyday use, troubleshooting, and removal**

Replace section 12's numbered items 3 through 6 and its final paragraph with:

```markdown
3. Start the MCP client where the sidecar was registered.
4. Check `/mcp` if darktable tools are missing.
5. Ask the client to inspect state before making any mutating call.
6. Close darktable normally when finished so its discovery record is removed.

The sidecar discovers darktable lazily on the first tool call. It is therefore
fine to start your MCP client before darktable, but the first darktable tool
call will fail until a live discovery record exists.
```

Replace the client-specific troubleshooting subsection with:

````markdown
### The MCP client does not show the tools

Inspect the saved entry with the command for your client:

```sh
claude mcp get darktable
# or
codex mcp get darktable
```

Then inspect `/mcp` inside the client. Recreate the registration if the
checkout or virtual environment moved. Start a new client session after
replacing the sidecar environment.
````

Change the authentication paragraph from `Claude configuration` to
`MCP client configuration`. Replace section 14's removal prose and command
with:

````markdown
Stop exposing remote control by closing darktable, or set
`security/enable_remote_control=FALSE` and restart it. Remove the matching
sidecar registration:

### Claude Code

```sh
claude mcp remove darktable
```

### Codex

```sh
codex mcp remove darktable
```
````

Retain the existing final paragraph about independently removing the build
tree, virtual environment, isolated config, and isolated cache.

Expected: the normal workflow and every troubleshooting statement apply to
both clients without erasing their distinct commands.

- [ ] **Step 5: Run documentation verification**

Run:

```bash
git diff --check
```

Expected: exit 0 with no output.

Run:

```bash
test ! -e tools/mcp/docs/ubuntu-claude-code-setup.md
test -f tools/mcp/docs/ubuntu-mcp-client-setup.md
```

Expected: both commands exit 0 with no output.

Run:

```bash
rg -n '^# Build and use darktable MCP with Claude Code or Codex|^### Claude Code$|^### Codex$|codex mcp (add|get|list|remove)' \
  tools/mcp/docs/ubuntu-mcp-client-setup.md
```

Expected: the title, both client subsection headings, and all four Codex MCP
lifecycle commands are present.

Run:

```bash
rg -n 'configuring Claude Code|these Claude Code tools|Claude Code does not show the tools|Claude configuration' \
  tools/mcp/docs/ubuntu-mcp-client-setup.md
```

Expected: exit 1 with no output because no stale Claude-only shared wording
remains.

Run:

```bash
codex mcp add --help
```

Expected: usage includes
`codex mcp add [OPTIONS] <NAME> (--url <URL> | -- <COMMAND>...)`.

Run:

```bash
git status --short
```

Expected: documentation files only. Before staging, Git may show the old guide
as deleted and the new guide and plan as untracked; no MCP source, packaging,
or test file is modified.

- [ ] **Step 6: Commit the documentation change**

Stage the plan and both sides of the guide rename, verify the staged patch,
then commit:

```bash
git add \
  docs/superpowers/plans/2026-07-19-darktable-mcp-codex-installation.md \
  tools/mcp/docs/ubuntu-claude-code-setup.md \
  tools/mcp/docs/ubuntu-mcp-client-setup.md
git diff --cached --check
git diff --cached --stat
git diff --cached -- \
  docs/superpowers/plans/2026-07-19-darktable-mcp-codex-installation.md \
  tools/mcp/docs/ubuntu-claude-code-setup.md \
  tools/mcp/docs/ubuntu-mcp-client-setup.md
git commit -m "docs: add Codex MCP setup instructions"
```

Expected: the staged stat and patch show the plan plus the client-neutral guide
rename with its content changes, and no non-documentation files. The commit
then succeeds.
