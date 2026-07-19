# Darktable MCP Codex Installation Design

**Date:** 2026-07-19

**Status:** Approved for implementation

## Goal

Make the existing Ubuntu installation guide equally usable by Claude Code and
Codex users. A reader should be able to build the branch, install the Python
sidecar, register it with either MCP client, confirm the tools are connected,
and remove the registration without consulting another document.

## Scope

Rename
`tools/mcp/docs/ubuntu-claude-code-setup.md` to
`tools/mcp/docs/ubuntu-mcp-client-setup.md` and generalize that guide in place.
The darktable build, sidecar installation, isolated-profile, discovery,
operation, test, and security instructions remain shared. Only the client
registration, client verification, everyday-use wording, troubleshooting, and
removal instructions branch by client.

This change does not alter the MCP sidecar, darktable's remote-control
protocol, packaging, tests, or Codex installation itself. It does not rewrite
historical implementation plans that mention the old guide path.

## Document Structure

The guide title and introduction will name both Claude Code and Codex. The
process diagram will use `Claude Code or Codex` as the MCP host, and shared
prose will use `MCP client` where the statement applies to both.

Steps 1 through 7 remain a single shared setup path. Step 8 becomes a shared
registration step with parallel subsections:

- Claude Code keeps the existing `claude mcp add --scope local` command and
  its local, user, and project scope explanation.
- Codex uses the stdio registration command below, which writes to
  `~/.codex/config.toml` by default and is shared by local Codex clients on the
  same host. Unlike Claude Code, `codex mcp add` has no `--scope` flag.
  Project-scoped Codex MCP configuration is possible through a trusted
  repository's `.codex/config.toml`, but it is not required by this guide.

```sh
codex mcp add darktable -- \
  "$MCP_BIN" \
  --config-dir "$DT_CONFIG" \
  --request-timeout 30

codex mcp get darktable
codex mcp list
```

Step 9 explains how to launch the chosen client (`claude` or `codex`) and use
`/mcp` to confirm the `darktable` server is connected. The existing example
prompts and image-content guidance apply to both clients and remain shared.

The everyday-startup section will refer to the selected MCP client instead of
Claude Code alone. Troubleshooting will show the matching `mcp get` command
for each client and tell readers to start a new client session after replacing
or moving the sidecar environment. The removal section will retain the Claude
Code command and add:

```sh
codex mcp remove darktable
```

## Accuracy and Safety

All commands continue to use the absolute `MCP_BIN` and isolated `DT_CONFIG`
paths established earlier in the guide. The Codex command passes the sidecar
arguments after `--`, matching the installed Codex CLI's stdio-server syntax.
Neither client configuration stores the short-lived darktable session token;
the sidecar continues to discover it at runtime.

Client-neutral wording must not erase genuine differences: Claude Code scope
flags remain documented only for Claude Code, while Codex configuration
behavior remains documented only for Codex. The guide continues to recommend
an isolated darktable profile and a 30-second request timeout.

## Verification

Verification is documentation-focused:

1. Run `git diff --check` to catch whitespace errors.
2. Confirm the renamed guide has no headings or shared prose that incorrectly
   treats Claude Code as the only supported client.
3. Confirm every client-specific lifecycle has matching commands: add, get or
   list, interactive `/mcp` verification, and remove.
4. Compare the documented Codex command with `codex mcp add --help` and the
   Claude Code command with the existing known-good guide syntax.
5. Confirm no non-documentation files changed.

The pre-change MCP unit baseline is 134 passed and 57 deselected. Because the
implementation changes documentation only, those tests do not need to be
rerun unless a non-documentation file changes.
