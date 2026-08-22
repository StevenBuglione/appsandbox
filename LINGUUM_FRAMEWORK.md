# Linguum Framework Integration

This repository is an upstream-derived implementation dependency of the Linguum
Firefox Application Framework. The canonical framework handoff, architecture,
source lock, schemas, and milestone reports live in
[`StevenBuglione/linguum-runtime`](https://github.com/StevenBuglione/linguum-runtime)
at:

```text
docs/framework-handoff/v1/
SOURCE_LOCK.json
reports/
```

Do not copy or independently edit those governance files here. Framework work in
this repository must cite the exact runtime checkpoint and the active root source
lock used for the work package.

## M0 qualified source

The owner-qualified DNS-fixed App Sandbox source for M0 is:

```text
repository: StevenBuglione/appsandbox
framework base: 8fddfa4e799e99df10dbdf537f3fd0f1e159347d
host baseline: 8fddfa4e799e99df10dbdf537f3fd0f1e159347d
guest baseline: 8fddfa4e799e99df10dbdf537f3fd0f1e159347d
runtime baseline: ada5f4867b2a77e55d017e66fdd6ac7db0c33084
```

The original handoff refs remain preserved in the runtime archive and in the
immutable `poc-plan1-working-ea48335` tag. The DNS-qualified preservation tag is
`poc-plan1-working-8fddfa4`; neither tag may be moved.

M0 installs governance only. This branch does not change App Sandbox product,
agent, renderer, protocol, or VM behavior.
