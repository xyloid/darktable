"""Tests for server.py: the four MCP tools, exercised in-process against
the fake darktable peer (no stdio transport, no darktable, no `mcp`
subprocess) by calling `FastMCP.call_tool()` directly -- the same call the
SDK's stdio dispatcher makes once a real MCP client is attached.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from mcp.server.fastmcp.exceptions import ToolError

from conftest import load_fixture
from darktable_mcp.server import build_server


def _write_record(config_dir: Path, server) -> Path:
    mcp_dir = config_dir / "mcp"
    mcp_dir.mkdir(parents=True, exist_ok=True)
    record_path = mcp_dir / "session-12345.json"
    record_path.write_text(
        json.dumps(
            {
                "protocol": "org.darktable.remote-edit",
                "protocol_version": 1,
                "darktable_version": "5.x",
                "pid": os.getpid(),  # this test process: guaranteed alive
                "host": "127.0.0.1",
                "port": server.port,
                "token": server.token,
                "started_at": "2026-07-05T15:04:05Z",
            }
        )
    )
    return record_path


async def _built_server(tmp_path, server):
    _write_record(tmp_path, server)
    return build_server(config_dir=str(tmp_path), connect_timeout=1.0, request_timeout=1.0)


async def call_tool_json(app, name: str, arguments: dict) -> object:
    """Calls a tool and returns its structured result.

    Every tool here is annotated `-> dict[str, Any]`, so FastMCP derives
    an output schema and `call_tool()` returns `(content, structured)`:
    a one-item text-content list (JSON, for clients that only render
    text) plus the structured dict itself. Both carry the same data;
    this helper checks that invariant once and returns the structured
    form, which is simpler for tests to assert on.
    """
    content, structured = await app.call_tool(name, arguments)
    assert len(content) == 1
    assert json.loads(content[0].text) == structured
    return structured


async def test_get_current_image_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_state_response_darkroom.json")
    server.handle_from_fixture("get_state", "get_state_response_darkroom.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_current_image", {})

    assert result == fixture["result"]


async def test_list_modules_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("list_modules_response.json")
    server.handle_from_fixture("list_modules", "list_modules_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "list_modules", {})

    assert result == fixture["result"]


async def test_list_modules_not_in_darkroom_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture("list_modules", "list_modules_error_not_in_darkroom_response.json")

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("list_modules", {})

    message = str(excinfo.value)
    assert "not_in_darkroom" in message
    assert "darkroom" in message  # the actionable hint text


async def test_get_module_schema_returns_fields_verbatim(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_module_schema_response.json")
    server.handle_from_fixture("get_module_schema", "get_module_schema_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_module_schema", {"module": "exposure"})

    assert result == fixture["result"]
    assert result["fields"] == fixture["result"]["fields"]


async def test_get_module_schema_unknown_module_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture(
        "get_module_schema", "get_module_schema_error_unknown_module_response.json"
    )

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("get_module_schema", {"module": "nonexistent_op"})

    message = str(excinfo.value)
    assert "unknown_module" in message
    assert "list_modules" in message  # the actionable hint text


async def test_get_module_params_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_module_params_response.json")
    server.handle_from_fixture("get_module_params", "get_module_params_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_module_params", {"module": "exposure", "instance": 0})

    assert result == fixture["result"]


async def test_get_module_params_defaults_instance_to_zero(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen_params = {}

    def handler(params):
        seen_params.update(params)
        return {"module": params["module"], "instance": params["instance"], "values": {}}

    server.handle("get_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool("get_module_params", {"module": "exposure"})

    assert seen_params == {"module": "exposure", "instance": 0}


async def test_list_tools_exposes_exactly_the_plan_tool_names(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    app = await _built_server(tmp_path, server)

    tools = await app.list_tools()
    names = {tool.name for tool in tools}

    assert names == {
        "get_current_image",
        "list_modules",
        "get_module_schema",
        "get_module_params",
    }


async def test_no_discovery_record_surfaces_discovery_error(tmp_path):
    app = build_server(config_dir=str(tmp_path))

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("get_current_image", {})

    assert "no live darktable discovery record" in str(excinfo.value)
