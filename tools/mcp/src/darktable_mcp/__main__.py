"""Entry point: `python -m darktable_mcp` / the `darktable-mcp` console
script. Runs the MCP server over stdio, per the plan's acceptance gate
("an MCP inspector/client can invoke all four tools against a real
darktable instance").
"""

from __future__ import annotations

import argparse
import sys

from .server import build_server


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="darktable-mcp",
        description=(
            "Read-only MCP sidecar for darktable's remote-edit protocol. "
            "Speaks stdio MCP to the client and framed JSON/TCP to darktable."
        ),
    )
    parser.add_argument(
        "--discovery-path",
        metavar="PATH",
        help="use this exact session-<pid>.json discovery record instead of searching",
    )
    parser.add_argument(
        "--pid",
        type=int,
        metavar="PID",
        help="select the discovery record for this darktable process id",
    )
    parser.add_argument(
        "--config-dir",
        metavar="DIR",
        help="darktable config directory to search for discovery records "
        "(default: the platform default, matching darktable's own --configdir default)",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=5.0,
        metavar="SECONDS",
        help="timeout for the initial TCP connect + hello handshake (default: 5.0)",
    )
    parser.add_argument(
        "--request-timeout",
        type=float,
        default=10.0,
        metavar="SECONDS",
        help="per-request timeout once connected (default: 10.0)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv if argv is not None else sys.argv[1:])
    app = build_server(
        discovery_path=args.discovery_path,
        pid=args.pid,
        config_dir=args.config_dir,
        connect_timeout=args.connect_timeout,
        request_timeout=args.request_timeout,
    )
    app.run(transport="stdio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
