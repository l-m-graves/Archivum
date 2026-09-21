"""The batch-ingest contract, run against the FastAPI backend of the Punchline
repository (the prototype server that Archivum replaces). The same scenarios
run against Archivum from tests/server/server_contract_test.cpp.

    PUNCHLINE_REPO=/path/to/punchline python -m pytest tests/contract -q

Needs the backend's requirements (fastapi, sqlalchemy, pydantic, httpx,
pytest). Nothing here survives the cutover: it proves the contract, it is not
a Python component of Archivum (punchline-updates.md section 10).
"""
from __future__ import annotations

import json
import os
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
SCENARIOS = json.loads((HERE / "scenarios.json").read_text(encoding="utf-8"))["scenarios"]

REPO = os.environ.get("PUNCHLINE_REPO", "")
if not REPO:
    pytest.skip("PUNCHLINE_REPO not set: the Punchline repository holds the FastAPI backend", allow_module_level=True)
BACKEND = Path(REPO) / "backend"
sys.path.insert(0, str(BACKEND))

from fastapi.testclient import TestClient  # noqa: E402
from sqlalchemy import func, select  # noqa: E402

from app.config import Settings  # noqa: E402
from app.main import create_app  # noqa: E402
from app.models import TimeEntry  # noqa: E402

TOKEN = "contract-token-0123456789abcdef"
START = datetime(2024, 3, 4, 9, 0, 0, tzinfo=timezone.utc)


def uid(n: int) -> str:
    return f"00000000-0000-4000-8000-{n:012d}"


def wire_entry(abstract: dict) -> dict:
    """The prototype's shape: a completed shift with a self-asserted employee id."""
    minutes = abstract.get("minutes", 480)
    e = {
        "uuid": uid(abstract["id"]),
        "employee_id": "E0001",
        "employee_name": "Mapped Employee",
        "company": "ACME",
        "cost_center": "4400",
        "clock_in": START.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "clock_out": (START + timedelta(minutes=minutes)).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "minutes": minutes,
        "note": "",
    }
    invalid = abstract.get("invalid")
    if invalid == "missing_required_field":
        e["cost_center"] = ""
    elif invalid == "uuid":
        e["uuid"] = "not-a-uuid"
    return e


@pytest.fixture
def server(tmp_path: Path):
    settings = Settings(
        database_url=f"sqlite:///{tmp_path / 'contract.db'}",
        api_token=TOKEN,
        max_batch_entries=500,
        max_shift_minutes=1440,
        fallback_supervisor_email="",
        smtp_host="",
        smtp_port=587,
        smtp_user="",
        smtp_password="",
        smtp_from="timeclock@test.example",
        smtp_starttls=False,
        mail_outbox_dir=str(tmp_path / "outbox"),
        digest_enabled=False,
        digest_hour=6,
        digest_minute=0,
        digest_timezone="UTC",
    )
    app = create_app(settings)
    with TestClient(app) as client:
        yield client, app.state.session_factory


def stored(session_factory) -> int:
    with session_factory() as session:
        return session.scalar(select(func.count()).select_from(TimeEntry)) or 0


@pytest.mark.parametrize("scenario", SCENARIOS, ids=[s["name"] for s in SCENARIOS])
def test_scenario(server, scenario):
    client, session_factory = server
    for step in scenario["steps"]:
        payload = {
            "client_version": "1.0.0",
            "device_id": "CONTRACT-DEVICE",
            "submitted_at": "2024-03-04T21:40:11Z",
            "entries": [wire_entry(e) for e in step["entries"]],
        }
        headers = {} if step.get("auth") == "none" else {"Authorization": f"Bearer {TOKEN}"}
        response = client.post("/api/v1/timesheets", json=payload, headers=headers)
        expect = dict(step.get("expect", {}))
        expect.update(step.get("expect_by_server", {}).get("fastapi", {}))
        if "http_status" in expect:
            assert response.status_code == expect["http_status"]
        else:
            assert response.status_code == 200, response.text
            body = response.json()
            if "accepted" in expect:
                assert body["accepted"] == [uid(n) for n in expect["accepted"]]
            if "accepted_any_of" in expect:
                assert body["accepted"] in [[uid(n) for n in option] for option in expect["accepted_any_of"]]
            if "rejected" in expect:
                assert [r["uuid"] for r in body["rejected"]] == [uid(n) for n in expect["rejected"]]
            if "rejected_count" in expect:
                assert len(body["rejected"]) == expect["rejected_count"]
            if "rejected_reason_mentions" in expect:
                reason = body["rejected"][0]["reason"]
                assert any(word in reason for word in expect["rejected_reason_mentions"]), reason
            if expect.get("flagged"):
                # The prototype flags by rejecting with a reason; Archivum by accepting and opening an exception.
                assert body["rejected"] and "exceeds" in body["rejected"][0]["reason"]
        if "stored" in expect:
            assert stored(session_factory) == expect["stored"]
        if "stored_any_of" in expect:
            assert stored(session_factory) in expect["stored_any_of"]
