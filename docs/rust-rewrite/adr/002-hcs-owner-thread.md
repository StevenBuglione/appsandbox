# ADR 002: HCS owner thread

Status: accepted.

One dedicated Windows worker will create, mutate, and destroy HCS resources. Callers exchange typed
commands/results and never receive raw handles. This makes affinity, cancellation, shutdown, and RAII
ordering explicit without declaring native handles `Send` or `Sync`.
