# Decision 0002: credential for Excel and XMLA

Status: decided (rulings v3, section 4).

## Problem

Excel's Analysis Services provider authenticates to an XMLA endpoint with
HTTP basic or Windows integrated authentication, not OIDC bearer tokens.
Excel also saves connection strings inside workbooks, and workbooks get
emailed, so a saved credential leaves the building attached to a file.

## Decision

A **per-user access token**, presented as the basic-auth password over
TLS, with these properties, all of them enforced server-side:

- Read-only, and scoped to the analytical store only. No path to
  `operational.db`. This is what makes a leak survivable.
- Expiring, configurable lifetime, default 90 days.
- Individually revocable; listed in the GUI with last-used timestamp so a
  user can see and revoke their own.
- Displayed once at generation, stored only as a hash, never retrievable.
- Audited on issue, on every use, and on revocation.
- Rate-limited per token.
- The GUI says "do not save this credential in a workbook" at generation,
  and the handbook repeats it.

## Alternative considered: Windows integrated authentication

If analyst machines are domain-joined, SPNEGO with Kerberos would avoid the
token-in-workbook problem entirely, because nothing is saved. It was not
taken because it requires a Kerberos server-side implementation (SPNEGO
negotiation, ticket validation, keytab management on Linux) that is a large
piece of work on its own, and because the analytical-only, expiring,
audited token bounds the leak risk to an acceptable level. Revisit if the
domain-join answer is yes and a leak actually occurs.
