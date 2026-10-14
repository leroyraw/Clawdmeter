#!/usr/bin/env python3
"""HTTP delivery of the usage payload to the board (WiFi transport).

Replaces BLE notify delivery: the daemon POSTs the same JSON payload it used
to write to the BLE RX characteristic to the board's local HTTP server
instead. Errors, timeouts, and non-200 responses are all treated as delivery
failure (return False) so the caller can retry on its own schedule.
"""
import aiohttp


async def deliver(payload: dict, host: str = "clawdmeter.local",
                   timeout: float = 3.0) -> bool:
    """POST payload as JSON to http://{host}/usage. Returns True on HTTP 200."""
    url = f"http://{host}/usage"
    try:
        client_timeout = aiohttp.ClientTimeout(total=timeout)
        async with aiohttp.ClientSession(timeout=client_timeout) as session:
            async with session.post(url, json=payload) as resp:
                return resp.status == 200
    except Exception:
        return False
