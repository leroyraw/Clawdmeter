#!/usr/bin/env python3
"""Unit tests for the daemon's HTTP delivery of usage payloads to the board.

Uses a mock aiohttp server (pytest-aiohttp's aiohttp_server fixture) to assert
the POSTed body, plus an unreachable-host case to assert graceful False.

Run: python -m pytest daemon/tests/test_http_delivery.py -x -q
"""
import pytest
from aiohttp import web

from daemon.http_delivery import deliver


async def _handler(request):
    request.app["seen"]["body"] = await request.json()
    return web.Response(text="ok")


@pytest.mark.asyncio
async def test_deliver_posts_payload_and_returns_true(aiohttp_server):
    app = web.Application()
    app["seen"] = {}
    app.router.add_post("/usage", _handler)
    srv = await aiohttp_server(app)

    ok = await deliver({"s": 42, "w": 13}, host=f"{srv.host}:{srv.port}")

    assert ok is True
    assert app["seen"]["body"] == {"s": 42, "w": 13}


@pytest.mark.asyncio
async def test_deliver_returns_false_when_unreachable():
    ok = await deliver({"s": 1, "w": 1}, host="127.0.0.1:1", timeout=0.5)
    assert ok is False
