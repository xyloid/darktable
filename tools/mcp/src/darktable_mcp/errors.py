"""Error types shared by the discovery and protocol layers.

This module has no dependency on the MCP SDK (see the package README):
`server.py` is the only file allowed to import `mcp`. Everything here is
plain Python so `discovery.py` and `protocol.py` stay usable outside an MCP
context (e.g. from a plain script or a different agent framework).

Wire error codes (the private darktable remote-edit protocol) are preserved
verbatim on `ProtocolError.code` -- callers that want the raw code for
programmatic handling (e.g. retrying on `busy`) still can. `hint()` adds a
short, actionable sentence for the *model-facing* MCP error text; it is
deliberately not baked into `message`, so callers that only want the wire
message can still get it via `.message`.
"""

from __future__ import annotations

from typing import Any


class DarktableMCPError(Exception):
    """Base class for every error the sidecar can raise to a caller."""


class ProtocolError(DarktableMCPError):
    """A well-formed `{"ok": false, "error": {...}}` response from darktable.

    Preserves the private wire error code (`code`) and message verbatim;
    `hint()` looks up an MCP-facing suggestion for that code, if one is
    known, without altering the underlying data.
    """

    def __init__(
        self,
        code: str,
        message: str,
        details: Any = None,
        retryable: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details
        self.retryable = retryable

    def hint(self) -> str | None:
        return _HINTS.get(self.code)

    def __str__(self) -> str:  # pragma: no cover - trivial formatting
        hint = self.hint()
        base = f"{self.code}: {self.message}"
        return f"{base} ({hint})" if hint else base


class TransportError(DarktableMCPError):
    """A connection-level failure: refused, timed out, reset, malformed
    framing, or any other problem below the wire's request/response
    envelope. Distinct from `ProtocolError`, which means darktable
    answered but declined the request.
    """


class RequestOutcomeUnknown(TransportError):
    """A request was sent (or may have been sent) and the connection was
    lost, or the deadline expired, before a response arrived.

    darktable may or may not have processed the request -- there is no way
    to tell from this side of a plain TCP socket. Callers MUST NOT treat
    this as either success or failure; in particular, `protocol.py` never
    auto-retries the request that raised this. For a read-only call this
    is normally safe to re-issue by hand (reading twice has no side
    effect); a future mutation-capable version of this client will need a
    stronger story (idempotency keys, or read-back-and-compare) before it
    can retry safely on the caller's behalf.
    """


class DiscoveryError(DarktableMCPError):
    """No usable discovery record could be found or the requested one is
    invalid/stale."""


# ---------------------------------------------------------------------------
# MCP-facing hints for private wire error codes.
#
# Keyed by the exact `error.code` string from the wire (see
# docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md).
# Every code from that reference's error table is covered here, including
# ones no current tool can trigger yet (e.g. `revision_conflict`), so this
# table does not need to change when later plan steps add mutating tools.
# ---------------------------------------------------------------------------
_HINTS: dict[str, str] = {
    "not_in_darkroom": "open an image in the darkroom view and try again",
    "no_image_open": "open an image in the darkroom before calling this tool",
    "unknown_module": "call list_modules to see the operation names valid for this image",
    "unknown_instance": "call list_modules to see the live instance numbers for this module",
    "unknown_field": "call get_module_schema to see the valid field names for this module",
    "unsupported_field": "this field is read-only from the sidecar; check get_module_schema's writable flag",
    "invalid_value": "the request violated the field's type, range, or the method's parameter shape",
    "instance_not_supported": "this module does not support multiple instances",
    "revision_conflict": "the darkroom history changed since the revision you read; re-read state and retry",
    "preview_failed": "preview rendering failed; this may be transient, retry once",
    "scope_failed": "scope computation failed; this may be transient, retry once",
    "unauthorized": "the session token was rejected; re-run discovery, a new darktable session issues a new token",
    "busy": "darktable is processing too many requests right now; wait briefly and retry",
    "request_too_large": "the request or response exceeded the 16 MiB frame limit; reduce the requested size",
    "internal": "darktable hit an internal error; check the darktable log",
}
