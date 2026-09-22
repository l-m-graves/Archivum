"""The rollback gate (Stage 6 rulings, item 3c): Archivum's rollback export,
replayed into the old FastAPI server through its own ingest endpoint, is
served back by the old server. Run against the checked-out Punchline
repository:

    PUNCHLINE_REPO=/path/to/punchline ARCHIVUM_ROLLBACK_EXPORT=/path/to/rb.json \\
        python -m pytest tests/contract/test_rollback.py -q

With ARCHIVUM_ROLLBACK_EXPORT unset the test uses the fixture export in
this directory (rollback-fixture.json, produced by `archivum
rollback-export` from the cli_test store), so the shape is proven on every
run; against real pilot data the operator points it at the real export.
Rollback is not rehearsed until this has run against the real export.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO = os.environ.get("PUNCHLINE_REPO", "")
if not REPO:
    pytest.skip("PUNCHLINE_REPO not set: the Punchline repository holds the FastAPI backend", allow_module_level=True)
sys.path.insert(0, str(Path(REPO) / "backend"))

from fastapi.testclient import TestClient  # noqa: E402

from app.config import Settings  # noqa: E402
from app.main import create_app  # noqa: E402

TOKEN = "rollback-token-0123456789abcdef"
EXPORT = Path(os.environ.get("ARCHIVUM_ROLLBACK_EXPORT", HERE / "rollback-fixture.json"))


@pytest.fixture
def old_server(tmp_path: Path):
    settings = Settings(
        database_url=f"sqlite:///{tmp_path / 'old-store.db'}",
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
        yield client


def test_export_replays_into_the_old_server_and_is_served_back(old_server):
    batches = json.loads(EXPORT.read_text(encoding="utf-8"))
    assert isinstance(batches, list) and batches, "the export is a non-empty array of batches"
    expected = {}
    for batch in batches:
        # The old server ignores keys it does not know (archivum_state); everything else must be accepted.
        response = old_server.post("/api/v1/timesheets", json=batch, headers={"Authorization": f"Bearer {TOKEN}"})
        assert response.status_code == 200, response.text
        body = response.json()
        assert body["rejected"] == [], body
        assert body["accepted"] == [e["uuid"] for e in batch["entries"]]
        for e in batch["entries"]:
            expected[e["uuid"]] = e
        # Replay is a no-op: the same batch again is accepted again and stored once.
        again = old_server.post("/api/v1/timesheets", json=batch, headers={"Authorization": f"Bearer {TOKEN}"})
        assert again.status_code == 200 and again.json()["accepted"] == body["accepted"]
    served = old_server.get("/api/v1/admin/entries?limit=1000", headers={"Authorization": f"Bearer {TOKEN}"}).json()
    assert {e["uuid"] for e in served} == set(expected)
    for e in served:
        want = expected[e["uuid"]]
        assert e["employee_id"] == want["employee_id"]
        assert e["company"] == want["company"] and e["cost_center"] == want["cost_center"]
        assert e["clock_in"] == want["clock_in"] and e["clock_out"] == want["clock_out"]
        assert e["minutes"] == want["minutes"]
        assert e["note"] == want["note"]
