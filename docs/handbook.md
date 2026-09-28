# Archivum IT handbook

The operator's document: what runs where, how it is upgraded, backed up,
restored and rolled back, and what each of those does and does not
preserve. Stage 11 completes it (installer, container, monthly restore
procedure, revocation-latency bound). The sections below are recorded as
the rulings that fix them land, so that nothing an operator has to know
waits for the stage.

## Cutover and rollback (Punchline)

Parallel running is phased by device: pilot devices run the rewritten
client against Archivum, every other device runs the old client against
the FastAPI server, and no punch is written to both stores. A pilot
device is rolled back by revoking its Archivum credential, reinstalling
the old client, and replaying that device's period export into the old
server:

```
archivum rollback-export --db <path> --period <id> --to <file> [--released-only]
```

The file holds the period's closed shifts as the batches the old server
ingests on `POST /api/v1/timesheets`, through its own uuid-idempotent
path, so replaying it twice stores nothing twice
(`docs/punchline-module.md`, "Rollback").

**What rollback preserves.** Hours: every closed shift, with its in and
out instants, its minutes, its note, and the employee's routing (company
and cost centre) as the old server expects them.

**What rollback cannot preserve.** The old store has no schema for
approval state, for exception records, or for device attestation.
Nothing about who approved a period and when, which exceptions were
opened and how each was resolved, or whether a punch came from an
enrolled device survives the replay; the old server sees completed
entries and nothing else, and it ignores the `archivum_state` key the
export carries for the operator's eyes. **Rollback preserves hours, not
the approval trail.** The trail stays in Archivum's store and its
archived logs and backups, which are therefore retained for the whole
rollback window and beyond it for as long as the period's audit report
may be asked for; a rollback plan states where they are and for how
long, and never claims they were carried across. Open shifts (someone
clocked in at the moment of the export) are skipped and counted, and are
re-entered by hand in the old client.
